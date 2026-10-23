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

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define HAVE_SOCKET_PROBES 1
#endif
#if defined(__linux__)
#include <sys/auxv.h>
#if defined(__arm__) && __has_include(<asm/hwcap.h>)
#include <asm/hwcap.h>
#endif
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <curl/curl.h>

#include <libavformat/avio.h>
#include <libavformat/version.h>
#include <libavutil/avstring.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "stream.h"
#include "stream_curl.h"

#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "cookies.h"
#include "demux/avio_crypto.h"
#include "demux/demux.h"
#include "misc/bstr.h"
#include "misc/dispatch.h"
#include "misc/path_utils.h"
#include "misc/thread_tools.h"
#include "mpv_talloc.h"
#include "network.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/path.h"
#include "osdep/threads.h"
#include "osdep/timer.h"

enum curl_proto {
    MP_CURL_PROTO_HTTP,
    MP_CURL_PROTO_FTP,
};

struct curl_scheme {
    bstr scheme;
    enum curl_proto proto;
};

static const struct curl_scheme curl_schemes[] = {
    {bstr0_lit("http"), MP_CURL_PROTO_HTTP},
    {bstr0_lit("https"), MP_CURL_PROTO_HTTP},
    {bstr0_lit("ftp"), MP_CURL_PROTO_FTP},
    {bstr0_lit("ftps"), MP_CURL_PROTO_FTP},
};

// Special args for use by lavf. Matches lavf/http.c "offset"/"end_offset" opts.
// `offset` is the inclusive starting byte.
// `end_offset` is the exclusive upper bound (0 = unbounded).
struct curl_open_args {
    int64_t offset;
    int64_t end_offset;
};

struct curl_opts {
    bool enabled;
    int http_version;
    int max_redirects;
    int max_retries;
    double connect_timeout;
    int64_t buffer_size;
    int64_t read_size;
    int64_t max_request_size;
    int parallel_requests;
    bool adaptive;
    int initial_requests;
    double latency_ms;
    int64_t observed_bps;
    double media_bitrate;
    int startup_requests;
    int64_t startup_bytes;
    double startup_buffer_seconds;
};

// Older lavf has a bug with nested IO cleanup, so don't enable curl by default.
// <https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/23082>
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 15, 101)
#define CURL_BY_DEFAULT
#elif LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 12, 102) && LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(62, 13, 0)
#define CURL_BY_DEFAULT /* 8.1 backport */
#elif LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 3, 103) && LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(62, 4, 0)
#define CURL_BY_DEFAULT /* 8.0 backport */
#endif

#define OPT_BASE_STRUCT struct curl_opts
const struct m_sub_options curl_conf = {
    .opts = (const struct m_option[]) {
        {"enabled", OPT_BOOL(enabled)},
        {"http-version", OPT_CHOICE(http_version,
            {"auto", CURL_HTTP_VERSION_NONE},
            {"1.0", CURL_HTTP_VERSION_1_0},
            {"1.1", CURL_HTTP_VERSION_1_1},
            {"2", CURL_HTTP_VERSION_2},
            {"2tls", CURL_HTTP_VERSION_2TLS},
            {"2-prior-knowledge", CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE},
            {"3", CURL_HTTP_VERSION_3},
            {"3only", CURL_HTTP_VERSION_3ONLY}
            )},
        {"max-redirects", OPT_INT(max_redirects), M_RANGE(0, 100)},
        {"max-retries", OPT_INT(max_retries), M_RANGE(0, 100)},
        {"connect-timeout", OPT_DOUBLE(connect_timeout), M_RANGE(0, 600)},
        {"buffer-size", OPT_BYTE_SIZE(buffer_size),
            M_RANGE(2 * CURL_MAX_WRITE_SIZE, M_MAX_MEM_BYTES)},
        {"read-size", OPT_BYTE_SIZE(read_size),
            M_RANGE(CURL_MAX_WRITE_SIZE, CURL_MAX_READ_SIZE)},
        {"max-request-size", OPT_BYTE_SIZE(max_request_size),
            M_RANGE(0, M_MAX_MEM_BYTES)},
        {"parallel-requests", OPT_INT(parallel_requests), M_RANGE(1, 16)},
        {"adaptive", OPT_BOOL(adaptive)},
        {"initial-requests", OPT_INT(initial_requests), M_RANGE(1, 16)},
        {"latency-ms", OPT_DOUBLE(latency_ms), M_RANGE(0, 600000)},
        {"observed-bps", OPT_BYTE_SIZE(observed_bps), M_RANGE(0, M_MAX_MEM_BYTES)},
        {"media-bitrate", OPT_DOUBLE(media_bitrate), M_RANGE(0, 1e12)},
        {"startup-requests", OPT_INT(startup_requests), M_RANGE(1, 16)},
        {"startup-bytes", OPT_BYTE_SIZE(startup_bytes), M_RANGE(0, M_MAX_MEM_BYTES)},
        {"startup-buffer-seconds", OPT_DOUBLE(startup_buffer_seconds), M_RANGE(0, 600)},
        {0}
    },
    .defaults = &(const struct curl_opts) {
#ifdef CURL_BY_DEFAULT
        .enabled = true,
#endif
        .http_version = CURL_HTTP_VERSION_NONE,
        .max_redirects = 16,
        .max_retries = 5,
        .connect_timeout = 30,
        .buffer_size = 4 << 20, // 4 MiB
        .read_size = 256 << 10,
        .max_request_size = 0,
        .parallel_requests = 1,
        .adaptive = true,
        .initial_requests = 1,
        .startup_requests = 1,
        .startup_buffer_seconds = 0.5,
    },
    .size = sizeof(struct curl_opts),
};

static const struct curl_scheme *curl_scheme_lookup(bstr url)
{
    bstr scheme = mp_split_proto(url, NULL);
    for (int i = 0; i < MP_ARRAY_SIZE(curl_schemes); i++) {
        if (bstrcasecmp(scheme, curl_schemes[i].scheme) == 0)
            return &curl_schemes[i];
    }
    return NULL;
}

// All rate state is owned by the curl thread. Snapshot readers use its dispatch
// queue, so a body callback requires no locks or per-chunk atomic operations.
#define RATE_BUCKET_NS MP_TIME_MS_TO_NS(250)
#define RATE_BUCKETS 8
struct curl_rate {
    uint64_t bytes;
    int64_t started_ns;
    int64_t bucket_id[RATE_BUCKETS];
    uint64_t bucket_bytes[RATE_BUCKETS];
    int64_t peak;
    int users;
    int64_t active_started_ns;
    int64_t active_ns;
};

#define MP_CURL_MAX_SOCKETS 64

// A socket libcurl opened through us. Entries live as long as the core: a
// pooled connection outlives the transfers that used it.
struct curl_socket_info {
    curl_socket_t fd; // CURL_SOCKET_BAD when the entry is free
    bool tcp;
    int64_t receive_buffer; // effective SO_RCVBUF read-back, -1 unknown
    double rtt_ms;          // kernel smoothed RTT, -1 unavailable
};

struct curl_ctx {
    mp_thread thread;
    struct mp_dispatch_queue *dispatch;
    CURLM *multi;
    CURLSH *share; // process-wide; NULL if it could not be created
    bool exit;

    // Set when libcurl cannot be used anymore.
    const char *error;
    atomic_bool failed;
    atomic_int active_requests;
    atomic_int peak_active_requests;
    atomic_int active_streams;
    atomic_int peak_active_streams;
    atomic_int requests_per_stream;
    atomic_int_fast64_t range_bytes;
    atomic_int_fast64_t ring_bytes;
    atomic_uint_fast64_t started_requests;
    atomic_uint_fast64_t finished_requests;
    atomic_uint_fast64_t failed_requests;
    atomic_uint_fast64_t retry_attempts;
    struct curl_rate traffic;
    int64_t thread_started_ns;
    int64_t thread_cpu_started_ns;
    struct curl_rate ring_wait;
    atomic_int_fast64_t consumer_wait_ns;
    // Sockets and host limits, owned by the curl thread.
    struct curl_socket_info sockets[MP_CURL_MAX_SOCKETS];
    uint64_t sockets_opened;
    int64_t next_socket_sample_ns;
    int64_t host_rmem_max;      // unprivileged SO_RCVBUF ceiling, -1 unknown
    int64_t host_autotune_max;  // receive autotuning ceiling, -1 unknown
    int hw_aes;                 // 1 yes, 0 no, -1 unknown
    // Player state handed over without waiting for this thread.
    mp_mutex state_lock;
    struct mp_curl_playback_state pending_state;
    bool state_pending;
    // Applied on the curl thread. Once the player reports, it owns pause and
    // buffer state, and curl-transport-control supplies only a bitrate prior.
    bool player_state;
    int64_t state_updates;
    double declared_bitrate;
    double observed_bitrate;
    atomic_int_fast64_t head_of_line_ns;
    atomic_int_fast64_t peak_reorder_bytes;
    // Worker wake-up lateness past its own deadline; recent is since the
    // last metrics snapshot. Both owned by the curl thread.
    atomic_int_fast64_t recent_lateness_ns;
    int64_t peak_lateness_ns;
    struct curl_rate lanes[MP_CURL_MAX_LANES];
    int64_t lane_capacity[MP_CURL_MAX_LANES];
    int64_t lane_samples[MP_CURL_MAX_LANES];
    int64_t next_stream_id;
    int64_t stream_id;
    int selected_requests;
    double latency_ms;
    int64_t capacity;
    int64_t capacity_samples;
    struct mp_curl_concurrency_metrics concurrency[MP_CURL_MAX_LANES];
    double media_bitrate;
    double buffer_seconds;
    uint64_t pause_serial;
    bool playback_paused;
    bool playback_seeking;
    int64_t paused_ns;
    int64_t pause_started_ns;
    int64_t buffer_generation;
};

struct priv;

enum curl_handle_kind {
    CURL_HANDLE_PRIMARY,
    CURL_HANDLE_DYNAMIC,
};

struct curl_handle_ref {
    enum curl_handle_kind kind;
    struct priv *p;
    int lane;
    int64_t started_ns;
    int64_t first_byte_ns;
    uint64_t received;
    bool blocked;
    bool measuring;
    uint64_t pause_serial;
    uint64_t generation;
};

#define MP_CURL_MAX_RANGES 32

// One ordered byte range of the parallel queue. Its storage belongs to the
// range until the consumer has read every byte of it, so a retry can verify
// already published bytes and a later range can never overwrite them.
struct range_desc {
    uint64_t start, end;  // absolute [start, end)
    uint8_t *data;        // end - start bytes, fixed per queue entry
    // Curl thread only.
    size_t received;      // contiguous bytes stored from start
    struct dynamic_request *request; // transfer filling it, NULL if none
    int retry_count;
    // Protected by priv.mtx.
    size_t published;     // bytes the consumer may read, <= received
    size_t consumed;      // bytes the consumer has taken
    bool complete;        // every byte is published
};

// A network transfer slot. Slots are independent of range storage, so a
// finished transfer can start the next range while its bytes wait to be read.
struct dynamic_request {
    struct curl_handle_ref ref;
    CURL *curl;
    struct range_desc *range; // range being fetched, NULL when idle
    size_t fetch_pos;         // next offset in range this transfer writes
    bool active;
    bool response_ok;
    bool changed;             // retried bytes differ from published bytes
};

// Per-stream state, owned by the curl thread.
struct priv {
    struct mp_log *log;
    struct mpv_global *global;
    struct curl_ctx *ctx;
    struct stream *s;

    struct curl_opts *opts;
    struct mp_network_opts *net_opts;

    CURL *curl;
    struct curl_handle_ref primary_ref;
    struct curl_slist *headers;
    char *url;
    const struct curl_scheme *scheme;

    // Stream parameters
    bool seekable;
    int64_t content_size; // -1 if unknown
    int64_t start_offset; // requested initial byte offset, immutable after open
    char *server_filename; // from Content-Disposition, NULL if absent

    // Producer state. Only touched by the curl thread.
    uint64_t request_start;    // absolute byte position of next request
    uint64_t request_received; // bytes received in the current request
    uint64_t request_end;      // exclusive byte cap (0 = unbounded)
    int retry_count;           // consecutive failed attempts at request_start
    bool active;               // handle is currently active in the multi
    bool response_ok;          // current response matches the requested bytes
    bool finished;             // current request has reached EOF
    bool reply_ranged;         // current response is a 206 byte range
    bool reply_eof;            // current response marks the end of the file

    // Parallel ranged prefetch. Transfers write straight into an ordered
    // queue of ranges after the primary request; the consumer reads the
    // queue once the ring holds nothing earlier.
    struct dynamic_request *requests;
    int num_requests;
    struct range_desc *ranges; // circular queue, num_ranges entries
    uint8_t *range_data;
    int num_ranges;
    char *etag;                // strong validator of the primary response
    uint64_t primary_end;
    uint64_t next_schedule;
    bool dynamic_started;
    bool dynamic_supported;
    bool primary_done;
    int selected_requests;
    int previous_requests;
    int rejected_requests;
    double rejected_capacity;
    int64_t rejected_ns;
    int rejected_lower_requests;
    double rejected_lower_capacity;
    int64_t rejected_lower_ns;
    int64_t stream_id;
    size_t segment_size;
    double latency_ms;
    double previous_capacity;
    uint64_t phase_bytes;
    uint64_t phase_delivered;
    int phase_completed;
    bool phase_blocked;
    int64_t last_adjustment_ns;
    int64_t capacity_samples;
    size_t paused_bytes;
    uint64_t generation;
    int transitioning;
    int learning_active;
    int64_t learning_updated_ns;
    int64_t phase_active_ns;
    bool ring_waiting; // curl-thread accounting, independent of resume commands
    bool startup;
    uint64_t startup_start;
    uint64_t startup_end;
    int64_t startup_buffer_generation;

    // Probe state. Set on the curl thread read by curl_open after.
    bool probed;
    bool stream_ok;
    bool metrics_registered;

    // Shared state, protected by mtx.
    mp_mutex mtx;
    mp_cond cond;
    uint8_t *buffer;
    size_t buffer_size;
    size_t head, tail, count;
    bool paused;         // write callback paused due to a full buffer
    int range_head;      // oldest queued range
    int range_count;     // queued ranges, read or not
    uint64_t ordered_pos; // end of the contiguous published queue prefix
    uint64_t queued_bytes; // published queue bytes not yet consumed
    uint64_t queue_pos;  // next queue byte the consumer reads
    bool queue_open;     // the ring holds nothing before the queue
    bool consumer_waiting;
    bool copying;        // consumer is copying queue bytes unlocked
    bool copy_waiter;    // curl thread waits for that copy to finish
    bool service_pending; // a CMD_PUMP is queued for the curl thread
    bool stream_eof;     // producer has delivered all data
    bool stream_error;   // unrecoverable error
    atomic_bool aborted; // canceled by user (mp_cancel)
    struct mp_icy *icy;  // ICY metadata state, dormant until Icy-MetaInt seen
};

// Curl thread

enum cmd_kind {
    CMD_ADD,
    CMD_REMOVE,
    CMD_SEEK,
    CMD_UNPAUSE,
    CMD_EXIT,
    CMD_START_DYNAMIC,
    CMD_PUMP,
    CMD_FALLBACK_DYNAMIC,
};

struct cmd {
    enum cmd_kind kind;
    struct curl_ctx *ctx;
    struct priv *p;
    int64_t pos;
    bool drop;
};

static void start_request(struct priv *p);
static void on_done(struct priv *p, CURLcode code);
static void start_dynamic(struct priv *p);
static void fill_dynamic(struct priv *p);
static void open_queue(struct priv *p);
static void apply_playback_state(struct curl_ctx *ctx);
static void stop_dynamic(struct priv *p);
static void on_dynamic_done(struct dynamic_request *request, CURLcode code);
static void fallback_dynamic(struct priv *p);
static void update_adaptation(struct priv *p);

static int64_t rate_current(struct curl_rate *rate, int64_t now)
{
    int64_t bucket = now / RATE_BUCKET_NS;
    uint64_t bytes = 0;
    // Four complete 250ms buckets form an exact wall-clock second. Never
    // divide a callback burst by a near-zero interval, including at startup.
    for (int n = 0; n < RATE_BUCKETS; n++) {
        int64_t age = bucket - rate->bucket_id[n];
        if (age >= 1 && age <= 4)
            bytes += rate->bucket_bytes[n];
    }
    if (rate->started_ns && now - rate->started_ns >= MP_TIME_S_TO_NS(1))
        rate->peak = MPMAX(rate->peak, bytes);
    return bytes;
}

static void rate_start(struct curl_rate *rate, int64_t now)
{
    if (!rate->started_ns)
        rate->started_ns = now;
    if (rate->users)
        rate->active_ns += rate->users * (now - rate->active_started_ns);
    rate->users++;
    rate->active_started_ns = now;
}

static void rate_stop(struct curl_rate *rate, int64_t now)
{
    mp_assert(rate->users > 0);
    rate->active_ns += rate->users * (now - rate->active_started_ns);
    rate->users--;
    rate->active_started_ns = now;
}

static void begin_ring_wait(struct priv *p, int64_t now)
{
    if (!p->ring_waiting) {
        p->ring_waiting = true;
        rate_start(&p->ctx->ring_wait, now);
    }
}

static void end_ring_wait(struct priv *p)
{
    if (p->ring_waiting) {
        p->ring_waiting = false;
        rate_stop(&p->ctx->ring_wait, mp_time_ns());
    }
}

static void rate_add(struct curl_rate *rate, size_t bytes, int64_t now)
{
    rate_current(rate, now);
    int64_t bucket = now / RATE_BUCKET_NS;
    int index = bucket % RATE_BUCKETS;
    if (rate->bucket_id[index] != bucket) {
        rate->bucket_id[index] = bucket;
        rate->bucket_bytes[index] = 0;
    }
    rate->bucket_bytes[index] += bytes;
    rate->bytes += bytes;
}

static int64_t learning_time(struct curl_ctx *ctx, int64_t now)
{
    return now - ctx->paused_ns -
        (ctx->playback_paused ? now - ctx->pause_started_ns : 0);
}

static void learning_tick(struct priv *p, int64_t now)
{
    int64_t clock = learning_time(p->ctx, now);
    if (p->learning_updated_ns && !p->transitioning && !p->startup &&
        p->learning_active > (p->paused ? 1 : 0))
    {
        p->phase_active_ns += clock - p->learning_updated_ns;
    }
    p->learning_updated_ns = clock;
}

static void begin_measurement(struct curl_handle_ref *ref)
{
    struct priv *p = ref->p;
    int64_t now = mp_time_ns();
    learning_tick(p, now);
    p->learning_active++;
    ref->started_ns = now;
    ref->first_byte_ns = 0;
    ref->received = 0;
    ref->blocked = false;
    ref->measuring = true;
    ref->pause_serial = p->ctx->pause_serial;
    ref->generation = p->generation;
    rate_start(&p->ctx->traffic, now);
    rate_start(&p->ctx->lanes[ref->lane], now);
}

static void end_measurement(struct curl_handle_ref *ref, bool succeeded)
{
    if (!ref->measuring)
        return;
    struct priv *p = ref->p;
    struct curl_ctx *ctx = p->ctx;
    int64_t now = mp_time_ns();
    learning_tick(p, now);
    p->learning_active--;
    if (ref->generation != p->generation) {
        mp_assert(p->transitioning > 0);
        p->transitioning--;
    }
    ref->measuring = false;
    rate_stop(&ctx->traffic, now);
    rate_stop(&ctx->lanes[ref->lane], now);
    // Capacity includes setup, TTFB and server stalls, but a deliberately
    // paused request is not evidence of low server capacity.
    if (succeeded && !ref->blocked && !ctx->playback_paused &&
        ref->pause_serial == ctx->pause_serial &&
        ref->generation == p->generation &&
        ref->received && now > ref->started_ns)
    {
        int64_t rate = ref->received / MP_TIME_NS_TO_S(now - ref->started_ns);
        int lane = ref->lane;
        if (ctx->stream_id == p->stream_id) {
            ctx->lane_capacity[lane] = ctx->lane_samples[lane]
                ? (ctx->lane_capacity[lane] * 3 + rate) / 4 : rate;
            ctx->lane_samples[lane]++;
        }
        if (ref->first_byte_ns) {
            double latency = MP_TIME_NS_TO_S(
                ref->first_byte_ns - ref->started_ns) * 1000;
            p->latency_ms = p->latency_ms > 0
                ? p->latency_ms * 0.75 + latency * 0.25 : latency;
            if (ctx->stream_id == p->stream_id)
                ctx->latency_ms = p->latency_ms;
        }
        p->phase_completed++;
    } else {
        p->phase_blocked = true;
    }
}

static void record_received_bytes(struct curl_handle_ref *ref, size_t bytes)
{
    int64_t now = mp_time_ns();
    if (!ref->first_byte_ns)
        ref->first_byte_ns = now;
    ref->received += bytes;
    rate_add(&ref->p->ctx->traffic, bytes, now);
    rate_add(&ref->p->ctx->lanes[ref->lane], bytes, now);
    struct priv *p = ref->p;
    if (!p->transitioning && !p->startup && !p->ctx->playback_paused &&
        ref->generation == p->generation)
    {
        p->phase_bytes += bytes;
    }
}

static void setup_curl(struct priv *p, CURL *curl);

static CURLMcode add_curl_handle(struct curl_ctx *ctx, CURL *handle)
{
    // Attached here, on the curl thread, while the handle is idle: streams
    // configure their handles before this core's share exists.
    if (ctx->share)
        curl_easy_setopt(handle, CURLOPT_SHARE, ctx->share);
    CURLMcode result = curl_multi_add_handle(ctx->multi, handle);
    if (result != CURLM_OK)
        return result;

    int active = atomic_fetch_add_explicit(&ctx->active_requests, 1,
                                            memory_order_relaxed) + 1;
    atomic_fetch_add_explicit(&ctx->started_requests, 1, memory_order_relaxed);
    int peak = atomic_load_explicit(&ctx->peak_active_requests,
                                    memory_order_relaxed);
    while (active > peak &&
           !atomic_compare_exchange_weak_explicit(
               &ctx->peak_active_requests, &peak, active,
               memory_order_relaxed, memory_order_relaxed))
    {}
    return CURLM_OK;
}

static CURLMcode remove_curl_handle(struct curl_ctx *ctx, CURL *handle)
{
    CURLMcode result = curl_multi_remove_handle(ctx->multi, handle);
    if (result == CURLM_OK) {
        int previous = atomic_fetch_sub_explicit(&ctx->active_requests, 1,
                                                  memory_order_relaxed);
        mp_assert(previous > 0);
    }
    return result;
}

static void finish_curl_request(struct curl_ctx *ctx, bool succeeded)
{
    atomic_fetch_add_explicit(&ctx->finished_requests, 1, memory_order_relaxed);
    if (!succeeded)
        atomic_fetch_add_explicit(&ctx->failed_requests, 1,
                                  memory_order_relaxed);
}

static void register_curl_stream(struct priv *p)
{
    p->metrics_registered = true;
    int active = atomic_fetch_add_explicit(&p->ctx->active_streams, 1,
                                            memory_order_relaxed) + 1;
    int peak = atomic_load_explicit(&p->ctx->peak_active_streams,
                                    memory_order_relaxed);
    while (active > peak &&
           !atomic_compare_exchange_weak_explicit(
               &p->ctx->peak_active_streams, &peak, active,
               memory_order_relaxed, memory_order_relaxed))
    {}
    atomic_store_explicit(&p->ctx->requests_per_stream,
                          p->opts->parallel_requests, memory_order_relaxed);
    atomic_store_explicit(&p->ctx->range_bytes, p->opts->max_request_size,
                          memory_order_relaxed);
    atomic_store_explicit(&p->ctx->ring_bytes, p->opts->buffer_size,
                          memory_order_relaxed);
}

static void unregister_curl_stream(struct priv *p)
{
    if (!p->metrics_registered)
        return;
    p->metrics_registered = false;
    int previous = atomic_fetch_sub_explicit(&p->ctx->active_streams, 1,
                                              memory_order_relaxed);
    mp_assert(previous > 0);
}

static void run_cmd(void *arg)
{
    struct cmd *c = arg;
    struct curl_ctx *ctx = c->ctx;
    switch (c->kind) {
    case CMD_ADD:
        c->p->stream_id = ++ctx->next_stream_id;
        c->p->startup_start = c->p->request_start;
        c->p->startup_buffer_generation = ctx->buffer_generation;
        ctx->stream_id = c->p->stream_id;
        ctx->selected_requests = c->p->selected_requests;
        // Seed only the summary; learned concurrency points require real data.
        ctx->capacity = c->p->opts->observed_bps;
        ctx->capacity_samples = 0;
        ctx->latency_ms = c->p->latency_ms;
        memset(ctx->concurrency, 0, sizeof(ctx->concurrency));
        memset(ctx->lane_capacity, 0, sizeof(ctx->lane_capacity));
        memset(ctx->lane_samples, 0, sizeof(ctx->lane_samples));
        MP_TRACE(c->p, "starting curl request at %" PRIu64 "\n", c->p->request_start);
        start_request(c->p);
        break;
    case CMD_REMOVE:
        end_ring_wait(c->p);
        stop_dynamic(c->p);
        if (c->p->active) {
            MP_TRACE(c->p, "removing curl handle\n");
            remove_curl_handle(ctx, c->p->curl);
            end_measurement(&c->p->primary_ref, false);
            c->p->active = false;
        }
        break;
    case CMD_UNPAUSE:
        end_ring_wait(c->p);
        // The consumer freed enough buffer space. Clear the pause flag and
        // resume the transfer.
        mp_mutex_lock(&c->p->mtx);
        MP_TRACE(c->p, "resuming curl transfer\n");
        learning_tick(c->p, mp_time_ns());
        c->p->paused = false;
        mp_mutex_unlock(&c->p->mtx);
        curl_easy_pause(c->p->curl, CURLPAUSE_CONT);
        break;
    case CMD_SEEK:
        end_ring_wait(c->p);
        stop_dynamic(c->p);
        MP_TRACE(c->p, "seeking to %" PRIu64 "\n", c->pos);
        if (c->p->active) {
            remove_curl_handle(ctx, c->p->curl);
            c->p->active = false;
            end_measurement(&c->p->primary_ref, false);
        }
        mp_mutex_lock(&c->p->mtx);
        if (c->drop)
            c->p->head = c->p->tail = c->p->count = 0;
        c->p->paused = false;
        c->p->stream_eof = false;
        c->p->stream_error = false;
        mp_cond_broadcast(&c->p->cond);
        mp_mutex_unlock(&c->p->mtx);
        c->p->request_start = c->pos;
        c->p->request_received = 0;
        c->p->retry_count = 0;
        c->p->finished = false;
        c->p->primary_done = false;
        c->p->paused_bytes = 0;
        c->p->phase_bytes = 0;
        c->p->phase_completed = 0;
        c->p->phase_delivered = 0;
        c->p->phase_blocked = false;
        c->p->phase_active_ns = 0;
        c->p->learning_updated_ns = 0;
        c->p->previous_requests = 0;
        c->p->rejected_requests = 0;
        c->p->rejected_lower_requests = 0;
        c->p->startup = c->p->opts->adaptive &&
                         c->p->opts->max_request_size > 0;
        c->p->startup_end = 0;
        c->p->startup_start = c->pos;
        c->p->startup_buffer_generation = ctx->buffer_generation;
        c->p->selected_requests = c->p->opts->adaptive
            ? MPMIN(c->p->opts->startup_requests,
                    c->p->opts->parallel_requests)
            : c->p->opts->parallel_requests;
        c->p->generation++;
        start_request(c->p);
        break;
    case CMD_START_DYNAMIC:
        start_dynamic(c->p);
        break;
    case CMD_PUMP:
        // The consumer freed a queue range; refill transfer slots.
        mp_mutex_lock(&c->p->mtx);
        c->p->service_pending = false;
        mp_mutex_unlock(&c->p->mtx);
        if (c->p->dynamic_started)
            fill_dynamic(c->p);
        break;
    case CMD_FALLBACK_DYNAMIC:
        fallback_dynamic(c->p);
        break;
    case CMD_EXIT:
        ctx->exit = true;
        break;
    }
}

static void cmd_async(struct priv *p, enum cmd_kind kind)
{
    struct cmd *c = talloc_zero(NULL, struct cmd);
    c->kind = kind;
    c->ctx = p->ctx;
    c->p = p;
    mp_dispatch_enqueue_autofree(p->ctx->dispatch, run_cmd, c);
}

static void cmd_sync(struct priv *p, enum cmd_kind kind, int64_t pos, bool drop)
{
    struct cmd c = {
        .kind = kind,
        .ctx = p->ctx,
        .p = p,
        .pos = pos,
        .drop = drop,
    };
    mp_dispatch_run(p->ctx->dispatch, run_cmd, &c);
}

static void set_failed(struct curl_ctx *ctx, const char *error)
{
    ctx->error = error;
    atomic_store_explicit(&ctx->failed, true, memory_order_release);
}

static bool curl_failed(struct curl_ctx *ctx, struct mp_log *log, int level)
{
    if (!atomic_load_explicit(&ctx->failed, memory_order_acquire))
        return false;
    mp_msg(log, level, "libcurl failed (%s), falling back to lavf\n", ctx->error);
    return true;
}

static void curl_wakeup(void *arg)
{
    struct curl_ctx *ctx = arg;
    curl_multi_wakeup(ctx->multi);
}

// DNS answers and TLS sessions shared by every core in the process, so a
// new playback core resumes TLS instead of repeating full handshakes and
// certificate-chain verification. Cookies and connections are not shared.
static mp_static_mutex share_lock = MP_STATIC_MUTEX_INITIALIZER;
static mp_mutex share_data_locks[CURL_LOCK_DATA_LAST];
static CURLSH *share;
static int share_users;

static void share_lock_cb(CURL *handle, curl_lock_data data,
                          curl_lock_access access, void *userptr)
{
    mp_mutex_lock(&share_data_locks[data]);
}

static void share_unlock_cb(CURL *handle, curl_lock_data data, void *userptr)
{
    mp_mutex_unlock(&share_data_locks[data]);
}

static CURLSH *acquire_share(void)
{
    mp_mutex_lock(&share_lock);
    if (!share) {
        // A share kept after a failed cleanup still owns initialized locks.
        for (int n = 0; n < CURL_LOCK_DATA_LAST; n++)
            mp_mutex_init(&share_data_locks[n]);
        share = curl_share_init();
        if (share &&
            (curl_share_setopt(share, CURLSHOPT_LOCKFUNC, share_lock_cb) ||
             curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, share_unlock_cb) ||
             curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS) ||
             curl_share_setopt(share, CURLSHOPT_SHARE,
                               CURL_LOCK_DATA_SSL_SESSION)))
        {
            curl_share_cleanup(share);
            share = NULL;
        }
        if (!share) {
            for (int n = 0; n < CURL_LOCK_DATA_LAST; n++)
                mp_mutex_destroy(&share_data_locks[n]);
        }
    }
    if (share)
        share_users++;
    CURLSH *result = share;
    mp_mutex_unlock(&share_lock);
    return result;
}

// Called after the releasing core's handles are gone.
static void release_share(void)
{
    mp_mutex_lock(&share_lock);
    if (share && !--share_users &&
        curl_share_cleanup(share) == CURLSHE_OK)
    {
        share = NULL;
        for (int n = 0; n < CURL_LOCK_DATA_LAST; n++)
            mp_mutex_destroy(&share_data_locks[n]);
    }
    mp_mutex_unlock(&share_lock);
}

// Socket and host probes. They report what the kernel says; every probe
// that cannot answer is unavailable (-1), never a plausible zero.

static int detect_hw_aes(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return 1;
#elif defined(__linux__) && defined(__aarch64__) && defined(HWCAP_AES)
    return !!(getauxval(AT_HWCAP) & HWCAP_AES);
#elif defined(__linux__) && defined(__arm__) && defined(HWCAP2_AES)
    return !!(getauxval(AT_HWCAP2) & HWCAP2_AES);
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
    __builtin_cpu_init();
    return !!__builtin_cpu_supports("aes");
#else
    return -1;
#endif
}

#if defined(__linux__)
// The field'th whitespace-separated integer of a small /proc file.
static int64_t read_proc_value(const char *path, int field)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    long long value = -1;
    for (int n = 0; n <= field; n++) {
        if (fscanf(f, "%lld", &value) != 1) {
            value = -1;
            break;
        }
    }
    fclose(f);
    return value >= 0 ? value : -1;
}
#endif

#if defined(__APPLE__)
static int64_t read_sysctl_value(const char *name)
{
    uint64_t value = 0; // a 4-byte answer fills the low half
    size_t len = sizeof(value);
    if (sysctlbyname(name, &value, &len, NULL, 0) != 0 ||
        (len != 4 && len != 8))
    {
        return -1;
    }
    return len == 4 ? (int64_t)(uint32_t)value : (int64_t)value;
}
#endif

static void probe_host_limits(struct curl_ctx *ctx)
{
    ctx->host_rmem_max = ctx->host_autotune_max = -1;
#if defined(__linux__)
    ctx->host_rmem_max = read_proc_value("/proc/sys/net/core/rmem_max", 0);
    ctx->host_autotune_max = read_proc_value("/proc/sys/net/ipv4/tcp_rmem", 2);
#elif defined(__APPLE__)
    ctx->host_rmem_max = read_sysctl_value("kern.ipc.maxsockbuf");
    ctx->host_autotune_max = read_sysctl_value("net.inet.tcp.autorcvbufmax");
#endif
    ctx->hw_aes = detect_hw_aes();
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++)
        ctx->sockets[n].fd = CURL_SOCKET_BAD;
}

#if HAVE_SOCKET_PROBES
static curl_socket_t open_socket_cb(void *clientp, curlsocktype purpose,
                                    struct curl_sockaddr *address)
{
    struct curl_ctx *ctx = clientp;
    int type = address->socktype;
    // libcurl may already have tucked creation flags into the type.
    int kind = type;
#ifdef SOCK_CLOEXEC
    kind &= ~SOCK_CLOEXEC;
    type |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
    kind &= ~SOCK_NONBLOCK;
#endif
    curl_socket_t fd = socket(address->family, type, address->protocol);
    if (fd == CURL_SOCKET_BAD)
        return fd;
    ctx->sockets_opened++;
    // A full table only loses observation, never the connection.
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        struct curl_socket_info *info = &ctx->sockets[n];
        if (info->fd == CURL_SOCKET_BAD) {
            *info = (struct curl_socket_info){
                .fd = fd,
                .tcp = kind == SOCK_STREAM,
                .receive_buffer = -1,
                .rtt_ms = -1,
            };
            break;
        }
    }
    return fd;
}

static int close_socket_cb(void *clientp, curl_socket_t fd)
{
    struct curl_ctx *ctx = clientp;
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        if (ctx->sockets[n].fd == fd)
            ctx->sockets[n].fd = CURL_SOCKET_BAD;
    }
    return close(fd);
}

static void sample_sockets(struct curl_ctx *ctx)
{
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        struct curl_socket_info *info = &ctx->sockets[n];
        if (info->fd == CURL_SOCKET_BAD)
            continue;
        int buffer = 0;
        socklen_t len = sizeof(buffer);
        info->receive_buffer =
            getsockopt(info->fd, SOL_SOCKET, SO_RCVBUF, &buffer, &len) == 0
            ? buffer : -1;
        info->rtt_ms = -1;
        if (!info->tcp)
            continue;
#if defined(__linux__) && defined(TCP_INFO)
        // Old SDKs have a shorter struct; trust only fields the kernel filled.
        struct tcp_info tcp = {0};
        len = sizeof(tcp);
        if (getsockopt(info->fd, IPPROTO_TCP, TCP_INFO, &tcp, &len) == 0 &&
            len >= offsetof(struct tcp_info, tcpi_rtt) + sizeof(tcp.tcpi_rtt))
        {
            info->rtt_ms = tcp.tcpi_rtt / 1000.0;
        }
#elif defined(__APPLE__) && defined(TCP_CONNECTION_INFO)
        struct tcp_connection_info tcp = {0};
        len = sizeof(tcp);
        if (getsockopt(info->fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &tcp, &len) == 0)
            info->rtt_ms = tcp.tcpi_srtt;
#endif
    }
}
#endif

static MP_THREAD_VOID curl_thread(void *arg)
{
    mp_thread_set_name("curl");
    struct curl_ctx *ctx = arg;
    ctx->thread_started_ns = mp_time_ns();
    ctx->thread_cpu_started_ns =
        mp_thread_cpu_time_ns(mp_thread_current_id());
    probe_host_limits(ctx);

    CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
    if (res == CURLE_OK) {
        ctx->multi = curl_multi_init();
        if (!ctx->multi) {
            res = CURLE_OUT_OF_MEMORY;
            curl_global_cleanup();
        }
    }
    if (res != CURLE_OK) {
        set_failed(ctx, curl_easy_strerror(res));
    } else {
        curl_multi_setopt(ctx->multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
        ctx->share = acquire_share();
        mp_dispatch_set_wakeup_fn(ctx->dispatch, curl_wakeup, ctx);
    }

    while (!ctx->exit) {
        // libcurl is unusable, but commands are still sent to this thread, so
        // keep serving them until CMD_EXIT, or their senders would wait forever.
        if (atomic_load_explicit(&ctx->failed, memory_order_relaxed)) {
            mp_dispatch_queue_process(ctx->dispatch, INFINITY);
            continue;
        }

        mp_dispatch_queue_process(ctx->dispatch, 0);
        apply_playback_state(ctx);
#if HAVE_SOCKET_PROBES
        if (mp_time_ns() >= ctx->next_socket_sample_ns) {
            ctx->next_socket_sample_ns = mp_time_ns() + MP_TIME_MS_TO_NS(500);
            sample_sockets(ctx);
        }
#endif

        // Stop early to avoid delays, this happens only when player is closing.
        if (ctx->exit)
            break;

        int running = 0;
        CURLMcode mres = curl_multi_perform(ctx->multi, &running);
        if (mres != CURLM_OK && mres != CURLM_CALL_MULTI_PERFORM) {
            set_failed(ctx, curl_multi_strerror(mres));
            continue;
        }

        CURLMsg *msg;
        int left = 0;
        while ((msg = curl_multi_info_read(ctx->multi, &left))) {
            if (msg->msg != CURLMSG_DONE)
                continue;
            struct curl_handle_ref *ref = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &ref);
            mp_assert(ref && ref->p);
            bool succeeded = msg->data.result == CURLE_OK;
            if (ref->kind == CURL_HANDLE_PRIMARY) {
                succeeded = succeeded && ref->p->response_ok &&
                            !atomic_load_explicit(&ref->p->aborted,
                                                  memory_order_relaxed);
            } else {
                struct dynamic_request *request = (struct dynamic_request *)ref;
                succeeded = succeeded && request->response_ok &&
                            request->range &&
                            request->fetch_pos ==
                                request->range->end - request->range->start &&
                            !atomic_load_explicit(&request->ref.p->aborted,
                                                  memory_order_relaxed);
            }
            remove_curl_handle(ctx, msg->easy_handle);
            end_measurement(ref, succeeded);
            finish_curl_request(ctx, succeeded);
            if (ref->kind == CURL_HANDLE_PRIMARY) {
                ref->p->active = false;
                on_done(ref->p, msg->data.result);
            } else {
                struct dynamic_request *request = (struct dynamic_request *)ref;
                request->active = false;
                on_dynamic_done(request, msg->data.result);
            }
        }

        rate_current(&ctx->traffic, mp_time_ns());
        for (int n = 0; n < MP_CURL_MAX_LANES; n++)
            rate_current(&ctx->lanes[n], mp_time_ns());
        // Wait until libcurl's own next deadline (at most 250 ms). A wait
        // that runs to its deadline and returns late measures how long this
        // thread waited for a CPU, the jitter a receive buffer must absorb.
        long timeout = -1;
        curl_multi_timeout(ctx->multi, &timeout);
        if (timeout < 0 || timeout > 250)
            timeout = 250;
        int64_t deadline = mp_time_ns() + timeout * INT64_C(1000000);
        int ready = 0;
        curl_multi_poll(ctx->multi, NULL, 0, timeout, &ready);
        int64_t late = mp_time_ns() - deadline;
        if (!ready && late >= 0) {
            atomic_store_explicit(&ctx->recent_lateness_ns,
                MPMAX(late, atomic_load_explicit(&ctx->recent_lateness_ns,
                                                 memory_order_relaxed)),
                memory_order_relaxed);
            if (late > ctx->peak_lateness_ns)
                ctx->peak_lateness_ns = late;
        }
    }

    if (ctx->multi) {
        curl_multi_cleanup(ctx->multi);
        if (ctx->share)
            release_share();
        curl_global_cleanup();
    }
    MP_THREAD_RETURN();
}

static void mp_curl_destroy(void *ptr)
{
    struct curl_ctx *ctx = ptr;
    struct cmd c = { .kind = CMD_EXIT, .ctx = ctx };
    mp_dispatch_run(ctx->dispatch, run_cmd, &c);
    mp_thread_join(ctx->thread);
    mp_mutex_destroy(&ctx->state_lock);
}

void mp_curl_global_init(struct mpv_global *global)
{
    struct curl_ctx *ctx = talloc_zero(global, struct curl_ctx);
    mp_mutex_init(&ctx->state_lock);
    talloc_set_destructor(ctx, mp_curl_destroy);
    ctx->dispatch = mp_dispatch_create(ctx);
    global->curl = ctx;
    mp_require(!mp_thread_create(&ctx->thread, curl_thread, ctx));
}

struct metrics_snapshot {
    struct curl_ctx *ctx;
    struct mp_curl_metrics *metrics;
};

// Multiplexed HTTP/2 streams share a connection, so a request count is not
// a connection count. Runs on the curl thread.
static int64_t count_connections(struct curl_ctx *ctx)
{
    CURL **handles = ctx->multi ? curl_multi_get_handles(ctx->multi) : NULL;
    if (!handles)
        return 0;
    curl_off_t seen[MP_CURL_MAX_LANES * 2 + 2];
    int count = 0;
    for (int n = 0; handles[n]; n++) {
        curl_off_t id = -1;
        if (curl_easy_getinfo(handles[n], CURLINFO_CONN_ID, &id) != CURLE_OK ||
            id < 0)
        {
            continue;
        }
        bool known = false;
        for (int i = 0; i < count; i++)
            known |= seen[i] == id;
        if (!known && count < MP_ARRAY_SIZE(seen))
            seen[count++] = id;
    }
    curl_free(handles);
    return count;
}

static void read_socket_metrics(struct curl_ctx *ctx,
                                struct mp_curl_metrics *metrics)
{
    metrics->tcp_rtt_min_ms = metrics->tcp_rtt_max_ms = -1;
    metrics->max_socket_receive_buffer = -1;
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        const struct curl_socket_info *info = &ctx->sockets[n];
        if (info->fd == CURL_SOCKET_BAD)
            continue;
        metrics->open_sockets++;
        metrics->max_socket_receive_buffer =
            MPMAX(metrics->max_socket_receive_buffer, info->receive_buffer);
        if (info->rtt_ms >= 0) {
            metrics->tcp_rtt_min_ms = metrics->tcp_rtt_min_ms < 0
                ? info->rtt_ms : MPMIN(metrics->tcp_rtt_min_ms, info->rtt_ms);
            metrics->tcp_rtt_max_ms = MPMAX(metrics->tcp_rtt_max_ms, info->rtt_ms);
        }
    }
    metrics->sockets_opened = ctx->sockets_opened;
    metrics->host_rmem_max = ctx->host_rmem_max;
    metrics->host_autotune_max = ctx->host_autotune_max;
    metrics->hw_aes = ctx->hw_aes;
}

static void read_metrics(void *arg)
{
    struct metrics_snapshot *snapshot = arg;
    struct curl_ctx *ctx = snapshot->ctx;
    struct mp_curl_metrics *metrics = snapshot->metrics;
    int64_t now = mp_time_ns();
    int64_t cpu = mp_thread_cpu_time_ns(mp_thread_current_id());
    bool cpu_available = cpu >= 0 &&
                         cpu >= MPMAX(ctx->thread_cpu_started_ns, 0);
    double elapsed = ctx->traffic.started_ns
        ? MP_TIME_NS_TO_S(now - ctx->traffic.started_ns) : 0;
    int64_t current = rate_current(&ctx->traffic, now);
    *metrics = (struct mp_curl_metrics) {
        .active_requests =
            atomic_load_explicit(&ctx->active_requests, memory_order_relaxed),
        .peak_active_requests =
            atomic_load_explicit(&ctx->peak_active_requests,
                                 memory_order_relaxed),
        .active_streams =
            atomic_load_explicit(&ctx->active_streams, memory_order_relaxed),
        .peak_active_streams =
            atomic_load_explicit(&ctx->peak_active_streams,
                                 memory_order_relaxed),
        .requests_per_stream =
            atomic_load_explicit(&ctx->requests_per_stream,
                                 memory_order_relaxed),
        .range_bytes =
            atomic_load_explicit(&ctx->range_bytes, memory_order_relaxed),
        .ring_bytes =
            atomic_load_explicit(&ctx->ring_bytes, memory_order_relaxed),
        .started_requests =
            atomic_load_explicit(&ctx->started_requests, memory_order_relaxed),
        .finished_requests =
            atomic_load_explicit(&ctx->finished_requests, memory_order_relaxed),
        .failed_requests =
            atomic_load_explicit(&ctx->failed_requests, memory_order_relaxed),
        .retry_attempts =
            atomic_load_explicit(&ctx->retry_attempts, memory_order_relaxed),
        .received_bytes = ctx->traffic.bytes,
        .average_received_bytes_per_second =
            elapsed > 0 ? ctx->traffic.bytes / elapsed : 0,
        .peak_received_bytes_per_second = ctx->traffic.peak,
        .current_received_bytes_per_second = current,
        .elapsed_seconds = elapsed,
        .curl_thread_cpu_seconds = cpu_available
            ? MP_TIME_NS_TO_S(cpu - MPMAX(ctx->thread_cpu_started_ns, 0)) : 0,
        .curl_thread_wall_seconds =
            MP_TIME_NS_TO_S(now - ctx->thread_started_ns),
        .curl_thread_cpu_available = cpu_available,
        .ring_wait_seconds = MP_TIME_NS_TO_S(ctx->ring_wait.active_ns +
            ctx->ring_wait.users * (now - ctx->ring_wait.active_started_ns)),
        .consumer_wait_seconds = MP_TIME_NS_TO_S(atomic_load_explicit(
            &ctx->consumer_wait_ns, memory_order_relaxed)),
        .head_of_line_seconds = MP_TIME_NS_TO_S(atomic_load_explicit(
            &ctx->head_of_line_ns, memory_order_relaxed)),
        .peak_reorder_bytes = atomic_load_explicit(&ctx->peak_reorder_bytes,
                                                   memory_order_relaxed),
        .physical_connections = count_connections(ctx),
        .playback_state_updates = ctx->state_updates,
        .playback_buffer_seconds = ctx->buffer_seconds,
        .playback_demand_bitrate = MPMAX(0, ctx->media_bitrate),
        .playback_user_paused = ctx->playback_paused,
        .recent_lateness_ms = atomic_exchange_explicit(&ctx->recent_lateness_ns,
            0, memory_order_relaxed) / 1e6,
        .peak_lateness_ms = ctx->peak_lateness_ns / 1e6,
        .selected_requests = ctx->selected_requests,
        .latency_ms = ctx->latency_ms,
        .capacity_bytes_per_second = ctx->capacity,
        .capacity_samples = ctx->capacity_samples,
        .stream_id = ctx->stream_id,
    };
    read_socket_metrics(ctx, metrics);
    for (int n = 0; n < MP_CURL_MAX_LANES; n++) {
        struct curl_rate *lane = &ctx->lanes[n];
        double active = MP_TIME_NS_TO_S(lane->active_ns +
            lane->users * (now - lane->active_started_ns));
        int64_t lane_current = rate_current(lane, now);
        metrics->lanes[n] = (struct mp_curl_lane_metrics) {
            .received_bytes = lane->bytes,
            .active_seconds = active,
            .average_bytes_per_second = active > 0 ? lane->bytes / active : 0,
            .current_bytes_per_second = lane_current,
            .peak_bytes_per_second = lane->peak,
            .capacity_bytes_per_second = ctx->lane_capacity[n],
            .samples = ctx->lane_samples[n],
        };
        metrics->concurrency[n] = ctx->concurrency[n];
    }
}

bool mp_curl_get_metrics(struct mpv_global *global,
                         struct mp_curl_metrics *metrics)
{
    if (!global || !global->curl || !metrics)
        return false;
    struct metrics_snapshot snapshot = {global->curl, metrics};
    mp_dispatch_run(global->curl->dispatch, read_metrics, &snapshot);
    return true;
}

struct curl_control {
    struct curl_ctx *ctx;
    double media_bitrate;
    double buffer_seconds;
    bool paused;
    int64_t buffer_generation;
};

static void set_playback_paused(struct curl_ctx *ctx, bool paused)
{
    if (ctx->playback_paused != paused) {
        int64_t now = mp_time_ns();
        if (paused) {
            ctx->pause_started_ns = now;
        } else {
            ctx->paused_ns += now - ctx->pause_started_ns;
        }
        ctx->pause_serial++;
    }
    ctx->playback_paused = paused;
}

static void set_control(void *arg)
{
    struct curl_control *control = arg;
    struct curl_ctx *ctx = control->ctx;
    ctx->declared_bitrate = control->media_bitrate;
    if (ctx->player_state) {
        ctx->media_bitrate = MPMAX(ctx->observed_bitrate, ctx->declared_bitrate);
        return;
    }
    ctx->buffer_generation = control->buffer_generation;
    set_playback_paused(ctx, control->paused);
    ctx->media_bitrate = control->media_bitrate;
    ctx->buffer_seconds = control->buffer_seconds;
}

// Take the player's latest state. Demand is the larger of observed
// consumption and the declared media bitrate: a short demuxer window can
// read below a file's average. A user pause is not a cache pause.
static void apply_playback_state(struct curl_ctx *ctx)
{
    mp_mutex_lock(&ctx->state_lock);
    bool fresh = ctx->state_pending;
    struct mp_curl_playback_state state = ctx->pending_state;
    ctx->state_pending = false;
    mp_mutex_unlock(&ctx->state_lock);
    if (!fresh)
        return;
    ctx->player_state = true;
    ctx->state_updates++;
    ctx->buffer_generation = ctx->state_updates;
    if (state.seeking != ctx->playback_seeking)
        ctx->pause_serial++; // a seek invalidates in-flight capacity evidence
    ctx->playback_seeking = state.seeking;
    set_playback_paused(ctx, state.user_paused);
    ctx->buffer_seconds = MPMAX(0, state.buffer_seconds);
    double observed = MPMAX(0, state.audio_bitrate) +
                      MPMAX(0, state.video_bitrate);
    ctx->observed_bitrate = observed;
    ctx->media_bitrate = MPMAX(observed, ctx->declared_bitrate);
}

void mp_curl_set_playback_state(struct mpv_global *global,
                                const struct mp_curl_playback_state *state)
{
    if (!global || !global->curl)
        return;
    struct curl_ctx *ctx = global->curl;
    mp_mutex_lock(&ctx->state_lock);
    ctx->pending_state = *state;
    ctx->state_pending = true;
    mp_mutex_unlock(&ctx->state_lock);
}

bool mp_curl_has_streams(struct mpv_global *global)
{
    return global && global->curl &&
           atomic_load_explicit(&global->curl->active_streams,
                                memory_order_relaxed) > 0;
}

void mp_curl_set_control(struct mpv_global *global, double media_bitrate,
                         double buffer_seconds, bool paused,
                         int64_t buffer_generation)
{
    if (!global || !global->curl)
        return;
    struct curl_control control = {
        global->curl, media_bitrate, buffer_seconds, paused, buffer_generation,
    };
    mp_dispatch_run(global->curl->dispatch, set_control, &control);
}

// Curl callbacks

static bool is_http_success(long resp)
{
    return resp >= 200 && resp < 300;
}

// Append `len` bytes to the ring buffer. Caller must hold p->mtx and have
// verified that there is enough free space.
static void ring_write(void *ctx, const char *data, size_t len)
{
    struct priv *p = ctx;
    size_t tail_chunk = MPMIN(p->buffer_size - p->tail, len);
    memcpy(p->buffer + p->tail, data, tail_chunk);
    memcpy(p->buffer, data + tail_chunk, len - tail_chunk);
    p->tail = (p->tail + len) % p->buffer_size;
    p->count += len;
}


// Called per chunk of body data.
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct priv *p = userdata;
    size_t bytes = size * nmemb;

    // Reject error bodies and servers that returned the wrong byte range.
    if (!p->response_ok)
        return CURL_WRITEFUNC_ERROR;

    if (atomic_load_explicit(&p->aborted, memory_order_relaxed))
        return CURL_WRITEFUNC_ERROR;
    // libcurl replays a paused body callback. Account the fresh download at
    // first arrival (including a chunk that cannot enter the ring yet), never
    // the replay or the later ordered-buffer memcpy.
    if (!p->paused_bytes)
        record_received_bytes(&p->primary_ref, bytes);

    mp_mutex_lock(&p->mtx);

    if (p->buffer_size - p->count < bytes) {
        // No room in the buffer. Pause the transfer and wait for the consumer.
        int64_t now = mp_time_ns();
        learning_tick(p, now);
        begin_ring_wait(p, now);
        p->paused = true;
        p->paused_bytes = bytes;
        p->primary_ref.blocked = true;
        mp_mutex_unlock(&p->mtx);
        MP_TRACE(p, "pausing curl transfer, buffer full (%zu bytes)\n", p->count);
        return CURL_WRITEFUNC_PAUSE;
    }

    mp_icy_process(p->icy, ptr, bytes, ring_write, p);

    p->paused = false;
    p->request_received += bytes;
    if (!p->startup && p->primary_ref.generation == p->generation)
        p->phase_delivered += bytes;
    p->paused_bytes = 0;

    if (p->consumer_waiting)
        mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);

    return bytes;
}

static int xferinfo_callback(void *userdata, curl_off_t dl_total, curl_off_t dl_now,
                             curl_off_t ul_total, curl_off_t ul_now)
{
    struct priv *p = userdata;
    return atomic_load_explicit(&p->aborted, memory_order_relaxed);
}

static int64_t parse_content_range_total(const char *value)
{
    if (!value)
        return -1;
    bstr after;
    if (!bstr_split_tok(bstr0(value), "/", &(bstr){0}, &after))
        return -1;
    bstr rest;
    long long total = bstrtoll(after, &rest, 10);
    return (rest.len == 0 && total > 0) ? (int64_t)total : -1;
}

static bool parse_exact_content_range(const char *value, uint64_t *start,
                                      uint64_t *end, int64_t *total)
{
    unsigned long long first = 0;
    unsigned long long last = 0;
    long long size = 0;
    char trailing = '\0';
    if (!value ||
        sscanf(value, "bytes %llu-%llu/%lld %c",
               &first, &last, &size, &trailing) != 3 ||
        last < first || size <= 0 || last >= (unsigned long long)size)
    {
        return false;
    }
    *start = first;
    *end = last + 1;
    *total = size;
    return true;
}

// "bytes <start>-<end>/<total>" where <total> may be "*". end is exclusive;
// total is -1 when unknown.
static bool parse_content_range(const char *value, uint64_t *start,
                                uint64_t *end, int64_t *total)
{
    unsigned long long first = 0;
    unsigned long long last = 0;
    int consumed = 0;
    if (!value ||
        sscanf(value, "bytes %llu-%llu/%n", &first, &last, &consumed) != 2 ||
        !consumed || last < first)
    {
        return false;
    }
    bstr rest = bstr_strip(bstr0(value + consumed));
    long long size = -1;
    if (!bstr_equals0(rest, "*")) {
        bstr tail;
        size = bstrtoll(rest, &tail, 10);
        if (!rest.len || tail.len || size < 0 || last >= (unsigned long long)size)
            return false;
    }
    *start = first;
    *end = last + 1;
    *total = size;
    return true;
}

// Extend the contiguous published prefix of the queue. Called with mtx held.
static uint64_t advance_ordered(struct priv *p)
{
    uint64_t before = p->ordered_pos;
    for (int n = 0; n < p->range_count; n++) {
        struct range_desc *range =
            &p->ranges[(p->range_head + n) % p->num_ranges];
        if (range->end <= p->ordered_pos)
            continue;
        if (range->start > p->ordered_pos)
            break;
        p->ordered_pos = range->start + range->published;
        if (!range->complete)
            break;
    }
    return p->ordered_pos - before;
}

// Make a range's stored prefix readable. Any validated prefix may be
// published: a retry either resumes after it under the same strong validator
// or re-reads it and must reproduce it byte for byte.
static void publish_range(struct priv *p, struct range_desc *range)
{
    mp_mutex_lock(&p->mtx);
    p->queued_bytes += range->received - range->published;
    range->published = range->received;
    range->complete = range->received == range->end - range->start;
    uint64_t ordered = advance_ordered(p);
    int64_t reorder = p->queued_bytes - (p->ordered_pos - p->queue_pos);
    int64_t peak = atomic_load_explicit(&p->ctx->peak_reorder_bytes,
                                        memory_order_relaxed);
    while (reorder > peak &&
           !atomic_compare_exchange_weak_explicit(&p->ctx->peak_reorder_bytes,
                &peak, reorder, memory_order_relaxed, memory_order_relaxed))
    {
    }
    if (ordered && !p->startup && range->request &&
        range->request->ref.generation == p->generation)
    {
        p->phase_delivered += ordered;
    }
    if (ordered && p->queue_open && p->consumer_waiting)
        mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

static size_t dynamic_write_callback(char *ptr, size_t size, size_t nmemb,
                                     void *userdata)
{
    struct dynamic_request *request = userdata;
    struct priv *p = request->ref.p;
    struct range_desc *range = request->range;
    size_t bytes = size * nmemb;

    if (!range || !request->response_ok ||
        atomic_load_explicit(&p->aborted, memory_order_relaxed) ||
        bytes > range->end - range->start - request->fetch_pos)
    {
        return CURL_WRITEFUNC_ERROR;
    }
    record_received_bytes(&request->ref, bytes);

    // A retry without a strong validator re-reads bytes the consumer may
    // already have taken; they must be identical, or the file changed.
    size_t pos = request->fetch_pos;
    size_t overlap = pos < range->received
        ? MPMIN(bytes, range->received - pos) : 0;
    if (overlap && memcmp(range->data + pos, ptr, overlap) != 0) {
        request->changed = true;
        return CURL_WRITEFUNC_ERROR;
    }
    memcpy(range->data + pos + overlap, ptr + overlap, bytes - overlap);
    request->fetch_pos = pos + bytes;
    if (request->fetch_pos > range->received) {
        range->received = request->fetch_pos;
        publish_range(p, range);
    }
    return bytes;
}

static int dynamic_xferinfo_callback(void *userdata, curl_off_t dl_total,
                                     curl_off_t dl_now, curl_off_t ul_total,
                                     curl_off_t ul_now)
{
    struct dynamic_request *request = userdata;
    return atomic_load_explicit(&request->ref.p->aborted, memory_order_relaxed);
}
static const char *header_value(CURL *c, const char *name)
{
    struct curl_header *h = NULL;
    if (curl_easy_header(c, name, 0, CURLH_HEADER, -1, &h) == CURLHE_OK)
        return h->value;
    return NULL;
}

static int64_t parse_content_range_start(CURL *c)
{
    const char *value = header_value(c, "Content-Range");
    if (!value)
        return -1;
    bstr range;
    if (!bstr_split_tok(bstr0(value), "/", &range, &(bstr){0}))
        return -1;
    bstr_eatstart0(&range, "bytes");
    bstr start;
    if (!bstr_split_tok(bstr_lstrip(range), "-", &start, &(bstr){0}))
        return -1;
    bstr rest;
    long long value_start = bstrtoll(start, &rest, 10);
    return start.len > 0 && rest.len == 0 && value_start >= 0
        ? value_start : -1;
}


static size_t dynamic_header_callback(char *buffer, size_t size, size_t nitems,
                                      void *userdata)
{
    struct dynamic_request *request = userdata;
    struct priv *p = request->ref.p;
    size_t bytes = size * nitems;
    struct bstr line = bstr_strip_linebreaks((bstr){buffer, bytes});
    if (line.len > 0)
        return bytes;

    long resp = 0;
    curl_easy_getinfo(request->curl, CURLINFO_RESPONSE_CODE, &resp);
    if (resp < 200 || (resp >= 300 && resp < 400))
        return bytes;

    struct range_desc *range = request->range;
    if (!range)
        return bytes;
    uint64_t first = range->start + request->fetch_pos;
    uint64_t start = 0;
    uint64_t end = 0;
    int64_t total = -1;
    // A resumed suffix is only spliced onto published bytes under the same
    // strong validator; a fresh or re-read range may omit it.
    const char *etag = header_value(request->curl, "ETag");
    bool same_entity = !p->etag ||
        (etag ? strcmp(etag, p->etag) == 0 : request->fetch_pos == 0);
    bool valid =
        resp == 206 &&
        parse_exact_content_range(header_value(request->curl, "Content-Range"),
                            &start, &end, &total) &&
        start == first && end == range->end &&
        total == p->content_size && same_entity;
    if (!valid) {
        MP_WARN(p, "parallel range %" PRIu64 "-%" PRIu64
                   " rejected (HTTP %ld)\n", first, range->end - 1, resp);
    }
    request->response_ok = valid;
    return bytes;
}

static void finalize_probe(struct priv *p)
{
    if (mp_msg_test(p->log, MSGL_DEBUG)) {
        long resp = 0;
        char *ctype = NULL;
        curl_easy_getinfo(p->curl, CURLINFO_RESPONSE_CODE, &resp);
        curl_easy_getinfo(p->curl, CURLINFO_CONTENT_TYPE, &ctype);
        MP_DBG(p, "proto=%.*s ok=%d code=%ld size=%" PRId64 " seekable=%d type=%s\n",
               BSTR_P(p->scheme->scheme), p->stream_ok, resp,
               p->content_size, p->seekable, ctype ? ctype : "-");
    }

    mp_mutex_lock(&p->mtx);
    p->probed = true;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

// Empty line is the end of the header. Skip intermediate 1xx and 3xx responses,
// we care about the final one.
static void probe_http(struct priv *p, struct bstr line)
{
    bool schedule_dynamic = false;
    if (line.len > 0) {
        // A new status line resets per-response state so that intermediate
        // 1xx/3xx responses don't leak ICY metadata into the final one.
        mp_mutex_lock(&p->mtx);
        if (bstr_startswith0(line, "HTTP/")) {
            mp_icy_reset(p->icy);
        } else {
            mp_icy_add_header(p->icy, line);
        }
        mp_mutex_unlock(&p->mtx);
        return;
    }

    long resp = 0;
    curl_easy_getinfo(p->curl, CURLINFO_RESPONSE_CODE, &resp);
    if (resp < 200 || (resp >= 300 && resp < 400))
        return;

    // The first range of an empty file is not satisfiable. Its body is
    // dropped and on_done() ends the stream.
    if (resp == 416 && p->request_start == 0 &&
        parse_content_range_total(header_value(p->curl, "Content-Range")) <= 0)
    {
        p->seekable = true;
        p->content_size = 0;
        p->reply_eof = true;
        p->stream_ok = true;
        goto done;
    }

    if (!is_http_success(resp)) {
        MP_ERR(p, "HTTP error %ld\n", resp);
        goto done;
    }
    p->reply_ranged = resp == 206;

    int64_t response_start = parse_content_range_start(p->curl);

    // A request with an explicit start offset must be honored by the server
    if (p->start_offset > 0 && response_start != p->start_offset) {
        MP_ERR(p, "Server ignored range request at offset %" PRId64 "\n",
               p->start_offset);
        goto done;
    }

    // Compressed responses are byte-addressed in the encoded representation,
    // which our caller can't translate, so they are non-seekable.
    const char *ce = header_value(p->curl, "Content-Encoding");
    bool compressed = ce && ce[0] && strcasecmp(ce, "identity") != 0;
    const char *ar = header_value(p->curl, "Accept-Ranges");
    bool accept_ranges = ar && strcasecmp(ar, "bytes") == 0;

    // Some servers reply 200 to an open-ended "Range: 0-" but 206 to explicit
    // byte ranges, so trust either. ICY metadata is interleaved with the body,
    // so byte ranges from the server don't line up with consumer offsets.
    p->seekable = !compressed && !mp_icy_active(p->icy) &&
                  (resp == 206 || accept_ranges);

    uint64_t range_start = 0;
    uint64_t range_end = 0;
    int64_t total = -1;
    bool exact_range =
        resp == 206 &&
        parse_exact_content_range(header_value(p->curl, "Content-Range"),
                            &range_start, &range_end, &total);
    if (p->seekable) {
        if (exact_range) {
            p->content_size = total;
            p->primary_end = range_end;
        } else {
            // On any non-206 success code use Content-Length.
            if (resp != 206) {
                curl_off_t cl = -1;
                if (curl_easy_getinfo(p->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                                      &cl) == CURLE_OK && cl >= 0)
                    total = cl;
            } else {
                total = parse_content_range_total(
                    header_value(p->curl, "Content-Range"));
            }
            p->content_size = total;
        }
    }

    const char *cd = header_value(p->curl, "Content-Disposition");
    if (cd)
        p->server_filename = mp_parse_content_disposition(p, p->s, bstr0(cd));
    // Weak validators (W/"...") do not promise byte-identical ranges.
    const char *etag = header_value(p->curl, "ETag");
    talloc_free(p->etag);
    p->etag = etag && etag[0] == '"' ? talloc_strdup(p, etag) : NULL;

    p->stream_ok = true;
    p->response_ok = true;
    schedule_dynamic = p->seekable && exact_range &&
                       range_start == p->request_start &&
                       p->opts->max_request_size > 0;
    p->dynamic_supported = schedule_dynamic;
done:
    finalize_probe(p);
    if (p->stream_ok && schedule_dynamic)
        cmd_async(p, CMD_START_DYNAMIC);
}

static void probe_ftp(struct priv *p, struct bstr line)
{
    if (line.len < 4 || line.start[3] != ' ')
        return;
    // Parse the line directly: libcurl only stamps CURLINFO_RESPONSE_CODE after
    // a reply is fully processed, so polling it from header_callback returns
    // the previous code.
    struct bstr code = {line.start, 3};
    if (!bstr_equals0(code, "150") && !bstr_equals0(code, "125"))
        return;

    curl_off_t cl = -1;
    if (curl_easy_getinfo(p->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                          &cl) == CURLE_OK && cl >= 0)
        p->content_size = cl;

    p->seekable = p->content_size > 0;
    p->stream_ok = true;
    p->response_ok = true;
    finalize_probe(p);
}

static void validate_http_response(struct priv *p, struct bstr line)
{
    if (line.len > 0)
        return;

    long resp = 0;
    curl_easy_getinfo(p->curl, CURLINFO_RESPONSE_CODE, &resp);
    if (resp < 200 || (resp >= 300 && resp < 400))
        return;

    uint64_t start = 0;
    uint64_t end = 0;
    int64_t total = -1;
    const char *range = header_value(p->curl, "Content-Range");

    // A 416 past the end of a file of unknown size is its end, and states
    // the size if the server knows it by now. One that states a size past
    // the requested offset contradicts itself and is an error.
    if (resp == 416 && p->content_size < 0) {
        total = parse_content_range_total(range);
        if (total < 0 || total <= (int64_t)p->request_start) {
            if (total >= 0) {
                mp_mutex_lock(&p->mtx);
                p->content_size = total;
                mp_mutex_unlock(&p->mtx);
            }
            p->reply_eof = true;
            p->response_ok = false;
            return;
        }
    }

    // Servers may cap a reply short of the requested range; on_done()
    // continues from where it stopped.
    p->reply_ranged = resp == 206;
    // Continuations append to delivered bytes; a different strong validator
    // means a different file. Servers that omit it on a reply cannot be told.
    const char *etag = header_value(p->curl, "ETag");
    bool same_entity = !p->etag || !etag || strcmp(etag, p->etag) == 0;
    bool valid =
        same_entity && is_http_success(resp) &&
        (!p->seekable ||
         (resp == 206 &&
          parse_content_range(range, &start, &end, &total) &&
          start == p->request_start &&
          (!p->primary_end || end <= p->primary_end) &&
          (total == p->content_size || p->content_size < 0)));
    if (!valid) {
        MP_WARN(p, "range %" PRIu64 "-%" PRIu64
                   " rejected (HTTP %ld)\n",
                p->request_start, p->primary_end - 1, resp);
    }
    p->response_ok = valid;
}

// Called per header line.
static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    struct priv *p = userdata;
    size_t bytes = size * nitems;
    struct bstr line = bstr_strip_linebreaks((bstr){buffer, bytes});

    if (p->probed) {
        if (p->scheme->proto == MP_CURL_PROTO_HTTP)
            validate_http_response(p, line);
        return bytes;
    }

    switch (p->scheme->proto) {
    case MP_CURL_PROTO_HTTP:
        probe_http(p, line);
        break;
    case MP_CURL_PROTO_FTP:
        probe_ftp(p, line);
        break;
    default:
        break;
    }

    return bytes;
}

static int debug_callback(CURL *handle, curl_infotype type, char *data, size_t size,
                          void *userdata)
{
    struct priv *p = userdata;
    const char *prefix;
    switch (type) {
    case CURLINFO_TEXT:       prefix = "* "; break;
    case CURLINFO_HEADER_IN:  prefix = "< "; break;
    case CURLINFO_HEADER_OUT: prefix = "> "; break;
    default:                  return 0;
    }
    bstr msg = bstr_strip_linebreaks((bstr){data, size});
    MP_TRACE(p, "%s%.*s\n", prefix, BSTR_P(msg));
    return 0;
}

// Request handling

static bool is_recoverable_error(CURLcode code)
{
    switch (code) {
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_GOT_NOTHING:
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
    case CURLE_WRITE_ERROR:
        return true;
    default:
        return false;
    }
}

// The request could not be started. This also ends the probe in curl_open().
static void request_failed(struct priv *p)
{
    mp_mutex_lock(&p->mtx);
    p->probed = true;
    p->stream_error = true;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

static void start_request(struct priv *p)
{
    if (atomic_load_explicit(&p->ctx->failed, memory_order_relaxed)) {
        request_failed(p);
        return;
    }

    if (p->finished) {
        mp_mutex_lock(&p->mtx);
        p->stream_eof = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    uint64_t start = p->request_start;

    bool ranged = !p->probed || p->seekable;
    bool chunked = ranged && p->opts->max_request_size > 0;
    bool capped = ranged && p->request_end > 0;

    bool past_size = p->seekable && p->content_size >= 0 && start >= p->content_size;
    bool past_end = capped && start >= p->request_end;
    if (past_size || past_end) {
        p->finished = true;
        mp_mutex_lock(&p->mtx);
        p->stream_eof = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    char range[64];
    if (chunked || capped) {
        uint64_t end = UINT64_MAX;
        if (chunked) {
            uint64_t bytes = p->opts->max_request_size;
            if (!p->probed && p->opts->adaptive) {
                // A tiny unknown-demand probe is useful locally, but an
                // extra small request taxes high-latency origins needlessly.
                uint64_t demand = p->opts->startup_bytes > 0
                    ? p->opts->startup_bytes
                    : p->latency_ms >= 100 ? p->segment_size : 64 * 1024;
                uint64_t leading = p->latency_ms >= 100 || demand > 64 * 1024
                    ? MPMIN(p->segment_size, demand) : 64 * 1024;
                bytes = MPMIN(bytes, leading);
            }
            end = bytes - 1 > UINT64_MAX - start
                ? UINT64_MAX : start + bytes - 1;
        }
        if (p->content_size > 0)
            end = MPMIN(end, p->content_size - 1);
        if (capped)
            end = MPMIN(end, p->request_end - 1);
        if (p->dynamic_started && p->primary_end > start)
            end = MPMIN(end, p->primary_end - 1);
        snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, start, end);
        curl_easy_setopt(p->curl, CURLOPT_RANGE, range);
        p->primary_end = end == UINT64_MAX ? 0 : end + 1;
    } else if (ranged) {
        snprintf(range, sizeof(range), "%" PRIu64 "-", start);
        curl_easy_setopt(p->curl, CURLOPT_RANGE, range);
        p->primary_end = 0;
    } else {
        curl_easy_setopt(p->curl, CURLOPT_RANGE, NULL);
    }
    p->primary_done = false;
    p->response_ok = false;
    p->reply_ranged = false;
    p->reply_eof = false;

    p->request_received = 0;
    p->paused_bytes = 0;
    CURLMcode result = add_curl_handle(p->ctx, p->curl);
    if (result != CURLM_OK) {
        MP_ERR(p, "could not start curl request: %s\n",
               curl_multi_strerror(result));
        request_failed(p);
        return;
    }
    begin_measurement(&p->primary_ref);
    p->active = true;
    if (p->probed && p->dynamic_supported)
        start_dynamic(p);
}

static uint64_t dynamic_limit(const struct priv *p)
{
    uint64_t limit = p->content_size;
    if (p->request_end > 0)
        limit = MPMIN(limit, p->request_end);
    return limit;
}

static void reset_phase(struct priv *p, int64_t now)
{
    p->phase_bytes = 0;
    p->phase_delivered = 0;
    p->phase_completed = 0;
    p->phase_blocked = false;
    p->phase_active_ns = 0;
    p->learning_updated_ns = learning_time(p->ctx, now);
}

static void update_adaptation(struct priv *p)
{
    struct curl_ctx *ctx = p->ctx;
    int64_t now = mp_time_ns();
    learning_tick(p, now);
    if (p->startup || p->transitioning || ctx->playback_paused)
        return;
    if (p->phase_blocked) {
        reset_phase(p, now);
        return;
    }
    double seconds = MP_TIME_NS_TO_S(p->phase_active_ns);
    if (seconds < 1.5 || p->phase_completed < p->selected_requests * 2)
        return;

    double capacity = p->phase_bytes / seconds;
    int selected = p->selected_requests;
    if (ctx->stream_id == p->stream_id) {
        struct mp_curl_concurrency_metrics *point = &ctx->concurrency[selected - 1];
        point->capacity_bytes_per_second = point->samples
            ? (point->capacity_bytes_per_second * 3 + capacity) / 4 : capacity;
        point->latency_ms = p->latency_ms;
        point->samples++;
        ctx->capacity = point->capacity_bytes_per_second;
        ctx->capacity_samples = ++p->capacity_samples;
        ctx->latency_ms = p->latency_ms;
    }

    if (p->opts->adaptive) {
        // The per-file declared bitrate is a floor under measured demand.
        double bitrate = MPMAX(ctx->media_bitrate, p->opts->media_bitrate);
        double target = bitrate * 1.75 / 8;
        const char *reason = "trial-fewer";
        bool settled = now - p->last_adjustment_ns >= MP_TIME_S_TO_NS(3);
        // Retry rejected counts only after capacity changes materially,
        // not on a timer at an unchanged CPU/server bottleneck.
        if (now - p->rejected_ns >= MP_TIME_S_TO_NS(30) &&
            (capacity < p->rejected_capacity * 0.67 ||
             capacity > p->rejected_capacity * 1.5))
        {
            p->rejected_requests = 0;
        }
        if (now - p->rejected_lower_ns >= MP_TIME_S_TO_NS(30) &&
            (capacity < p->rejected_lower_capacity * 0.67 ||
             capacity > p->rejected_lower_capacity * 1.5))
        {
            p->rejected_lower_requests = 0;
        }
        bool can_increase = selected < p->opts->parallel_requests &&
                            selected + 1 != p->rejected_requests;
        bool can_decrease = selected > 1 &&
                            selected - 1 != p->rejected_lower_requests;
        if (p->previous_requests) {
            bool increased = selected > p->previous_requests;
            bool useful = increased
                ? capacity > p->previous_capacity * 1.12 &&
                  p->phase_delivered >= p->phase_bytes / 2
                : capacity >= p->previous_capacity * 0.90 ||
                  (target > 0 && capacity >= target);
            if (!useful) {
                reason = increased ? "reject-no-gain" : "restore-capacity";
                if (increased) {
                    p->rejected_requests = selected;
                    p->rejected_capacity = p->previous_capacity;
                    p->rejected_ns = now;
                } else {
                    p->rejected_lower_requests = selected;
                    p->rejected_lower_capacity = p->previous_capacity;
                    p->rejected_lower_ns = now;
                }
                selected = p->previous_requests;
            } else if (!increased) {
                // A lower count retained comparable (or sufficient) capacity.
                // Do not immediately climb back into the discarded count.
                p->rejected_requests = p->previous_requests;
                p->rejected_capacity = capacity;
                p->rejected_ns = now;
            }
            p->previous_requests = 0;
            p->last_adjustment_ns = now;
        } else if (settled && target > 0 && capacity < target && can_increase)
        {
            reason = "trial-more";
            p->previous_requests = selected;
            p->previous_capacity = capacity;
            selected++;
            p->last_adjustment_ns = now;
        } else if (can_decrease &&
                   (target <= 0 || capacity >= target || !can_increase) &&
                   now - p->last_adjustment_ns >= MP_TIME_S_TO_NS(6))
        {
            p->previous_requests = selected;
            p->previous_capacity = capacity;
            selected--;
            p->last_adjustment_ns = now;
        }
        if (selected != p->selected_requests) {
            MP_INFO(p, "adaptive request lanes: %d -> %d (%s, "
                       "capacity %.0f bytes/s, target %.0f bytes/s)\n",
                    p->selected_requests, selected, reason, capacity, target);
            p->selected_requests = selected;
            p->generation++;
            p->transitioning = p->learning_active;
        }
    }
    if (ctx->stream_id == p->stream_id)
        ctx->selected_requests = p->selected_requests;
    reset_phase(p, now);
}

// Start (or retry) the transfer of a range on an idle slot. With a strong
// validator a retry resumes after the bytes already stored; without one it
// re-reads the whole range and verifies the stored prefix.
static bool start_range_transfer(struct dynamic_request *request,
                                 struct range_desc *range)
{
    struct priv *p = request->ref.p;
    size_t from = p->etag ? range->received : 0;
    request->range = range;
    request->fetch_pos = from;
    request->response_ok = false;
    request->changed = false;
    range->request = request;

    char text[64];
    snprintf(text, sizeof(text), "%" PRIu64 "-%" PRIu64,
             range->start + from, range->end - 1);
    curl_easy_setopt(request->curl, CURLOPT_RANGE, text);
    CURLMcode result = add_curl_handle(p->ctx, request->curl);
    if (result != CURLM_OK) {
        request->range = NULL;
        range->request = NULL;
        MP_WARN(p, "could not schedule parallel range: %s\n",
                curl_multi_strerror(result));
        cmd_async(p, CMD_FALLBACK_DYNAMIC);
        return false;
    }
    begin_measurement(&request->ref);
    request->active = true;
    MP_TRACE(p, "prefetching range %" PRIu64 "-%" PRIu64 "\n",
             range->start + from, range->end - 1);
    return true;
}

// Queue the next range at next_schedule, or return NULL when every queue
// entry still holds unread bytes.
static struct range_desc *queue_next_range(struct priv *p)
{
    uint64_t limit = dynamic_limit(p);
    if (p->next_schedule >= limit)
        return NULL;

    // Full preallocated segments amortize range/header/TLS bookkeeping.
    // Shrinking ranges as throughput falls compounds slow-client overhead
    // and changes the workload in the middle of concurrency comparisons.
    uint64_t bytes = p->segment_size;
    if (p->startup) {
        if (p->next_schedule >= p->startup_end)
            return NULL;
        bytes = MPMIN(bytes, p->startup_end - p->next_schedule);
        uint64_t remaining = p->startup_end - p->ordered_pos;
        bytes = MPMIN(bytes, MPMAX(64 * 1024,
                         remaining / p->selected_requests));
    }

    mp_mutex_lock(&p->mtx);
    struct range_desc *range = NULL;
    if (p->range_count < p->num_ranges) {
        range = &p->ranges[(p->range_head + p->range_count) % p->num_ranges];
        p->range_count++;
        range->start = p->next_schedule;
        range->end = p->next_schedule + MPMIN(bytes, limit - p->next_schedule);
        range->published = 0;
        range->consumed = 0;
        range->complete = false;
    }
    mp_mutex_unlock(&p->mtx);
    if (!range)
        return NULL;
    range->received = 0;
    range->request = NULL;
    range->retry_count = 0;
    p->next_schedule = range->end;
    return range;
}

// Every range has been fetched and nothing remains to schedule.
static void check_dynamic_eof(struct priv *p)
{
    if (!p->dynamic_started || !p->primary_done ||
        p->next_schedule < dynamic_limit(p))
    {
        return;
    }
    mp_mutex_lock(&p->mtx);
    bool done = true;
    for (int n = 0; n < p->range_count; n++)
        done &= p->ranges[(p->range_head + n) % p->num_ranges].complete;
    if (done) {
        p->finished = true;
        p->stream_eof = true;
        mp_cond_broadcast(&p->cond);
    }
    mp_mutex_unlock(&p->mtx);
    if (done)
        end_ring_wait(p);
}

static void fill_dynamic(struct priv *p)
{
    if (p->startup) {
        int64_t now = mp_time_ns();
        if (!p->startup_end) {
            double bitrate = MPMAX(p->ctx->media_bitrate,
                                   p->opts->media_bitrate);
            double remaining_seconds = MPMAX(0,
                p->opts->startup_buffer_seconds - p->ctx->buffer_seconds);
            uint64_t demand = p->opts->startup_bytes > 0
                ? p->opts->startup_bytes
                : MPMAX(128 * 1024, bitrate / 8 * remaining_seconds);
            p->startup_end = p->startup_start +
                MPMIN(demand, UINT64_MAX - p->startup_start);
        }
        bool buffered = p->ctx->buffer_generation > p->startup_buffer_generation &&
            p->opts->startup_buffer_seconds > 0 &&
            p->ctx->buffer_seconds >= p->opts->startup_buffer_seconds;
        if (p->ordered_pos >= p->startup_end || buffered) {
            learning_tick(p, now);
            p->startup = false;
            int steady = MPMIN(p->opts->initial_requests,
                              p->opts->parallel_requests);
            p->selected_requests = steady;
            p->generation++;
            p->transitioning = p->learning_active;
            if (p->ctx->stream_id == p->stream_id)
                p->ctx->selected_requests = steady;
            reset_phase(p, now);
        }
    }
    update_adaptation(p);
    // Drain a configuration generation before beginning the next trial.
    // Otherwise a new request can contribute only half its time/body.
    if (p->transitioning)
        return;
    int scheduled = p->active;
    for (int n = 0; n < p->num_requests; n++) {
        struct dynamic_request *request = &p->requests[n];
        if (request->active && request->ref.generation != p->generation)
            return;
        scheduled += request->active;
    }
    // Storage, not ring space, bounds read-ahead: a range holds its bytes
    // until they are read, so later ranges never overtake the queue's budget.
    // While the consumer still has a range of unread bytes, wait for room to
    // refill a whole batch. Trickling one range per consumed range would
    // measure one transfer while reporting the selected count.
    mp_mutex_lock(&p->mtx);
    int room = p->num_ranges - p->range_count;
    uint64_t unread = p->ordered_pos - p->queue_pos;
    mp_mutex_unlock(&p->mtx);
    int wanted = p->selected_requests - scheduled;
    if (wanted > 0 && room < wanted && unread >= p->segment_size) {
        begin_ring_wait(p, mp_time_ns());
        return;
    }
    for (int n = 0; n < p->num_requests && scheduled < p->selected_requests; n++) {
        struct dynamic_request *request = &p->requests[n];
        if ((p->active && n == 0) || request->active)
            continue;
        struct range_desc *range = queue_next_range(p);
        if (!range) {
            if (p->next_schedule < dynamic_limit(p))
                begin_ring_wait(p, mp_time_ns());
            break;
        }
        end_ring_wait(p);
        if (!start_range_transfer(request, range))
            break;
        scheduled++;
    }
}

static void stop_dynamic(struct priv *p)
{
    end_ring_wait(p);
    // The consumer copies queue bytes without the lock; wait before the
    // storage goes away. Seek and close come from the consumer itself, but
    // a fallback is raised asynchronously on this thread.
    mp_mutex_lock(&p->mtx);
    while (p->copying) {
        p->copy_waiter = true;
        mp_cond_wait(&p->cond, &p->mtx);
    }
    p->copy_waiter = false;
    p->queue_open = false;
    p->range_head = p->range_count = 0;
    p->queued_bytes = 0;
    mp_mutex_unlock(&p->mtx);
    for (int n = 0; n < p->num_requests; n++) {
        struct dynamic_request *request = &p->requests[n];
        if (request->active) {
            remove_curl_handle(p->ctx, request->curl);
            end_measurement(&request->ref, false);
            request->active = false;
        }
        if (request->curl)
            curl_easy_cleanup(request->curl);
    }
    talloc_free(p->range_data);
    talloc_free(p->ranges);
    talloc_free(p->requests);
    p->range_data = NULL;
    p->ranges = NULL;
    p->requests = NULL;
    p->num_ranges = 0;
    p->num_requests = 0;
    p->dynamic_started = false;
}

static void start_dynamic(struct priv *p)
{
    uint64_t segment_size = p->segment_size;
    int count = p->opts->parallel_requests;
    if (p->dynamic_started || !p->dynamic_supported ||
        p->content_size <= 0 || p->primary_end < p->request_start)
    {
        return;
    }
    if (p->primary_end >= dynamic_limit(p))
        return;
    // The queue gets the same memory as the old per-request segments: one
    // ring's worth, split into as many whole ranges as fit.
    int ranges = segment_size > 0
        ? MPCLAMP(p->buffer_size / segment_size, count, MP_CURL_MAX_RANGES)
        : 0;
    if (segment_size > SIZE_MAX || !ranges ||
        (size_t)ranges > SIZE_MAX / (size_t)segment_size)
    {
        MP_WARN(p, "parallel range buffer size is too large\n");
        p->dynamic_supported = false;
        return;
    }

    size_t segment_bytes = segment_size;
    uint64_t limit = dynamic_limit(p);
    p->primary_end = MPMIN(p->primary_end, limit);
    p->next_schedule = p->primary_end;
    p->num_requests = count;
    p->num_ranges = ranges;
    p->requests = talloc_zero_array(p, struct dynamic_request, count);
    p->ranges = talloc_zero_array(p, struct range_desc, ranges);
    p->range_data = talloc_size(p, ranges * segment_bytes);
    if (!p->requests || !p->ranges || !p->range_data) {
        MP_WARN(p, "could not allocate parallel range requests\n");
        talloc_free(p->range_data);
        talloc_free(p->ranges);
        talloc_free(p->requests);
        p->range_data = NULL;
        p->ranges = NULL;
        p->requests = NULL;
        p->num_requests = p->num_ranges = 0;
        p->dynamic_supported = false;
        return;
    }
    for (int n = 0; n < ranges; n++)
        p->ranges[n].data = p->range_data + n * segment_bytes;
    mp_mutex_lock(&p->mtx);
    p->range_head = p->range_count = 0;
    p->ordered_pos = p->queue_pos = p->primary_end;
    p->queued_bytes = 0;
    p->queue_open = false;
    mp_mutex_unlock(&p->mtx);
    p->dynamic_started = true;

    for (int n = 0; n < count; n++) {
        struct dynamic_request *request = &p->requests[n];
        request->ref.kind = CURL_HANDLE_DYNAMIC;
        request->ref.p = p;
        request->ref.lane = n;
        request->curl = curl_easy_init();
        if (!request->curl) {
            MP_WARN(p, "could not allocate parallel range request\n");
            p->dynamic_supported = false;
            stop_dynamic(p);
            return;
        }
        setup_curl(p, request->curl);
        curl_easy_setopt(request->curl, CURLOPT_PRIVATE, &request->ref);
        curl_easy_setopt(request->curl, CURLOPT_WRITEFUNCTION,
                         dynamic_write_callback);
        curl_easy_setopt(request->curl, CURLOPT_WRITEDATA, request);
        curl_easy_setopt(request->curl, CURLOPT_HEADERFUNCTION,
                         dynamic_header_callback);
        curl_easy_setopt(request->curl, CURLOPT_HEADERDATA, request);
        curl_easy_setopt(request->curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(request->curl, CURLOPT_XFERINFOFUNCTION,
                         dynamic_xferinfo_callback);
        curl_easy_setopt(request->curl, CURLOPT_XFERINFODATA, request);
    }

    fill_dynamic(p);

    MP_VERBOSE(p, "parallel ranged streaming enabled: %d requests, "
                  "%d queued %" PRIu64 " byte ranges\n",
               count, ranges, segment_size);
}

// The primary request has delivered everything before the queue: let the
// consumer read on into it.
static void open_queue(struct priv *p)
{
    mp_mutex_lock(&p->mtx);
    p->queue_open = true;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
    check_dynamic_eof(p);
}

static void fallback_dynamic(struct priv *p)
{
    if (!p->dynamic_started)
        return;
    bool resume_primary =
        p->primary_done &&
        !atomic_load_explicit(&p->aborted, memory_order_relaxed);
    stop_dynamic(p);
    // Unread queue bytes are dropped and fetched again by one request from
    // the first byte the consumer has not taken; the ring holds only bytes
    // before the queue, so nothing is delivered twice.
    if (resume_primary) {
        mp_mutex_lock(&p->mtx);
        p->request_start = p->queue_pos;
        mp_mutex_unlock(&p->mtx);
    }
    p->dynamic_supported = false;
    p->selected_requests = 1;
    p->previous_requests = 0;
    if (p->ctx->stream_id == p->stream_id) {
        p->ctx->selected_requests = 1;
        p->ctx->capacity = 0;
    }
    if (resume_primary)
        start_request(p);
}

static void on_dynamic_done(struct dynamic_request *request, CURLcode code)
{
    struct priv *p = request->ref.p;
    struct range_desc *range = request->range;
    bool aborted = atomic_load_explicit(&p->aborted, memory_order_relaxed);
    request->range = NULL;
    if (!range)
        return;
    range->request = NULL;
    size_t expected = range->end - range->start;
    if (code == CURLE_OK && request->response_ok &&
        request->fetch_pos == expected && !aborted)
    {
        fill_dynamic(p);
        check_dynamic_eof(p);
        return;
    }

    if (request->changed) {
        // Bytes already delivered no longer match the server's file.
        MP_ERR(p, "parallel range %" PRIu64 "-%" PRIu64 " changed while "
                  "it was retried\n", range->start, range->end - 1);
        mp_mutex_lock(&p->mtx);
        p->stream_error = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    bool response_rejected = !request->response_ok &&
                             (code == CURLE_OK || code == CURLE_WRITE_ERROR);
    bool recoverable =
        !aborted && (response_rejected || is_recoverable_error(code)) &&
        range->retry_count < p->opts->max_retries;
    if (recoverable) {
        range->retry_count++;
        atomic_fetch_add_explicit(&p->ctx->retry_attempts, 1,
                                  memory_order_relaxed);
        const char *reason = response_rejected
            ? "HTTP range rejected" : curl_easy_strerror(code);
        MP_WARN(p, "%s, retrying parallel range (#%d)\n",
                reason, range->retry_count);
        uint64_t generation = request->ref.generation;
        if (start_range_transfer(request, range)) {
            if (generation != p->generation)
                p->transitioning++;
            request->ref.generation = generation;
        }
        return;
    }

    if (!aborted) {
        MP_WARN(p, "parallel range failed; continuing with one request\n");
        cmd_async(p, CMD_FALLBACK_DYNAMIC);
    }
}

static void log_curl_error(struct priv *p, const char *what, CURLcode code)
{
    MP_ERR(p, "%s: %s\n", what, curl_easy_strerror(code));
    if (code == CURLE_PEER_FAILED_VERIFICATION ||
        code == CURLE_SSL_CACERT_BADFILE)
    {
        MP_ERR(p,
            "TLS certificate verification failed.\n"
            "This usually means an outdated CA bundle, a self-signed "
            "certificate,\nor a MITM proxy on your network. To bypass at "
            "your own risk, pass\n--tls-verify=no.\n");
    }
}

static void on_done(struct priv *p, CURLcode code)
{
    bool aborted = atomic_load_explicit(&p->aborted, memory_order_relaxed);

    if (!p->probed) {
        // Connection died before any headers arrived.
        if (code != CURLE_OK && !aborted)
            log_curl_error(p, "error", code);
        mp_mutex_lock(&p->mtx);
        p->probed = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    // Roll completed bytes into request_start so retries and chunk
    // continuations resume at the next missing byte.
    uint64_t received = p->request_received;
    p->request_start += received;
    p->request_received = 0;

    if (p->reply_eof && !aborted) {
        // The reply marked the end of the file and its body was dropped, so
        // the curl result means nothing.
        p->finished = true;
        mp_mutex_lock(&p->mtx);
        p->stream_eof = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    if (code == CURLE_OK && p->response_ok && !aborted) {
        p->retry_count = 0;
        if (p->dynamic_supported) {
            p->primary_done = true;
            if (!p->dynamic_started)
                start_dynamic(p);
            if (p->dynamic_started) {
                if (p->request_start != p->primary_end) {
                    MP_WARN(p, "primary range ended at unexpected offset; "
                               "continuing with one request\n");
                    cmd_async(p, CMD_FALLBACK_DYNAMIC);
                    return;
                }
                fill_dynamic(p);
                open_queue(p);
                return;
            }
        }

        // Keep requesting from where this reply stopped: requests are
        // chunked, servers cap their replies, and with an unknown size only
        // a 416 marks the end.
        bool more = p->seekable &&
                    (p->opts->max_request_size > 0 || p->reply_ranged);
        bool past_size = p->content_size >= 0 && p->request_start >= p->content_size;
        bool past_end = p->request_end > 0 && p->request_start >= p->request_end;
        if (more && !past_size && !past_end) {
            if (!received) {
                // The same request would be repeated forever.
                MP_ERR(p, "Server sent back no data at offset %" PRIu64 "\n",
                       p->request_start);
                mp_mutex_lock(&p->mtx);
                p->stream_error = true;
                mp_cond_broadcast(&p->cond);
                mp_mutex_unlock(&p->mtx);
                return;
            }
            start_request(p);
            return;
        }

        p->finished = true;
        mp_mutex_lock(&p->mtx);
        p->stream_eof = true;
        mp_cond_broadcast(&p->cond);
        mp_mutex_unlock(&p->mtx);
        return;
    }

    // Retry transport failures and invalid HTTP responses from the next
    // missing byte. Never append an error body to the media stream.
    bool response_rejected = !p->response_ok &&
                             (code == CURLE_OK || code == CURLE_WRITE_ERROR);
    bool recoverable =
        !aborted && p->seekable &&
        (response_rejected || is_recoverable_error(code)) &&
        p->retry_count < p->opts->max_retries;
    if (recoverable) {
        p->retry_count++;
        atomic_fetch_add_explicit(&p->ctx->retry_attempts, 1,
                                  memory_order_relaxed);
        const char *reason = response_rejected
            ? "HTTP response rejected" : curl_easy_strerror(code);
        MP_WARN(p, "%s, retrying (#%d) from %" PRIu64 "\n",
                reason, p->retry_count, p->request_start);
        uint64_t generation = p->primary_ref.generation;
        start_request(p);
        if (p->active && generation != p->generation) {
            p->primary_ref.generation = generation;
            p->transitioning++;
        }
        return;
    }

    if (!aborted) {
        if (response_rejected)
            MP_ERR(p, "HTTP response rejected\n");
        else
            log_curl_error(p, "transfer failed", code);
    }

    mp_mutex_lock(&p->mtx);
    p->stream_error = true;
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
}

static void on_cancel(void *ctx)
{
    struct priv *p = ctx;
    atomic_store(&p->aborted, true);
    mp_mutex_lock(&p->mtx);
    mp_cond_broadcast(&p->cond);
    mp_mutex_unlock(&p->mtx);
    if (p->ctx && p->ctx->multi)
        curl_multi_wakeup(p->ctx->multi);
}

// Configuration and initial setup

static struct curl_slist *build_header_list(struct priv *p)
{
    struct curl_slist *list = NULL;
    if (p->net_opts->referrer && p->net_opts->referrer[0]) {
        char *h = talloc_asprintf(NULL, "Referer: %s", p->net_opts->referrer);
        list = curl_slist_append(list, h);
        talloc_free(h);
    }
    if (p->net_opts->http_header_fields) {
        for (int i = 0; p->net_opts->http_header_fields[i]; i++)
            list = curl_slist_append(list, p->net_opts->http_header_fields[i]);
    }
    if (p->scheme->proto == MP_CURL_PROTO_HTTP)
        list = curl_slist_append(list, "Icy-MetaData: 1");
    return list;
}

static void setup_curl(struct priv *p, CURL *c)
{
    curl_easy_setopt(c, CURLOPT_URL, p->url);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    // Enable verbose output with trace level logging.
    curl_easy_setopt(c, CURLOPT_VERBOSE, mp_msg_test(p->log, MSGL_TRACE) ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, debug_callback);
    curl_easy_setopt(c, CURLOPT_DEBUGDATA, p);

    bool identity = p->start_offset > 0 || p->request_end > 0 ||
                    p->opts->max_request_size > 0;
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, identity ? "identity" : "");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, (long)p->opts->max_redirects);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)p->opts->http_version);
    curl_easy_setopt(c, CURLOPT_HSTS_CTRL, (long)CURLHSTS_ENABLE);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    // libcurl's 16 KiB default costs one body callback (and recv) per 16 KiB.
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)p->opts->read_size);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS,
                     (long)(p->opts->connect_timeout * 1000));
    // Abort a transfer that receives nothing for --network-timeout seconds,
    // as lavf's socket timeout does, so a connection that went quiet without
    // closing is retried instead of starving the demuxer indefinitely.
    // libcurl skips the check while a transfer is paused on a full ring.
    if (p->net_opts->timeout > 0) {
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME,
                         (long)MPMAX(1, ceil(p->net_opts->timeout)));
    }

    if (p->net_opts->useragent && p->net_opts->useragent[0])
        curl_easy_setopt(c, CURLOPT_USERAGENT, p->net_opts->useragent);
    if (p->net_opts->http_proxy && p->net_opts->http_proxy[0])
        curl_easy_setopt(c, CURLOPT_PROXY, p->net_opts->http_proxy);

    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, p->net_opts->tls_verify ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, p->net_opts->tls_verify ? 2L : 0L);
    if (p->net_opts->tls_ca_file) {
        char *path = mp_get_user_path(p, p->global, p->net_opts->tls_ca_file);
        curl_easy_setopt(c, CURLOPT_CAINFO, path);
    }
    if (p->net_opts->tls_cert_file) {
        char *path = mp_get_user_path(p, p->global, p->net_opts->tls_cert_file);
        curl_easy_setopt(c, CURLOPT_SSLCERT, path);
    }
    if (p->net_opts->tls_key_file) {
        char *path = mp_get_user_path(p, p->global, p->net_opts->tls_key_file);
        curl_easy_setopt(c, CURLOPT_SSLKEY, path);
    }

    if (p->net_opts->cookies_enabled) {
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
        char *file = p->net_opts->cookies_file;
        if (file && file[0]) {
            void *tmp = talloc_new(NULL);
            char *path = mp_get_user_path(tmp, p->global, file);
            bstr data = stream_read_file2(path, tmp,
                STREAM_READ_FILE_FLAGS_DEFAULT & ~STREAM_LOCAL_FS_ONLY,
                p->global, 1000000);
            bstr buf = data;
            while (buf.len) {
                bstr line = bstr_strip_linebreaks(bstr_getline(buf, &buf));
                if (!line.len)
                    continue;
                char *line_str = bstrto0(tmp, line);
                curl_easy_setopt(c, CURLOPT_COOKIELIST, line_str);
            }
            talloc_free(tmp);
        }
    }

    if (p->headers)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, p->headers);
#if HAVE_SOCKET_PROBES
    curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, open_socket_cb);
    curl_easy_setopt(c, CURLOPT_OPENSOCKETDATA, p->ctx);
    curl_easy_setopt(c, CURLOPT_CLOSESOCKETFUNCTION, close_socket_cb);
    curl_easy_setopt(c, CURLOPT_CLOSESOCKETDATA, p->ctx);
#endif
}

// stream_curl implementation

static int curl_fill_buffer(struct stream *s, void *buffer, int max_len)
{
    struct priv *p = s->priv;
    if (max_len <= 0)
        return 0;

    mp_mutex_lock(&p->mtx);

    // The ring holds bytes before the queue, so it is always drained first.
    while (p->count == 0 && !(p->queue_open && p->ordered_pos > p->queue_pos) &&
           !p->stream_eof && !p->stream_error &&
           !atomic_load_explicit(&p->aborted, memory_order_relaxed))
    {
        int64_t started = mp_time_ns();
        // Later bytes are already here; only the next one is missing.
        bool blocked = p->queue_open && p->count == 0 &&
                       p->queued_bytes > p->ordered_pos - p->queue_pos;
        p->consumer_waiting = true;
        mp_cond_wait(&p->cond, &p->mtx);
        p->consumer_waiting = false;
        int64_t waited = mp_time_ns() - started;
        atomic_fetch_add_explicit(&p->ctx->consumer_wait_ns, waited,
                                  memory_order_relaxed);
        if (blocked) {
            atomic_fetch_add_explicit(&p->ctx->head_of_line_ns, waited,
                                      memory_order_relaxed);
        }
    }

    size_t copy = MPMIN((size_t)max_len, p->count);
    bool service = false;
    if (copy > 0) {
        size_t head_chunk = MPMIN(p->buffer_size - p->head, copy);
        memcpy(buffer, p->buffer + p->head, head_chunk);
        memcpy((char *)buffer + head_chunk, p->buffer, copy - head_chunk);
        p->head = (p->head + copy) % p->buffer_size;
        p->count -= copy;
    } else if (p->queue_open && p->ordered_pos > p->queue_pos &&
               !p->stream_error)
    {
        // Claim published bytes of the oldest range and copy them without
        // the lock every curl callback needs. Published bytes never change
        // and the storage stays until consumed; a fallback waits for us.
        struct range_desc *range = &p->ranges[p->range_head];
        size_t offset = range->consumed;
        copy = MPMIN((size_t)max_len, range->published - offset);
        p->copying = true;
        mp_mutex_unlock(&p->mtx);
        memcpy(buffer, range->data + offset, copy);
        mp_mutex_lock(&p->mtx);
        p->copying = false;
        if (p->copy_waiter)
            mp_cond_broadcast(&p->cond);
        range->consumed += copy;
        p->queue_pos += copy;
        p->queued_bytes -= copy;
        if (range->complete && range->consumed == range->end - range->start) {
            p->range_head = (p->range_head + 1) % p->num_ranges;
            p->range_count--;
            service = !p->service_pending;
            p->service_pending = true;
        }
    }

    bool unpause = p->paused && !p->stream_eof && !p->stream_error &&
                   p->buffer_size - p->count >= p->buffer_size / 2;

    mp_mutex_unlock(&p->mtx);

    if (unpause)
        cmd_async(p, CMD_UNPAUSE);
    if (service)
        cmd_async(p, CMD_PUMP);

    return copy;
}

static int curl_seek(struct stream *s, int64_t pos)
{
    struct priv *p = s->priv;
    if (pos < 0)
        return 0;
    cmd_sync(p, CMD_SEEK, pos, true);
    return 1;
}

static int64_t curl_get_size(struct stream *s)
{
    struct priv *p = s->priv;
    return p->content_size;
}

static int curl_control(struct stream *s, int cmd, void *arg)
{
    struct priv *p = s->priv;
    switch (cmd) {
    case STREAM_CTRL_GET_METADATA: {
        mp_mutex_lock(&p->mtx);
        struct mp_tags *tags = mp_icy_get_metadata(p->icy, s);
        mp_mutex_unlock(&p->mtx);
        if (!tags)
            break;
        *(struct mp_tags **)arg = tags;
        return STREAM_OK;
    }
    }
    return STREAM_UNSUPPORTED;
}

static void priv_destructor(void *ptr)
{
    struct priv *p = ptr;
    if (p->s->cancel)
        mp_cancel_set_cb(p->s->cancel, NULL, NULL);
    if (p->curl) {
        cmd_sync(p, CMD_REMOVE, 0, false);
        curl_easy_cleanup(p->curl);
    }
    if (p->headers)
        curl_slist_free_all(p->headers);
    unregister_curl_stream(p);
    mp_mutex_destroy(&p->mtx);
    mp_cond_destroy(&p->cond);
}

static void curl_close(struct stream *s)
{
    struct priv *p = s->priv;
    if (!p)
        return;
    if (s->cancel)
        mp_cancel_set_cb(s->cancel, NULL, NULL);
    if (p->curl) {
        cmd_sync(p, CMD_REMOVE, 0, false);
        curl_easy_cleanup(p->curl);
        p->curl = NULL;
    }
}

static int curl_open(stream_t *s, const struct stream_open_args *args)
{
    if (s->mode != STREAM_READ)
        return STREAM_UNSUPPORTED;
    if (!s->global || !s->global->curl) {
        MP_ERR(s, "curl backend not initialized\n");
        return STREAM_ERROR;
    }

    struct curl_opts *opts = mp_get_config_group(s, s->global, &curl_conf);
    if (!opts->enabled)
        return STREAM_NO_MATCH;
    if (curl_failed(s->global->curl, s->log, MSGL_ERR))
        return STREAM_NO_MATCH;

    struct priv *p = talloc_zero(s, struct priv);
    s->priv = p;
    talloc_set_destructor(p, priv_destructor);

    p->log = s->log;
    p->global = s->global;
    p->ctx = s->global->curl;
    p->s = s;
    p->opts = talloc_steal(p, opts);
    p->net_opts = mp_get_config_group(p, s->global, &mp_network_conf);
    p->url = talloc_strdup(p, s->url);
    p->scheme = curl_scheme_lookup(bstr0(p->url));
    // Only supported URLs are supposed to reach here.
    mp_assert(p->scheme);
    p->content_size = -1;
    p->buffer_size = p->opts->buffer_size;
    p->selected_requests = p->opts->adaptive
        ? MPMIN(p->opts->startup_requests, p->opts->parallel_requests)
        : p->opts->parallel_requests;
    p->startup = p->opts->adaptive && p->opts->max_request_size > 0;
    p->latency_ms = p->opts->latency_ms;
    // Speculative memory is one ring, not max-request-size times the cap.
    p->segment_size = MPMIN(p->opts->max_request_size,
                           p->buffer_size / p->opts->parallel_requests);
    p->buffer = talloc_size(p, p->buffer_size);
    p->icy = mp_icy_new(p);

    if (args->special_arg) {
        const struct curl_open_args *oa = args->special_arg;
        if (oa->offset > 0) {
            p->start_offset = oa->offset;
            p->request_start = oa->offset;
        }
        if (oa->end_offset > 0)
            p->request_end = oa->end_offset;
    }

    mp_mutex_init(&p->mtx);
    mp_cond_init(&p->cond);
    register_curl_stream(p);
    p->aborted = false;

    p->curl = curl_easy_init();
    if (!p->curl) {
        MP_ERR(s, "curl_easy_init failed\n");
        return STREAM_NO_MATCH;
    }

    p->headers = build_header_list(p);
    p->primary_ref = (struct curl_handle_ref) {
        .kind = CURL_HANDLE_PRIMARY,
        .p = p,
    };
    setup_curl(p, p->curl);
    curl_easy_setopt(p->curl, CURLOPT_PRIVATE, &p->primary_ref);
    curl_easy_setopt(p->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(p->curl, CURLOPT_WRITEDATA, p);
    curl_easy_setopt(p->curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(p->curl, CURLOPT_HEADERDATA, p);
    curl_easy_setopt(p->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(p->curl, CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
    curl_easy_setopt(p->curl, CURLOPT_XFERINFODATA, p);
    if (s->cancel)
        mp_cancel_set_cb(s->cancel, on_cancel, p);

    cmd_sync(p, CMD_ADD, 0, false);

    mp_mutex_lock(&p->mtx);
    while (!p->probed && !atomic_load_explicit(&p->aborted, memory_order_relaxed))
        mp_cond_wait(&p->cond, &p->mtx);
    mp_mutex_unlock(&p->mtx);

    // libcurl may have failed while the request was in flight.
    if (!p->stream_ok && curl_failed(s->global->curl, s->log, MSGL_ERR))
        return STREAM_NO_MATCH;
    if (!p->stream_ok || atomic_load(&p->aborted))
        return STREAM_ERROR;

    char *content_type = NULL;
    curl_easy_getinfo(p->curl, CURLINFO_CONTENT_TYPE, &content_type);
    bstr mime = bstr_strip(bstr_split(bstr0(content_type), ";", NULL));
    if (mime.len)
        s->mime_type = bstrto0(s, mime);
    s->server_filename = talloc_strdup(s, p->server_filename);

    const char *effective_url = NULL;
    curl_easy_getinfo(p->curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    s->effective_url = talloc_strdup(s, effective_url);

    s->seekable = p->seekable;
    s->is_network = true;
    s->streaming = true;
    s->fast_skip = true;
    s->fill_buffer = curl_fill_buffer;
    s->seek = p->seekable ? curl_seek : NULL;
    s->get_size = curl_get_size;
    s->control = curl_control;
    s->close = curl_close;
    s->pos = p->start_offset;

    return STREAM_OK;
}

static bool curl_has_proto(bstr scheme)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    mp_require(info && info->protocols);
    return bstr_in_list0(scheme, (char **)info->protocols);
}

static char **curl_get_protocols(void)
{
    int num = 0;
    char **protocols = NULL;
    for (int i = 0; i < MP_ARRAY_SIZE(curl_schemes); i++) {
        bstr scheme = curl_schemes[i].scheme;
        if (curl_has_proto(scheme))
            MP_TARRAY_APPEND(NULL, protocols, num, bstrdup0(protocols, scheme));
    }
    MP_TARRAY_APPEND(NULL, protocols, num, NULL);
    return protocols;
}

const stream_info_t stream_info_curl = {
    .name = "curl",
    .open2 = curl_open,
    .get_protocols = curl_get_protocols,
    .stream_origin = STREAM_ORIGIN_NET,
};

// FFmpeg AVIOContext implementation
// Allows demuxers to use our stream_curl in nested io and sub-demuxers. This
// should route all traffic through our implementation.

struct curl_avio_cookie {
    const AVClass *av_class;
    struct stream *stream;
    struct mp_cancel *cancel;
    const char *location; // final URL after redirects, exposed via the "location" opt
    AVIOContext *transport;
};

static const AVClass curl_avio_cookie_class = {
    .class_name = "mpv_curl_avio",
    .item_name  = av_default_item_name,
    .option     = (const AVOption[]) {
        {"location", "The actual location of the data received",
         offsetof(struct curl_avio_cookie, location),
         AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, AV_OPT_FLAG_DECODING_PARAM},
        {0}
    },
    .version = LIBAVUTIL_VERSION_INT,
};

static void *curl_avio_child_next(void *obj, void *prev)
{
    AVIOContext *pb = obj;
    return prev ? NULL : pb->opaque; // the cookie
}

static const AVClass curl_avio_class = {
    .class_name = "mpv_curl_avio",
    .item_name  = av_default_item_name,
    .child_next = curl_avio_child_next,
    .version    = LIBAVUTIL_VERSION_INT,
};

static bool is_protocol_allowed(struct mp_log *log, bstr scheme,
                                const char *whitelist, const char *blacklist)
{
    // `scheme` is required to be wrapped null-terminated string literal.
    // This is UB otherwise, see curl_schemes.
    mp_assert(scheme.len && scheme.start[scheme.len] == '\0');
    if (whitelist && av_match_list(scheme.start, whitelist, ',') <= 0) {
        mp_err(log, "Protocol '%.*s' not on whitelist '%s'!\n", BSTR_P(scheme), whitelist);
        return false;
    }
    if (blacklist && av_match_list(scheme.start, blacklist, ',') > 0) {
        mp_err(log, "Protocol '%.*s' on blacklist '%s'!\n", BSTR_P(scheme), blacklist);
        return false;
    }
    return true;
}

static int curl_avio_read(void *opaque, uint8_t *buf, int size)
{
    struct curl_avio_cookie *c = opaque;
    int ret = stream_read_partial(c->stream, buf, size);
    return ret > 0 ? ret : AVERROR_EOF;
}

static int64_t curl_avio_seek(void *opaque, int64_t pos, int whence)
{
    struct curl_avio_cookie *c = opaque;
    if (whence == AVSEEK_SIZE) {
        int64_t end = stream_get_size(c->stream);
        return end >= 0 ? end : AVERROR(ENOSYS);
    }
    if (whence == SEEK_END) {
        int64_t end = stream_get_size(c->stream);
        if (end < 0)
            return AVERROR(EINVAL);
        pos += end;
    } else if (whence == SEEK_CUR) {
        pos += stream_tell(c->stream);
    } else if (whence != SEEK_SET) {
        return AVERROR(EINVAL);
    }
    if (pos < 0)
        return AVERROR(EINVAL);
    if (!stream_seek(c->stream, pos))
        return AVERROR(EIO);
    return pos;
}

static int open_curl_transport(struct demuxer *demuxer, AVIOContext **pb_out,
                               void **cookie_out, const char *url, int flags,
                               AVDictionary **options,
                               const char *whitelist, const char *blacklist)
{
    *pb_out = NULL;
    *cookie_out = NULL;

    if (flags & AVIO_FLAG_WRITE)
        return AVERROR(ENOSYS);

    // Check protocol early, to return ENOSYS and allow lavf to fallback.
    const struct curl_scheme *cs = curl_scheme_lookup(bstr0(url));
    if (!cs || !curl_has_proto(cs->scheme))
        return AVERROR(ENOSYS);

    struct curl_opts *opts = mp_get_config_group(NULL, demuxer->global, &curl_conf);
    bool enabled = opts->enabled;
    talloc_free(opts);
    if (!enabled)
        return AVERROR(ENOSYS);

    // The context is required to be initialized in global.
    mp_require(demuxer->global && demuxer->global->curl);
    if (curl_failed(demuxer->global->curl, demuxer->log, MSGL_V))
        return AVERROR(ENOSYS);

    struct curl_open_args oa = {0};
    if (options && *options) {
        AVDictionaryEntry *e;
        // lavf's http demuxer exposes initial/final byte offsets as AVOptions
        // Some demuxers, like lavf/hls.c assume it is always available, even for
        // custom IO... Add support for this.
        if ((e = av_dict_get(*options, "offset", NULL, 0)))
            oa.offset = strtoll(e->value, NULL, 10);
        if ((e = av_dict_get(*options, "end_offset", NULL, 0)))
            oa.end_offset = strtoll(e->value, NULL, 10);
    }

    if (!is_protocol_allowed(demuxer->log, cs->scheme, whitelist, blacklist))
        return AVERROR(EINVAL);

    // Each nested stream gets its own mp_cancel slaved to the main demuxer,
    // so the http backend can install its own wake-up callback without
    // clobbering the top-level stream or any sibling nested stream.
    struct mp_cancel *cancel = mp_cancel_new(NULL);
    mp_cancel_set_parent(cancel, demuxer->cancel);

    struct stream_open_args args = {
        .global = demuxer->global,
        .cancel = cancel,
        .url = url,
        .flags = STREAM_READ | (demuxer->stream_origin & STREAM_ORIGIN_MASK),
        .sinfo = &stream_info_curl,
        .special_arg = &oa,
    };

    struct stream *s = NULL;
    int r = stream_create_with_args(&args, &s);
    if (r != STREAM_OK || !s) {
        talloc_free(cancel);
        return AVERROR(EIO);
    }

    struct curl_avio_cookie *c = talloc_zero(NULL, struct curl_avio_cookie);
    c->av_class = &curl_avio_cookie_class;
    c->stream = s;
    c->cancel = cancel;
    c->location = s->effective_url ? s->effective_url : s->url;

    void *buffer = av_malloc(64 * 1024);
    if (!buffer) {
        free_stream(s);
        talloc_free(cancel);
        talloc_free(c);
        return AVERROR(ENOMEM);
    }

    AVIOContext *pb = avio_alloc_context(buffer, 64 * 1024, 0, c,
                                         curl_avio_read, NULL,
                                         s->seekable ? curl_avio_seek : NULL);
    if (!pb) {
        av_free(buffer);
        free_stream(s);
        talloc_free(cancel);
        talloc_free(c);
        return AVERROR(ENOMEM);
    }
    pb->seekable = s->seekable ? AVIO_SEEKABLE_NORMAL : 0;
    pb->av_class = &curl_avio_class;
    pb->pos = oa.offset;

    *pb_out = pb;
    *cookie_out = c;
    return 0;
}

static void close_curl_transport(AVIOContext *pb, struct curl_avio_cookie *c)
{
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    free_stream(c->stream);
    talloc_free(c->cancel);
    talloc_free(c);
}

// Open a `crypto+...` URL by opening the inner URL with curl and layering
// AES-128-CBC decryption on top using the `key`/`iv` hex strings from the
// AVDictionary. Returns AVERROR(ENOSYS) when curl can't handle the inner or
//the AES options are missing.
static int open_curl_crypto(struct demuxer *demuxer, AVIOContext **pb_out,
                            void **cookie_out, const char *inner_url,
                            int flags, AVDictionary **options,
                            const char *whitelist, const char *blacklist)
{
    if (flags & AVIO_FLAG_WRITE)
        return AVERROR(ENOSYS);
    if (!options || !*options)
        return AVERROR(ENOSYS);

    AVDictionaryEntry *key_e = av_dict_get(*options, "key", NULL, 0);
    AVDictionaryEntry *iv_e = av_dict_get(*options, "iv", NULL, 0);
    if (!key_e || !iv_e)
        return AVERROR(ENOSYS);

    void *tmp = talloc_new(NULL);
    AVIOContext *transport = NULL;
    void *cookie = NULL;
    AVIOContext *wrapper = NULL;

    bstr key = {0}, iv = {0};
    bstr_decode_hex(tmp, bstr0(key_e->value), &key);
    bstr_decode_hex(tmp, bstr0(iv_e->value), &iv);

    int r = open_curl_transport(demuxer, &transport, &cookie, inner_url, flags,
                                options, whitelist, blacklist);
    if (r < 0)
        goto done;

    r = mp_avio_crypto_open(&wrapper, transport, key, iv);
    if (r < 0) {
        MP_ERR(demuxer, "Failed to set up crypto stream: %s\n", av_err2str(r));
        close_curl_transport(transport, cookie);
        goto done;
    }

    // Consume from the dict so demuxer-side mp_avdict_print_unset stays quiet.
    av_dict_set(options, "key", NULL, 0);
    av_dict_set(options, "iv", NULL, 0);

    ((struct curl_avio_cookie *)cookie)->transport = transport;
    *pb_out = wrapper;
    *cookie_out = cookie;

done:
    talloc_free(tmp);
    return r;
}

int mp_curl_avio_open(struct demuxer *demuxer, AVIOContext **pb_out,
                      void **cookie_out, const char *url, int flags,
                      AVDictionary **options,
                      const char *whitelist, const char *blacklist)
{
    *pb_out = NULL;
    *cookie_out = NULL;

    if (flags & AVIO_FLAG_WRITE)
        return AVERROR(ENOSYS);

    // Nested IO plumbs whitelist/blacklist through the AVDictionary, use that
    // if set, same as FFmpeg's implementation.
    if (options && *options) {
        AVDictionaryEntry *e;
        if ((e = av_dict_get(*options, "protocol_whitelist", NULL, 0)))
            whitelist = e->value;
        if ((e = av_dict_get(*options, "protocol_blacklist", NULL, 0)))
            blacklist = e->value;
    }

    bstr rest = bstr0(url);
    if (bstr_eatstart0(&rest, "crypto+") || bstr_eatstart0(&rest, "crypto:")) {
        if (!is_protocol_allowed(demuxer->log, (bstr)bstr0_lit("crypto"), whitelist, blacklist))
            return AVERROR(EINVAL);
        return open_curl_crypto(demuxer, pb_out, cookie_out, rest.start,
                                flags, options, whitelist, blacklist);
    }

    return open_curl_transport(demuxer, pb_out, cookie_out, url, flags,
                               options, whitelist, blacklist);
}

void mp_curl_avio_close(AVIOContext *pb, void *cookie)
{
    struct curl_avio_cookie *c = cookie;
    if (!c)
        return;

    AVIOContext *transport = c->transport;
    if (transport) {
        mp_avio_crypto_close(&pb);
    } else {
        transport = pb;
    }
    close_curl_transport(transport, c);
}
