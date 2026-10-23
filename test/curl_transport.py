#!/usr/bin/env python3
"""Exercise the installed libmpv HTTP transport; no network access or display.

Run: python3 test/curl_transport.py --libmpv /path/to/libmpv.so
Add --benchmark for latency/per-lane/shared-cap/serialized-origin measurements.
The fixture is the real HTTP boundary. It does not mock scheduling or metrics.
"""

import argparse
import contextlib
import ctypes as C
import hashlib
import http.server
import json
import pathlib
import re
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest


class Node(C.Structure):
    pass


class NodeList(C.Structure):
    _fields_ = [("num", C.c_int), ("values", C.POINTER(Node)),
                ("keys", C.POINTER(C.c_char_p))]


class NodeValue(C.Union):
    _fields_ = [("string", C.c_char_p), ("flag", C.c_int),
                ("int64", C.c_int64), ("double", C.c_double),
                ("list", C.POINTER(NodeList)), ("pointer", C.c_void_p)]


Node._fields_ = [("u", NodeValue), ("format", C.c_int)]


class Event(C.Structure):
    _fields_ = [("id", C.c_int), ("error", C.c_int),
                ("userdata", C.c_uint64), ("data", C.c_void_p)]


class EndFile(C.Structure):
    _fields_ = [("reason", C.c_int), ("error", C.c_int)]


def decode(node):
    if node.format == 1:
        return node.u.string.decode()
    if node.format == 3:
        return bool(node.u.flag)
    if node.format == 4:
        return node.u.int64
    if node.format == 5:
        return node.u.double
    if node.format in (7, 8):
        values = node.u.list.contents
        result = [decode(values.values[i]) for i in range(values.num)]
        return dict(zip((values.keys[i].decode() for i in range(values.num)), result)) if node.format == 8 else result
    return None


def encode(value, keep):
    node = Node()
    if isinstance(value, bool):
        node.format, node.u.flag = 3, value
    elif isinstance(value, int):
        node.format, node.u.int64 = 4, value
    elif isinstance(value, float):
        node.format, node.u.double = 5, value
    elif isinstance(value, dict):
        values = (Node * len(value))(*(encode(v, keep) for v in value.values()))
        keys = (C.c_char_p * len(value))(*(k.encode() for k in value))
        entries = NodeList(len(value), values, keys)
        keep.extend((values, keys, entries))
        node.format, node.u.list = 8, C.pointer(entries)
    else:
        raise TypeError(value)
    return node


class Player:
    def __init__(self, library, **options):
        self.lib = C.CDLL(library)
        self.lib.mpv_create.restype = C.c_void_p
        signatures = {
            "mpv_set_option_string": ([C.c_void_p, C.c_char_p, C.c_char_p], C.c_int),
            "mpv_initialize": ([C.c_void_p], C.c_int),
            "mpv_command": ([C.c_void_p, C.POINTER(C.c_char_p)], C.c_int),
            "mpv_get_property": ([C.c_void_p, C.c_char_p, C.c_int, C.c_void_p], C.c_int),
            "mpv_set_property": ([C.c_void_p, C.c_char_p, C.c_int, C.c_void_p], C.c_int),
            "mpv_set_property_string": ([C.c_void_p, C.c_char_p, C.c_char_p], C.c_int),
            "mpv_free_node_contents": ([C.POINTER(Node)], None),
            "mpv_wait_event": ([C.c_void_p, C.c_double], C.POINTER(Event)),
            "mpv_terminate_destroy": ([C.c_void_p], None),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.lib, name)
            fn.argtypes, fn.restype = args, result
        self.handle = self.lib.mpv_create()
        if not self.handle:
            raise RuntimeError("mpv_create failed")
        defaults = {"config": "no", "terminal": "no", "vid": "no", "ao": "null",
                    "ao-null-untimed": "yes", "idle": "yes", "cache": "yes",
                    "demuxer-max-bytes": "64KiB", "demuxer-readahead-secs": "0.1",
                    "curl-enabled": "yes", "curl-max-request-size": "256KiB",
                    "curl-buffer-size": "1MiB", "curl-parallel-requests": "4"}
        defaults.update(options)
        try:
            for key, value in defaults.items():
                self.check(self.lib.mpv_set_option_string(self.handle, key.encode(), str(value).encode()), key)
            self.check(self.lib.mpv_initialize(self.handle), "initialize")
        except BaseException:
            self.close()
            raise

    @staticmethod
    def check(result, operation):
        if result < 0:
            raise RuntimeError(f"{operation}: libmpv error {result}")

    def close(self):
        if self.handle:
            self.lib.mpv_terminate_destroy(self.handle)
            self.handle = None

    def command(self, *args):
        encoded = (C.c_char_p * (len(args) + 1))(*(str(arg).encode() for arg in args), None)
        self.check(self.lib.mpv_command(self.handle, encoded), args[0])

    def get(self, name):
        node = Node()
        if self.lib.mpv_get_property(self.handle, name.encode(), 6, C.byref(node)) < 0:
            return None
        try:
            return decode(node)
        finally:
            self.lib.mpv_free_node_contents(C.byref(node))

    def set(self, name, value):
        self.check(self.lib.mpv_set_property_string(self.handle, name.encode(), str(value).encode()), name)

    def transport_control(self, values):
        # Returns libmpv's result: tests assert rejections as well as success.
        keep = []
        node = encode(values, keep)
        return self.lib.mpv_set_property(self.handle, b"curl-transport-control", 6, C.byref(node))

    def configure(self, epoch, **facts):
        self.check(self.transport_control({"format": 2, "op": 0, "playback-epoch": epoch,
                                           **{key.replace("_", "-"): value for key, value in facts.items()}}),
                   "configure")

    def play(self, url, loaded=None, timeout=90):
        started = time.monotonic()
        self.command("loadfile", url)
        samples = []
        positions = []
        first_progress = None
        was_loaded = False
        while time.monotonic() - started < timeout:
            event = self.lib.mpv_wait_event(self.handle, 0.025).contents
            metrics = self.get("curl-transport-metrics")
            if metrics:
                samples.append((time.monotonic() - started, metrics))
            if event.id == 8:
                was_loaded = True
                if loaded:
                    loaded(self)
            position = self.get("time-pos")
            if position is not None:
                positions.append((time.monotonic(), position))
            if first_progress is None and position is not None and position > 0:
                first_progress = time.monotonic() - started
            if event.id == 7:
                if not was_loaded:
                    raise AssertionError("transport failed before file-loaded")
                end = C.cast(event.data, C.POINTER(EndFile)).contents
                if end.reason != 0:
                    raise AssertionError(f"playback did not reach EOF: reason={end.reason}, error={end.error}")
                return {"seconds": time.monotonic() - started, "started": started,
                        "first-progress-seconds": first_progress,
                        "samples": samples, "positions": positions,
                        "metrics": self.get("curl-transport-metrics")}
        raise AssertionError("transport did not reach EOF")


class Origin:
    def __init__(self, size=8 << 20, latency=0.04, per_lane=2 << 20,
                 shared=0, serialized=False, reorder=False, fault=None,
                 cap=0, unknown_size=False, contention=False, etag=None,
                 changed=False, pause=None, fault_after=1, tls=None):
        pcm = (bytes(range(256)) * ((size + 255) // 256))[:size - size % 4]
        self.pcm = pcm
        self.payload = (b"RIFF" + struct.pack("<I", len(pcm) + 36) + b"WAVEfmt " +
                        struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16) +
                        b"data" + struct.pack("<I", len(pcm)) + pcm)
        self.latency, self.per_lane, self.shared = latency, per_lane, shared
        self.serialized, self.reorder, self.fault = serialized, reorder, fault
        self.cap, self.unknown_size = cap, unknown_size
        self.contention = contention
        # etag: strong validator to send; changed: the file is replaced after
        # the injected fault; pause: (offset, seconds) to go quiet mid-body.
        self.etag, self.changed, self.pause = etag, changed, pause
        self.fault_after = fault_after  # first request start eligible for the fault
        self.replaced = False
        self.resumed = []  # per TLS request: did the handshake resume a session
        self.fault_range = None
        self.pause_started = threading.Event()
        self.pause_window = None
        self.active = 0
        self.lock = threading.Lock()
        self.disk = threading.Lock()
        self.next_shared = 0
        self.requests = []
        self.if_ranges = []
        self.faulted = False
        origin = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
                start = int(match[1]) if match else 0
                end = min(int(match[2]) + 1 if match and match[2] else len(origin.payload), len(origin.payload))
                if start >= end:
                    self.send_response(416)
                    total = "*" if origin.unknown_size else len(origin.payload)
                    self.send_header("Content-Range", f"bytes */{total}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if origin.cap and match:
                    end = min(end, start + origin.cap)
                with origin.lock:
                    if origin.tls:
                        origin.resumed.append(self.connection.session_reused)
                    inject = bool(origin.fault and not origin.faulted and start >= origin.fault_after)
                    origin.faulted |= inject
                    if inject:
                        origin.fault_range = (start, end)
                    payload, etag = origin.representation()
                    origin.requests.append((start, end))
                    origin.if_ranges.append(self.headers.get("If-Range"))
                    origin.active += 1
                guard = origin.disk if origin.serialized else contextlib.nullcontext()
                try:
                    with guard:
                        time.sleep(origin.latency + (0.1 if origin.reorder and start == 0 else 0))
                        self.send_response(206 if match else 200)
                        self.send_header("Accept-Ranges", "bytes")
                        self.send_header("Content-Type", "audio/wav")
                        self.send_header("Content-Length", str(end - start))
                        if etag:
                            self.send_header("ETag", etag)
                        if match:
                            first = start + 1 if inject and origin.fault == "range" else start
                            total = "*" if origin.unknown_size else len(origin.payload)
                            self.send_header("Content-Range", f"bytes {first}-{end - 1}/{total}")
                        self.end_headers()
                        cut = inject and origin.fault in ("truncate", "stall")
                        stop = start + (end - start) // 2 if cut else end
                        deadline = time.monotonic()
                        for offset in range(start, stop, 16384):
                            if origin.pause and origin.pause_window is None and \
                                    offset <= origin.pause[0] < offset + 16384:
                                paused = time.monotonic()
                                origin.pause_started.set()
                                time.sleep(origin.pause[1])
                                origin.pause_window = (paused, time.monotonic())
                            block = payload[offset:min(offset + 16384, stop)]
                            if origin.shared:
                                with origin.lock:
                                    deadline_shared = max(time.monotonic(), origin.next_shared)
                                    origin.next_shared = deadline_shared + len(block) / origin.shared
                                time.sleep(max(0, deadline_shared - time.monotonic()))
                            if origin.per_lane:
                                with origin.lock:
                                    # Model an origin/client whose aggregate
                                    # capacity falls with concurrent requests.
                                    penalty = origin.active ** 2 if origin.contention else 1
                                deadline += len(block) * penalty / origin.per_lane
                                time.sleep(max(0, deadline - time.monotonic()))
                            self.wfile.write(block)
                            self.wfile.flush()
                        if stop != end and origin.fault == "stall":
                            # Go quiet with the connection open, as a server
                            # that lost interest without closing does.
                            time.sleep(STALL_SECONDS)
                        if stop != end:
                            self.close_connection = True
                            if origin.changed:
                                with origin.lock:
                                    origin.replaced = True
                except (BrokenPipeError, ConnectionResetError):
                    pass
                finally:
                    with origin.lock:
                        origin.active -= 1

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.tls = tls
        if tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(tls / "cert.pem", tls / "key.pem")
            self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def representation(self):
        if not self.replaced:
            return self.payload, self.etag
        # Same length, different samples: only validators or bytes reveal it.
        body = self.payload[:44] + bytes(b ^ 0x5a for b in self.payload[44:])
        return body, '"replaced"' if self.etag else None

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    @property
    def url(self):
        scheme = "https" if self.tls else "http"
        return f"{scheme}://127.0.0.1:{self.server.server_port}/fixture.wav"


LIBRARY = None
STALL_SECONDS = 30


class CurlTransport(unittest.TestCase):
    def test_received_wall_rate_and_ordered_pcm(self):
        # This is the primary regression: parallel bytes previously advanced the
        # numerator without advancing the primary-only time denominator.
        with Origin(reorder=True) as origin, tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "actual.pcm"
            player = Player(LIBRARY, **{"ao": "pcm", "ao-pcm-file": output,
                                       "ao-pcm-waveheader": "no", "audio-format": "s16"})
            try:
                # Keep the same assertions runnable against the old library,
                # while exercising reordered dynamic callbacks on the new one.
                if player.get("option-info/curl-initial-requests") is not None:
                    player.set("options/curl-initial-requests", "4")
                    player.set("options/curl-startup-requests", "4")
                result = player.play(origin.url)
                metrics = result["metrics"]
                wall_rate = metrics["received-bytes"] / result["seconds"]
                self.assertLessEqual(metrics["average-received-bytes-per-second"], wall_rate * 1.2)
                self.assertGreater(metrics["average-received-bytes-per-second"], wall_rate * 0.8)
                self.assertEqual(sum(lane["received-bytes"] for lane in metrics["lane-samples"]), metrics["received-bytes"])
                self.assertGreater(metrics["consumer-delivered-bytes"], len(origin.pcm))
                self.assertEqual(metrics["copy-out-bytes"], metrics["consumer-delivered-bytes"])
                self.assertGreaterEqual(metrics["ordered-available-bytes"], metrics["consumer-delivered-bytes"])
                self.assertGreaterEqual(metrics["received-bytes"], metrics["ordered-available-bytes"])
                self.assertEqual(metrics["copy-in-bytes"], metrics["ordered-available-bytes"])
                self.assertEqual(metrics["reorder-copy-bytes"], 0)
                self.assertGreater(metrics["body-callbacks"], 0)
                self.assertGreater(metrics["worker-iterations"], 0)
                self.assertGreater(metrics["perform-wall-seconds"], 0)
                self.assertGreater(metrics["poll-wall-seconds"], 0)
                self.assertLessEqual(sum(metrics[key] for key in
                    ("perform-wall-seconds", "dispatch-wall-seconds", "poll-wall-seconds")),
                    metrics["curl-thread-wall-seconds"])
                for lane in metrics["lane-samples"]:
                    expected = lane["received-bytes"] / lane["active-seconds"]
                    self.assertAlmostEqual(lane["average-bytes-per-second"], expected, delta=2)
            finally:
                player.close()
            self.assertEqual(hashlib.sha256(output.read_bytes()).digest(), hashlib.sha256(origin.pcm).digest())

    def test_pause_seek_reopen_lifetime(self):
        with Origin(size=3 << 20, latency=0.02) as origin:
            # The pause outlasts --network-timeout: a transfer held on a full
            # ring is not a stall and must resume without a retry.
            player = Player(LIBRARY, **{"pause": "yes", "curl-buffer-size": "64KiB",
                                       "network-timeout": "1"})
            observed = {}

            def pause_seek_resume(current):
                time.sleep(2.5)
                settled = current.get("curl-transport-metrics")
                time.sleep(1.25)
                idle = current.get("curl-transport-metrics")
                self.assertEqual(idle["received-bytes"], settled["received-bytes"])
                self.assertEqual(idle["current-received-bytes-per-second"], 0)
                self.assertGreater(idle["ring-wait-seconds"] -
                                   settled["ring-wait-seconds"], 1)
                self.assertGreater(idle["curl-thread-wall-seconds"],
                                   settled["curl-thread-wall-seconds"])
                if idle["curl-thread-cpu-available"]:
                    self.assertGreaterEqual(idle["curl-thread-cpu-seconds"],
                                            settled["curl-thread-cpu-seconds"])
                    self.assertLessEqual(idle["curl-thread-cpu-seconds"],
                                         idle["curl-thread-wall-seconds"])
                observed["before-seek"] = idle["received-bytes"]
                current.command("seek", "2", "absolute+exact")
                current.set("pause", "no")

            try:
                first = player.play(origin.url, loaded=pause_seek_resume)["metrics"]
                self.assertGreaterEqual(first["received-bytes"], observed["before-seek"])
                self.assertEqual(first["retry-attempts"], 0)
                self.assertGreater(first["consumer-wait-seconds"], 0)
                second = player.play(origin.url)["metrics"]
                self.assertGreater(second["received-bytes"], first["received-bytes"])
                self.assertGreater(second["stream-id"], first["stream-id"])
                for key in ("curl-thread-wall-seconds", "ring-wait-seconds",
                            "consumer-wait-seconds", "consumer-delivered-bytes", "ordered-available-bytes",
                            "copy-in-bytes", "copy-out-bytes", "body-callbacks"):
                    self.assertGreaterEqual(second[key], first[key])
            finally:
                player.close()

    def test_invalid_and_truncated_ranges_preserve_pcm(self):
        for fault in ("range", "truncate"):
            with self.subTest(fault=fault), Origin(size=2 << 20, fault=fault) as origin, tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory) / "actual.pcm"
                player = Player(LIBRARY, **{"ao": "pcm", "ao-pcm-file": output,
                                           "ao-pcm-waveheader": "no", "audio-format": "s16",
                                           "curl-adaptive": "no"})
                try:
                    metrics = player.play(origin.url)["metrics"]
                    self.assertGreater(metrics["retry-attempts"], 0)
                finally:
                    player.close()
                self.assertEqual(output.read_bytes(), origin.pcm)

    def test_published_prefix_is_read_before_its_range_completes(self):
        # The first parallel range goes quiet halfway while later ranges are
        # ready. Its received prefix must reach playback during the pause
        # instead of waiting for the whole range (or the ranges behind it).
        mib = 1 << 20
        with Origin(size=8 << 20, latency=0.01, per_lane=4 << 20, etag='"v1"',
                    pause=(mib + mib // 2, 3.0)) as origin:
            player = Player(LIBRARY, **{"curl-adaptive": "no", "curl-max-request-size": "1MiB",
                                       "curl-buffer-size": "8MiB"})
            try:
                result = player.play(origin.url)
            finally:
                player.close()
            self.assertIsNotNone(origin.pause_window)
            paused, resumed = origin.pause_window
            during = [position for at, position in result["positions"]
                      if paused + 1.0 <= at <= resumed - 0.2]
            self.assertTrue(during)
            range_start = (mib - 44) / 192000
            self.assertGreater(max(during), range_start + 1.5)
            # The consumer then waits on the paused range with later ranges
            # retained: that wait is head-of-line blocking, nothing else.
            metrics = result["metrics"]
            self.assertGreater(metrics["head-of-line-seconds"], 1.0)
            self.assertLess(metrics["head-of-line-seconds"], metrics["consumer-wait-seconds"] + 0.01)
            self.assertGreater(metrics["peak-reorder-bytes"], mib)
            self.assertGreater(metrics["head-of-line-events"], 0)
            self.assertGreaterEqual(metrics["reorder-wait-seconds"], metrics["head-of-line-seconds"])
            gaps = [sample for at, sample in result["samples"]
                    if paused + 1.0 <= result["started"] + at <= resumed - 0.2
                    and sample["reorder-bytes"] > 0]
            self.assertGreaterEqual(len(gaps), 2)
            self.assertGreater(gaps[-1]["oldest-gap-seconds"], gaps[0]["oldest-gap-seconds"])
            self.assertGreater(gaps[-1]["reorder-wait-seconds"], gaps[0]["reorder-wait-seconds"])
            for sample in gaps:
                self.assertGreaterEqual(sample["retained-bytes"], sample["readable-bytes"] + sample["reorder-bytes"])
                self.assertFalse(sample["snapshot-truncated"])
                self.assertTrue(sample["streams"])
                self.assertLessEqual(sum(row["active-requests"] for row in sample["connections"]),
                                     sample["active-requests"])
                for row in sample["connections"]:
                    self.assertGreater(row["connection-id"], 0)
                    self.assertGreater(row["socket-generation"], 0)
                    self.assertEqual(row["http-version"], 11)
                    self.assertEqual(row["transport"], 1)
            connections = [sample["physical-connections"] for _, sample in result["samples"]]
            self.assertGreaterEqual(max(connections), 2)
            self.assertLessEqual(max(connections), 4)

    def test_no_validator_withholds_incomplete_range(self):
        # An exact 206 alone cannot authorize exposing a retriable prefix.
        # Later completed ranges must not make this incomplete leading one
        # visible, but completing it must still deliver exact PCM.
        mib = 1 << 20
        # Beyond lavf's initial format probe: playback, not probing, is now
        # requesting this prefix. Lifetime counters include probe re-reads.
        with Origin(size=10 << 20, latency=0.01, per_lane=4 << 20,
                    pause=(7 * mib + mib // 2, 3.0)) as origin, tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "actual.pcm"
            player = Player(LIBRARY, **{"ao": "pcm", "ao-pcm-file": output,
                                       "ao-pcm-waveheader": "no", "audio-format": "s16",
                                       "curl-adaptive": "no", "curl-max-request-size": "1MiB",
                                       "curl-buffer-size": "8MiB"})
            try:
                result = player.play(origin.url)
            finally:
                player.close()
            self.assertIsNotNone(origin.pause_window)
            paused, resumed = origin.pause_window
            during = [position for at, position in result["positions"]
                      if paused + 1.0 <= at <= resumed - 0.2]
            self.assertTrue(during, "playback reached the incomplete leading range")
            self.assertLessEqual(max(during), (7 * mib - 44) / 192000 + 0.1)
            self.assertEqual(output.read_bytes(), origin.pcm)

    def test_seek_during_partial_publication_delivers_exact_bytes(self):
        # Seek while one range is half published and later ranges are
        # retained: the queue is torn down mid-flight, and everything played
        # after the seek must be exactly the file from the seek target on.
        mib = 1 << 20
        with Origin(size=6 << 20, latency=0.01, per_lane=4 << 20, etag='"v1"',
                    pause=(mib + mib // 2, 3.0)) as origin, tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "actual.pcm"
            player = Player(LIBRARY, **{"ao": "pcm", "ao-pcm-file": output, "ao-pcm-waveheader": "no",
                                       "audio-format": "s16", "curl-adaptive": "no",
                                       "curl-max-request-size": "1MiB", "curl-buffer-size": "8MiB"})
            target = 2.0

            def seek_during_pause():
                if origin.pause_started.wait(30):
                    time.sleep(0.5)
                    player.command("seek", target, "absolute+exact")

            seeker = threading.Thread(target=seek_during_pause, daemon=True)
            try:
                seeker.start()
                player.play(origin.url)
                seeker.join(5)
            finally:
                player.close()
            self.assertIsNotNone(origin.pause_window)
            tail = origin.pcm[int(target * 192000):]
            self.assertTrue(output.read_bytes().endswith(tail))

    def test_retry_resumes_or_verifies_published_prefix(self):
        # A strong validator permits suffix retry; without one the incomplete
        # descriptor remains hidden and is re-read in full. Every byte still
        # reaches playback exactly once.
        for etag in ('"v1"', None):
            with self.subTest(etag=etag), Origin(size=2 << 20, fault="truncate", etag=etag,
                                                 fault_after=512 << 10) as origin, \
                    tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory) / "actual.pcm"
                player = Player(LIBRARY, **{"ao": "pcm", "ao-pcm-file": output,
                                           "ao-pcm-waveheader": "no", "audio-format": "s16",
                                           "curl-adaptive": "no"})
                try:
                    player.play(origin.url)
                finally:
                    player.close()
                self.assertEqual(output.read_bytes(), origin.pcm)
                start, end = origin.fault_range
                middle = start + (end - start) // 2
                starts = [first for first, _ in origin.requests]
                if etag:
                    self.assertIn(middle, starts)
                    self.assertEqual(starts.count(start), 1)
                    retried = next(index for index, (first, _) in enumerate(origin.requests) if first == middle)
                    self.assertEqual(origin.if_ranges[retried], etag)
                else:
                    self.assertNotIn(middle, starts)
                    self.assertEqual(starts.count(start), 2)

    def test_replaced_file_is_never_spliced_into_playback(self):
        # The file changes while a published range is being retried. Whether
        # the validator or the re-read bytes reveal it, playback must stop
        # with a prefix of the original rather than mix two files.
        for etag in ('"v1"', None):
            # Fault after lavf's 5 MB format probe, which reads and seeks back.
            with self.subTest(etag=etag), Origin(size=8 << 20, fault="truncate", etag=etag,
                                                 changed=True, fault_after=7 << 20) as origin, \
                    tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory) / "actual.pcm"
                player = Player(LIBRARY, **{"ao": "pcm", "ao-pcm-file": output,
                                           "ao-pcm-waveheader": "no", "audio-format": "s16",
                                           "curl-adaptive": "no", "curl-max-retries": "1"})
                try:
                    with contextlib.suppress(AssertionError):
                        player.play(origin.url, timeout=30)
                finally:
                    player.close()
                actual = output.read_bytes() if output.exists() else b""
                self.assertTrue(origin.replaced)
                self.assertLess(len(actual), len(origin.pcm))
                self.assertEqual(actual, origin.pcm[:len(actual)])

    def test_player_reports_playback_state_to_transport(self):
        # No host control at all: the player itself tells the transport when
        # the viewer paused, and what playback actually consumes, including
        # while the cache is full and the ordinary cache timer is idle.
        with Origin(size=4 << 20, latency=0.01) as origin:
            player = Player(LIBRARY, **{"pause": "yes", "ao-null-untimed": "no"})
            observed = {}

            def paused_then_play(current):
                time.sleep(0.5)
                before = current.get("curl-transport-metrics")
                time.sleep(1.5)
                after = current.get("curl-transport-metrics")
                observed["paused"] = (before, after)
                observed["resumed-at"] = time.monotonic()
                current.set("pause", "no")

            try:
                result = player.play(origin.url, loaded=paused_then_play)
            finally:
                player.close()
            before, after = observed["paused"]
            self.assertTrue(after["playback-user-paused"])
            self.assertGreaterEqual(after["playback-state-updates"] - before["playback-state-updates"], 4)
            playing = [metrics for at, metrics in result["samples"]
                       if result["started"] + at >= observed["resumed-at"] + 2]
            self.assertTrue(playing)
            self.assertFalse(playing[-1]["playback-user-paused"])
            demand = playing[-1]["playback-demand-bits-per-second"]
            self.assertGreater(demand, 1536000 * 0.75)
            self.assertLess(demand, 1536000 * 1.25)

    def test_socket_and_host_probes_match_the_kernel(self):
        # RTT comes from the kernel's TCP state, not request timing, and host
        # receive ceilings and AES support are what the system reports.
        if not sys.platform.startswith("linux"):
            self.skipTest("compares against Linux /proc")
        with Origin(size=4 << 20, latency=0.05) as origin:
            player = Player(LIBRARY)
            try:
                result = player.play(origin.url)
            finally:
                player.close()
        live = [metrics for _, metrics in result["samples"] if metrics["open-sockets"] > 0]
        self.assertTrue(live)
        rtts = [metrics["tcp-rtt-min-ms"] for metrics in live if metrics["tcp-rtt-min-ms"] >= 0]
        self.assertTrue(rtts)
        # Loopback RTT is far below the origin's 50 ms response delay.
        self.assertLess(max(rtts), 20)
        self.assertTrue(any(metrics["max-socket-receive-buffer-bytes"] > 0 for metrics in live))
        metrics = result["metrics"]
        self.assertGreater(metrics["sockets-opened"], 0)
        rmem_max = int(pathlib.Path("/proc/sys/net/core/rmem_max").read_text())
        autotune = int(pathlib.Path("/proc/sys/net/ipv4/tcp_rmem").read_text().split()[2])
        self.assertEqual(metrics["host-rmem-max-bytes"], rmem_max)
        self.assertEqual(metrics["host-receive-autotune-max-bytes"], autotune)
        flags = re.search(r"^(?:flags|Features)\s*:(.*)$", pathlib.Path("/proc/cpuinfo").read_text(), re.M)
        if flags:
            self.assertEqual(metrics["hw-aes"], int("aes" in flags[1].split()))

    def test_transport_control_epochs_facts_and_background_traffic(self):
        # Configure installs one playback's facts under a newer epoch; stale
        # or malformed controls change nothing. Background host traffic makes
        # capacity evidence invalid while it lasts.
        with Origin(size=8 << 20, latency=0.02, per_lane=256 << 10) as origin:
            player = Player(LIBRARY, **{"ao-null-untimed": "no", "curl-initial-requests": "4",
                                       "curl-startup-requests": "4"})
            checks = {}

            def control(current):
                # 20 Mbit/s from delivered size over duration, well above the
                # WAV's measured 1.5 Mbit/s: demand follows the larger.
                current.configure(5, delivery_size_bytes=25000000, delivery_duration_seconds=10.0)
                checks["duplicate"] = current.transport_control(
                    {"format": 2, "op": 0, "playback-epoch": 5,
                     "delivery-size-bytes": 25000000, "delivery-duration-seconds": 10.0})
                checks["conflict"] = current.transport_control(
                    {"format": 2, "op": 0, "playback-epoch": 5, "delivery-bitrate": 1.0})
                checks["stale"] = current.transport_control(
                    {"format": 2, "op": 0, "playback-epoch": 4, "delivery-bitrate": 9e9})
                for bad in ({"format": 1, "op": 0, "playback-epoch": 6},
                            {"format": 2, "op": 0, "playback-epoch": 6, "media-bitrate": 1.0},
                            {"format": 2, "op": 0, "playback-epoch": 0},
                            {"format": 2, "op": 0, "playback-epoch": 6, "delivery-size-bytes": -1},
                            {"format": 2, "op": 1, "playback-epoch": 5, "delivery-bitrate": 1.0}):
                    checks.setdefault("malformed", []).append(current.transport_control(bad))
                checks["after"] = current.get("curl-transport-metrics")
                self.assertGreaterEqual(current.transport_control(
                    {"format": 2, "op": 1, "playback-epoch": 5, "background-network": True}), 0)

            try:
                result = player.play(origin.url, loaded=control)
            finally:
                player.close()
            self.assertGreaterEqual(checks["duplicate"], 0)
            self.assertLess(checks["conflict"], 0)
            self.assertGreaterEqual(checks["stale"], 0)
            self.assertTrue(all(code < 0 for code in checks["malformed"]))
            after = checks["after"]
            self.assertEqual(after["playback-epoch"], 5)
            self.assertEqual(after["stale-controls"], 1)
            self.assertEqual(after["rejected-controls"], 1)
            self.assertAlmostEqual(after["declared-bits-per-second"], 20e6, delta=1)
            self.assertAlmostEqual(after["playback-demand-bits-per-second"], 20e6, delta=1)
            metrics = result["metrics"]
            self.assertTrue(metrics["background-network"])
            # Throughout background traffic no capacity sample is accepted.
            self.assertEqual(metrics["capacity-samples"], after["capacity-samples"])

    def test_cores_share_tls_sessions(self):
        # Every playback starts a new core. A second core must resume the
        # first core's TLS session instead of a full handshake, while each
        # core keeps its own connections.
        if not shutil.which("openssl"):
            self.skipTest("openssl is required to create a test certificate")
        with tempfile.TemporaryDirectory() as directory:
            tls = pathlib.Path(directory)
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                            "ec_paramgen_curve:P-256", "-nodes", "-days", "1", "-subj", "/CN=127.0.0.1",
                            "-addext", "subjectAltName=IP:127.0.0.1", "-keyout", tls / "key.pem",
                            "-out", tls / "cert.pem"], check=True, capture_output=True)
            options = {"tls-ca-file": tls / "cert.pem", "tls-verify": "yes",
                       "curl-adaptive": "no", "curl-parallel-requests": "1"}
            with Origin(size=1 << 20, tls=tls) as origin:
                first = Player(LIBRARY, **options)
                try:
                    first.play(origin.url)
                    handshakes = len(origin.resumed)
                    second = Player(LIBRARY, **options)
                    try:
                        second.play(origin.url)
                    finally:
                        second.close()
                finally:
                    first.close()
            self.assertGreater(handshakes, 0)
            self.assertFalse(origin.resumed[0])
            self.assertGreater(len(origin.resumed), handshakes)
            self.assertTrue(origin.resumed[handshakes])

    def test_capped_and_unsized_replies_are_continued(self):
        # Servers may answer a range with less than was asked, and may not
        # know the size, leaving a 416 to mark the end.
        for origin_args in ({"cap": 100 << 10}, {"unknown_size": True}):
            with self.subTest(**origin_args), Origin(size=2 << 20, **origin_args) as origin, \
                    tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory) / "actual.pcm"
                player = Player(LIBRARY, **{"ao": "pcm", "ao-pcm-file": output,
                                           "ao-pcm-waveheader": "no", "audio-format": "s16"})
                try:
                    player.play(origin.url)
                finally:
                    player.close()
                self.assertEqual(output.read_bytes(), origin.pcm)

    def test_stalled_transfer_is_retried_after_network_timeout(self):
        with Origin(size=2 << 20, fault="stall") as origin, tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "actual.pcm"
            player = Player(LIBRARY, **{"ao": "pcm", "ao-pcm-file": output,
                                       "ao-pcm-waveheader": "no", "audio-format": "s16",
                                       "curl-adaptive": "no", "network-timeout": "1"})
            try:
                result = player.play(origin.url, timeout=STALL_SECONDS - 5)
                self.assertGreater(result["metrics"]["retry-attempts"], 0)
            finally:
                player.close()
            self.assertEqual(output.read_bytes(), origin.pcm)

    def test_adapts_per_lane_limit_and_reverts_serialization(self):
        for serialized in (False, True):
            with self.subTest(serialized=serialized), Origin(size=10 << 20, latency=0.08, per_lane=1 << 20, serialized=serialized) as origin:
                player = Player(LIBRARY, **{"curl-media-bitrate": "48000000"})
                try:
                    result = player.play(origin.url)
                    choices = [sample[1]["selected-requests"] for sample in result["samples"]]
                    self.assertGreater(max(choices), 1)
                    self.assertLessEqual(max(choices), 4)
                    if serialized:
                        self.assertEqual(result["metrics"]["selected-requests"], 1)
                    points = result["metrics"]["concurrency-samples"]
                    self.assertTrue(any(point["requests"] == 1 for point in points))
                    self.assertTrue(any(point["requests"] == 2 for point in points))
                finally:
                    player.close()

    def test_negative_scaling_downshifts_even_below_demand(self):
        # A learned high count must decay even when no count can satisfy the
        # bitrate target. The real HTTP origin is slower with more requests.
        with Origin(size=24 << 20, latency=0.02, per_lane=1 << 20,
                    contention=True) as origin:
            player = Player(LIBRARY, **{"curl-initial-requests": "4",
                                       "curl-startup-requests": "1",
                                       "curl-media-bitrate": "48000000"})
            try:
                result = player.play(origin.url)
                metrics = result["metrics"]
                points = {point["requests"]: point["capacity-bytes-per-second"]
                          for point in metrics["concurrency-samples"]}
                self.assertIn(4, points)
                self.assertIn(1, points)
                self.assertGreater(points[1], points[4] * 1.15)
                self.assertLess(points[1], 48000000 * 1.75 / 8)
                self.assertEqual(metrics["selected-requests"], 1)
            finally:
                player.close()

    def test_rejected_downshift_does_not_repeat_below_demand(self):
        # Two lanes are useful but cannot meet headroom. Once a one-lane
        # trial loses capacity, an unchanged origin must not be retried every
        # six seconds, repeatedly starving the consumer.
        with Origin(size=24 << 20, latency=0.02, per_lane=512 << 10) as origin:
            player = Player(LIBRARY, **{"curl-parallel-requests": "2",
                                       "curl-initial-requests": "2",
                                       "curl-startup-requests": "1",
                                       "curl-media-bitrate": "48000000"})
            try:
                result = player.play(origin.url)
                metrics = result["metrics"]
                points = {point["requests"]: point["capacity-bytes-per-second"]
                          for point in metrics["concurrency-samples"]}
                self.assertIn(1, points)
                self.assertIn(2, points)
                self.assertGreater(points[2], points[1] * 1.15)
                self.assertLess(points[2], 48000000 * 1.75 / 8)
                transitions = []
                for elapsed, sample in result["samples"]:
                    choice = sample["selected-requests"]
                    if not transitions or choice != transitions[-1][1]:
                        transitions.append((elapsed, choice))
                downshifts = [elapsed for (_previous_time, previous), (elapsed, choice)
                              in zip(transitions, transitions[1:])
                              if previous == 2 and choice == 1]
                self.assertEqual(len(downshifts), 1)
                restored = next(elapsed for elapsed, choice in transitions
                                if elapsed > downshifts[0] and choice == 2)
                self.assertGreater(result["seconds"] - restored, 8)
                self.assertEqual(metrics["selected-requests"], 2)
            finally:
                player.close()

    def test_cached_bursts_still_learn_and_downshift(self):
        # A timed consumer spends most wall time with a full ring. Capacity
        # must accumulate actual active transfers across those idle gaps.
        with Origin(size=8 << 20, latency=0.02, per_lane=256 << 10) as origin:
            player = Player(LIBRARY, **{"ao-null-untimed": "no",
                                       "curl-initial-requests": "4",
                                       "curl-startup-requests": "4",
                                       "curl-media-bitrate": "1536000"})
            try:
                result = player.play(origin.url,
                    loaded=lambda current: current.configure(1, delivery_bitrate=1536000.0))
                metrics = result["metrics"]
                points = metrics["concurrency-samples"]
                self.assertGreater(metrics["capacity-samples"], 0)
                self.assertLess(metrics["selected-requests"], 4)
                four = next(point for point in points if point["requests"] == 4)
                self.assertGreater(four["capacity-bytes-per-second"],
                                   metrics["average-received-bytes-per-second"] * 1.5)
            finally:
                player.close()


def benchmark(library):
    scenarios = {
        "low-rtt": {"latency": 0.005},
        "high-rtt-per-lane": {"latency": 0.2, "per_lane": 1 << 20},
        "shared-cap": {"latency": 0.04, "shared": 2 << 20, "per_lane": 0},
        "serialized-disk": {"latency": 0.04, "per_lane": 1 << 20, "serialized": True},
        "small-startup": {"size": 256 << 10, "latency": 0.2},
    }
    for name, settings in scenarios.items():
        for lanes in (1, 2, 4):
            with Origin(**settings) as origin:
                player = Player(library, **{"curl-initial-requests": lanes,
                                            "curl-startup-requests": lanes,
                                            "curl-ttfb-ms": settings["latency"] * 1000,
                                            "curl-media-bitrate": "12000000"})
                try:
                    result = player.play(origin.url)
                    metrics = result["metrics"]
                    print(json.dumps({"scenario": name, "initial-lanes": lanes,
                                      "seconds": result["seconds"],
                                      "first-progress-seconds": result["first-progress-seconds"],
                                      "selected-requests": metrics["selected-requests"],
                                      "received-bytes": metrics["received-bytes"],
                                      "average-bytes-per-second": metrics["average-received-bytes-per-second"],
                                      "concurrency-samples": metrics["concurrency-samples"]}), flush=True)
                finally:
                    player.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libmpv", required=True)
    parser.add_argument("--benchmark", action="store_true")
    args, remaining = parser.parse_known_args()
    LIBRARY = str(pathlib.Path(args.libmpv).resolve())
    if args.benchmark:
        benchmark(LIBRARY)
    else:
        unittest.main(argv=[__file__, *remaining])
