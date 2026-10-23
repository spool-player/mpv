/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#include <libavformat/avio.h>

struct mpv_global;
struct demuxer;

// Initialize libcurl state, must be called before stream_curl is used.
void mp_curl_global_init(struct mpv_global *global);

// Playback facts the player supplies to the transport, at most every 250 ms.
// Rates are bits/s of media consumption (already scaled by speed); negative
// values are unknown.
struct mp_curl_playback_state {
    double buffer_seconds;
    int64_t fw_bytes, max_fw_bytes;
    double speed;
    double audio_bitrate, video_bitrate;
    bool idle, underrun, eof, user_paused, seeking;
};

// Never blocks on the transport; the latest state wins.
void mp_curl_set_playback_state(struct mpv_global *global,
                                const struct mp_curl_playback_state *state);
// Whether any curl stream is open, i.e. whether playback state is wanted.
bool mp_curl_has_streams(struct mpv_global *global);

#define MP_CURL_MAX_LANES 16

struct mp_curl_lane_metrics {
    int64_t received_bytes;
    double active_seconds;
    int64_t average_bytes_per_second;
    int64_t current_bytes_per_second;
    int64_t peak_bytes_per_second;
    int64_t capacity_bytes_per_second;
    int64_t samples;
};
struct mp_curl_concurrency_metrics {
    int64_t capacity_bytes_per_second;
    double ttfb_ms;
    int64_t samples;
};

#define MP_CURL_MAX_METRIC_STREAMS 64
#define MP_CURL_MAX_METRIC_CONNECTIONS 64

struct mp_curl_stream_metrics {
    int64_t id;
    int64_t readable_bytes, retained_bytes, reorder_bytes;
    bool prefix_publication_available;
};

struct mp_curl_connection_metrics {
    int64_t id, socket_generation; // opaque, never a descriptor/address
    int http_version; // 0 unknown, 10, 11, 20, 30
    int transport; // 0 unknown, 1 TCP, 2 UDP
    int64_t active_requests;
    int64_t receive_buffer, receive_window; // -1 unavailable
    double rtt_ms; // -1 unavailable
};

struct mp_curl_protocol_metrics {
    int http_version;
    int64_t ordered_bytes;
    double cpu_ns_per_ordered_byte;
    bool cpu_available;
};

struct mp_curl_metrics {
    int64_t active_requests;
    int64_t peak_active_requests;
    int64_t active_streams;
    int64_t peak_active_streams;
    int64_t requests_per_stream;
    int64_t range_bytes;
    int64_t ring_bytes;
    int64_t started_requests;
    int64_t finished_requests;
    int64_t failed_requests;
    int64_t retry_attempts;
    int64_t received_bytes;
    int64_t ordered_available_bytes, consumer_delivered_bytes;
    int64_t readable_bytes, retained_bytes, reorder_bytes;
    int64_t copy_in_bytes, copy_out_bytes, reorder_copy_bytes;
    int64_t body_callbacks, worker_iterations, coalesced_wakeups;
    int64_t head_of_line_events;
    double reorder_wait_seconds, oldest_gap_seconds;
    double perform_wall_seconds, dispatch_wall_seconds, poll_wall_seconds;
    // Callback wall time is nested in perform (or dispatch on unpause).
    bool callback_time_included;
    bool snapshot_truncated;
    int num_streams, num_connections;
    struct mp_curl_stream_metrics streams[MP_CURL_MAX_METRIC_STREAMS];
    struct mp_curl_connection_metrics connections[MP_CURL_MAX_METRIC_CONNECTIONS];
    struct mp_curl_protocol_metrics protocols[3]; // HTTP/1.1, HTTP/2, HTTP/3
    int64_t average_received_bytes_per_second;
    int64_t peak_received_bytes_per_second;
    int64_t current_received_bytes_per_second;
    double elapsed_seconds;
    // Lifetime worker CPU and wall clocks; wall time includes idle/poll waits.
    double curl_thread_cpu_seconds;
    double curl_thread_wall_seconds;
    bool curl_thread_cpu_available;
    // Summed wall time across streams (not CPU time), including ongoing
    // producer-full and consumer-empty intervals in every snapshot.
    double ring_wait_seconds;
    double consumer_wait_seconds;
    // Consumer wait while later queue bytes were already retained: the
    // cost of ordered delivery, not of the network as a whole.
    double head_of_line_seconds;
    int64_t peak_reorder_bytes;     // retained bytes beyond the first gap
    int64_t physical_connections;   // distinct connections of active transfers
    double streams_per_connection; // connected active requests / connections
    // Worker wake-up delay past its deadline: largest since the previous
    // snapshot, and for the worker's lifetime.
    double recent_lateness_ms;
    double peak_lateness_ms;
    // Sockets as the kernel reports them (-1 unavailable). RTT is the kernel's
    // smoothed TCP RTT, never request time-to-first-byte.
    int64_t open_sockets;
    int64_t sockets_opened;
    double tcp_rtt_min_ms, tcp_rtt_max_ms;
    int64_t max_socket_receive_buffer;
    int64_t host_rmem_max, host_autotune_max;
    int hw_aes;
    int64_t playback_epoch, stale_controls, rejected_controls;
    bool background_network;
    double declared_bitrate; // bits/s demand prior from configured facts
    // Playback state as the transport sees it.
    int64_t playback_state_updates; // player reports received
    double playback_buffer_seconds;
    double playback_demand_bitrate; // bits/s used as demand, 0 unknown
    bool playback_user_paused;
    int64_t selected_requests;
    double ttfb_ms;
    int64_t capacity_bytes_per_second;
    int64_t capacity_samples;
    int64_t stream_id;
    struct mp_curl_lane_metrics lanes[MP_CURL_MAX_LANES];
    struct mp_curl_concurrency_metrics concurrency[MP_CURL_MAX_LANES];
};

// Read a curl-thread snapshot of counters retained for this mpv core's lifetime.
// Logical lanes are concurrent request slots, not operating system threads.
// Returns false until the curl backend has been initialized.
bool mp_curl_get_metrics(struct mpv_global *global,
                         struct mp_curl_metrics *metrics);

// Host transport control (curl-transport-control, format 2). A configure
// installs one playback's media facts under a strictly newer epoch; an update
// changes live host state for the current epoch only. Facts <= 0 are unknown.
struct mp_curl_control {
    int op; // 0 configure, 1 update
    int64_t epoch; // > 0
    // op 0. Bitrates are bits/s, sizes bytes, durations seconds.
    double source_bitrate, delivery_bitrate;
    int64_t source_size, delivery_size;
    double source_duration, delivery_duration;
    bool direct_file; // delivered bytes are the original file
    // op 1: the host is fetching other data from the network (e.g. previews).
    bool background_network;
};

// False when a configure conflicts with the installed one for its epoch.
// Older epochs and updates for another epoch are ignored and counted.
bool mp_curl_set_control(struct mpv_global *global,
                         const struct mp_curl_control *control);

// Open `url` via mpv's libcurl backend and wrap it as a fresh AVIOContext.
// On success returns 0, fills *pb_out with the new context, and sets *data to
// an opaque handle that must later be passed to mp_curl_avio_close() to
// release all associated resources.
// Returns AVERROR(ENOSYS) when this backend doesn't handle the URL.
// Returns AVERROR(EINVAL) when the URL's protocol is rejected by
// `whitelist`/`blacklist`.
// `flags` is the AVIO_FLAG_* mask passed to AVFormatContext.io_open.
// If `options` has `protocol_whitelist` or `protocol_blacklist` entries they
// override the explicit `whitelist`/`blacklist` arguments (same as FFmpeg).
int mp_curl_avio_open(struct demuxer *demuxer, AVIOContext **pb_out,
                      void **data, const char *url, int flags,
                      AVDictionary **options,
                      const char *whitelist, const char *blacklist);

// Tear down an AVIOContext previously produced by mp_curl_avio_open().
void mp_curl_avio_close(AVIOContext *pb, void *data);
