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
import struct
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

    def control(self, bitrate=0, buffer=0, paused=False):
        keep = []
        node = encode({"media-bitrate": bitrate, "buffer-seconds": buffer, "paused": paused}, keep)
        self.check(self.lib.mpv_set_property(self.handle, b"curl-transport-control", 6, C.byref(node)), "control")

    def play(self, url, loaded=None, timeout=90):
        started = time.monotonic()
        self.command("loadfile", url)
        samples = []
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
            if first_progress is None and position is not None and position > 0:
                first_progress = time.monotonic() - started
            if event.id == 7:
                if not was_loaded:
                    raise AssertionError("transport failed before file-loaded")
                end = C.cast(event.data, C.POINTER(EndFile)).contents
                if end.reason != 0:
                    raise AssertionError(f"playback did not reach EOF: reason={end.reason}, error={end.error}")
                return {"seconds": time.monotonic() - started,
                        "first-progress-seconds": first_progress,
                        "samples": samples, "metrics": self.get("curl-transport-metrics")}
        raise AssertionError("transport did not reach EOF")


class Origin:
    def __init__(self, size=8 << 20, latency=0.04, per_lane=2 << 20,
                 shared=0, serialized=False, reorder=False, fault=None,
                 cap=0, unknown_size=False):
        pcm = (bytes(range(256)) * ((size + 255) // 256))[:size - size % 4]
        self.pcm = pcm
        self.payload = (b"RIFF" + struct.pack("<I", len(pcm) + 36) + b"WAVEfmt " +
                        struct.pack("<IHHIIHH", 16, 1, 2, 48000, 192000, 4, 16) +
                        b"data" + struct.pack("<I", len(pcm)) + pcm)
        self.latency, self.per_lane, self.shared = latency, per_lane, shared
        self.serialized, self.reorder, self.fault = serialized, reorder, fault
        self.cap, self.unknown_size = cap, unknown_size
        self.lock = threading.Lock()
        self.disk = threading.Lock()
        self.next_shared = 0
        self.requests = []
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
                    inject = bool(origin.fault and not origin.faulted and start > 0)
                    origin.faulted |= inject
                    origin.requests.append((start, end))
                guard = origin.disk if origin.serialized else contextlib.nullcontext()
                try:
                    with guard:
                        time.sleep(origin.latency + (0.1 if origin.reorder and start == 0 else 0))
                        self.send_response(206 if match else 200)
                        self.send_header("Accept-Ranges", "bytes")
                        self.send_header("Content-Type", "audio/wav")
                        self.send_header("Content-Length", str(end - start))
                        if match:
                            first = start + 1 if inject and origin.fault == "range" else start
                            total = "*" if origin.unknown_size else len(origin.payload)
                            self.send_header("Content-Range", f"bytes {first}-{end - 1}/{total}")
                        self.end_headers()
                        cut = inject and origin.fault in ("truncate", "stall")
                        stop = start + (end - start) // 2 if cut else end
                        deadline = time.monotonic()
                        for offset in range(start, stop, 16384):
                            block = origin.payload[offset:min(offset + 16384, stop)]
                            if origin.shared:
                                with origin.lock:
                                    deadline_shared = max(time.monotonic(), origin.next_shared)
                                    origin.next_shared = deadline_shared + len(block) / origin.shared
                                time.sleep(max(0, deadline_shared - time.monotonic()))
                            if origin.per_lane:
                                deadline += len(block) / origin.per_lane
                                time.sleep(max(0, deadline - time.monotonic()))
                            self.wfile.write(block)
                            self.wfile.flush()
                        if stop != end and origin.fault == "stall":
                            # Go quiet with the connection open, as a server
                            # that lost interest without closing does.
                            time.sleep(STALL_SECONDS)
                        if stop != end:
                            self.close_connection = True
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_port}/fixture.wav"


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
                current.control(paused=True)
                time.sleep(2.5)
                settled = current.get("curl-transport-metrics")
                time.sleep(1.25)
                idle = current.get("curl-transport-metrics")
                self.assertEqual(idle["received-bytes"], settled["received-bytes"])
                self.assertEqual(idle["current-received-bytes-per-second"], 0)
                observed["before-seek"] = idle["received-bytes"]
                current.command("seek", "2", "absolute+exact")
                current.control(paused=False)
                current.set("pause", "no")

            try:
                first = player.play(origin.url, loaded=pause_seek_resume)["metrics"]
                self.assertGreaterEqual(first["received-bytes"], observed["before-seek"])
                self.assertEqual(first["retry-attempts"], 0)
                second = player.play(origin.url)["metrics"]
                self.assertGreater(second["received-bytes"], first["received-bytes"])
                self.assertGreater(second["stream-id"], first["stream-id"])
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
                    loaded=lambda current: current.control(bitrate=1536000, buffer=5))
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
                                            "curl-latency-ms": settings["latency"] * 1000,
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
