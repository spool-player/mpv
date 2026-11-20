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
#include <time.h>

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define HAVE_SOCKET_PROBES 1
#elif defined(_WIN32)
#include <winsock2.h>
// Winsock types must precede the extension headers.
#include <mstcpip.h>
#include <ws2tcpip.h>
#define HAVE_SOCKET_PROBES 1
#endif
#if defined(__linux__)
#include <sys/auxv.h>
#if (defined(__arm__) || defined(__aarch64__)) && __has_include(<asm/hwcap.h>)
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
#include "curl_controller.h"
#include "curl_budget.h"

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
    int fixed_requests, receive_mode, http_mode;
    int64_t receive_bytes;
    bool explore;

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
        {"fixed-requests", OPT_INT(fixed_requests), M_RANGE(0, 16)},
        {"explore", OPT_BOOL(explore)},
        {"receive-mode", OPT_CHOICE(receive_mode, {"os-auto", 0}, {"measured", 1}, {"fixed", 2})},
        {"receive-bytes", OPT_BYTE_SIZE(receive_bytes), M_RANGE(1 << 20, 16 << 20)},
        {"http-mode", OPT_CHOICE(http_mode, {"auto", 0}, {"tcp", 1}, {"http1", 2}, {"prefer-h3", 3})},
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
        .buffer_size = 8 << 20, // one core-wide baseline
        .read_size = 256 << 10,
        .max_request_size = 16 << 20,
        .parallel_requests = 4,
        .explore = true,
        .receive_mode = 1,
        .receive_bytes = 4 << 20,
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
    uint64_t charge;
    uint64_t inode;
    int64_t udp_drops;
    int64_t receive_requested;
    int receive_status;
    int64_t receive_buffer; // effective SO_RCVBUF read-back, -1 unknown
    double rtt_ms;          // kernel smoothed RTT, -1 unavailable
    int64_t generation;
    int64_t connection_id; // CURLINFO_CONN_ID + 1; 0 not yet associated
    int http_version;
    int64_t receive_window;
    // Cached with the 500ms socket probes. ACTIVESOCKET is completion-oriented;
    // in-flight transfers also match this kernel connection tuple.
    int family, local_port, peer_port;
    uint8_t local_address[16], peer_address[16];
    bool endpoints_valid;
};

// Read by consumers only at their sampling boundary, never body callbacks.
// Deduplicate by thread: nested streams can share one demux reader thread.
#define MP_CURL_MAX_CONSUMERS 64
struct curl_consumer_cpu {
    uint64_t generation;
    unsigned users;
    int64_t last_cpu_ns, total_ns, next_sample_ns;
    bool valid;
};
static atomic_uint_fast64_t next_consumer_generation = 1;
static _Thread_local uint64_t consumer_thread_generation;

struct curl_ctx {
    mp_thread thread;
    struct mp_dispatch_queue *dispatch;
    CURLM *multi;
    CURLM *trial;
    int64_t trial_id, next_pool_id;
    int trial_users, trial_receive_mode, trial_http_mode;
    int64_t trial_receive_bytes;
    uint64_t multi_buffer_charge, trial_buffer_charge;
    bool in_completions;
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
    int64_t thread_cpu_ns, next_tick_ns, last_cpu_ns;
    uint64_t protocol_ordered[4], protocol_previous[4];
    struct mp_curl_protocol_metrics protocol_metrics[3];
    uint64_t ordered_bytes, copy_in_bytes, body_callbacks, iterations;
    int64_t perform_ns, dispatch_ns, poll_ns;
    // Retired stream totals. Live consumer counters stay under priv.mtx.
    uint64_t delivered_bytes, coalesced_wakeups, hol_events;
    int64_t retired_consumer_wait_ns, retired_hol_ns, retired_gap_ns;
    struct priv *streams; // worker-owned intrusive registry, no playback cap
    struct warm_request *warm;
    struct curl_handle_ref *handles; // active handles, no libcurl enumeration
    int64_t experiment_stream;
    int64_t raw_epoch;
    double worker_cpu, demand_ewma, recent_demand;
    int64_t cpu_window_ns, demand_started_ns, demand_window_ns;
    bool worker_cpu_available;
    struct mp_curl_playback_state playback_state;
    struct mp_curl_budget budget;
    struct curl_rate ring_wait;
    // Sockets and host limits, owned by the curl thread.
    struct curl_socket_info sockets[MP_CURL_MAX_SOCKETS];
    uint64_t sockets_opened;
    int64_t open_socket_count;
    bool socket_snapshot_truncated;
    int64_t next_socket_sample_ns;
    int64_t host_rmem_max;      // unprivileged SO_RCVBUF ceiling, -1 unknown
    int64_t host_autotune_max;  // receive autotuning ceiling, -1 unknown
    int hw_aes;                 // 1 yes, 0 no, -1 unknown
    // Player state handed over without waiting for this thread.
    mp_mutex state_lock;
    struct curl_consumer_cpu consumer_cpu[MP_CURL_MAX_CONSUMERS];
    bool consumer_snapshot_truncated;
    int64_t retired_consumer_cpu_ns;
    bool consumer_cpu_seen, consumer_cpu_incomplete;
    struct mp_curl_playback_state pending_state;
    bool state_pending;
    // Applied on the curl thread. The player owns pause and buffer state;
    // the host's control supplies facts and live host state.
    int64_t epoch;
    struct mp_curl_control configured;
    int64_t stale_controls, rejected_controls;
    bool background_network;
    int64_t state_updates;
    double declared_bitrate;
    double observed_bitrate;
    int64_t reorder_bytes, peak_reorder_bytes;
    // Worker wake-up lateness past its own deadline; recent is since the
    // last metrics snapshot. Both owned by the curl thread.
    atomic_int_fast64_t recent_lateness_ns;
    int64_t peak_lateness_ns;
    // 0 then power-of-two microsecond upper bounds; no allocation/sorting.
    uint64_t lateness_bins[32], lateness_samples;
    int64_t next_lateness_window_ns;
    int64_t lateness_window_max_ns;
    double lateness_p50_ms, lateness_p99_ms;
    bool lateness_available;
    struct curl_rate lanes[MP_CURL_MAX_LANES];
    int64_t lane_capacity[MP_CURL_MAX_LANES];
    int64_t lane_samples[MP_CURL_MAX_LANES];
    int64_t next_stream_id;
    int64_t stream_id;
    int selected_requests;
    double ttfb_ms;
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
    CURL_HANDLE_WARM,
};

struct curl_handle_ref {
    enum curl_handle_kind kind;
    struct priv *p;
    CURL *handle;
    CURLM *pool;
    int64_t pool_id;
    struct curl_handle_ref *next;
    int64_t connection_id;
    int http_version;
    int lane;
    int64_t started_ns;
    int64_t first_byte_ns;
    uint64_t received;
    bool blocked;
    bool measuring;
    bool requested_h3;
    bool fresh_connection;
    int64_t last_progress_ns;
    bool foreign; // other traffic shared the link during the measurement
    uint64_t pause_serial;
    uint64_t generation;
};

struct warm_request {
    struct curl_slist *headers;
    struct curl_handle_ref ref;
    struct warm_request *next;
};

#define MP_CURL_MAX_RANGES 32

// One ordered byte range of the parallel queue. Its storage belongs to the
// range until the consumer has read every byte of it, so a retry can verify
// already published bytes and a later range can never overwrite them.
struct range_desc {
    uint64_t start, end;  // absolute [start, end)
    uint8_t *data;        // admitted bytes owned by this descriptor
    uint64_t allocation;
    // Curl thread only.
    size_t received;      // contiguous bytes stored from start
    struct dynamic_request *request; // transfer filling it, NULL if none
    int retry_count;
    bool rescued;
    // Protected by priv.mtx.
    size_t published;     // bytes the consumer may read, <= received
    size_t consumed;      // bytes the consumer has taken
    size_t retained; // received snapshot under mtx, including hidden bytes
    bool complete;        // every byte is published
    int protocol_index; // protocol which supplied this range's fresh bytes
};

// A network transfer slot. Slots are independent of range storage, so a
// finished transfer can start the next range while its bytes wait to be read.
struct dynamic_request {
    struct curl_handle_ref ref;
    CURL *curl;
    struct range_desc *range; // range being fetched, NULL when idle
    size_t fetch_pos;         // next offset in range this transfer writes
    size_t length; // range size; the descriptor may be recycled
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
    struct priv *metric_next;
    bool metric_linked;

    struct curl_opts *opts;
    struct mp_curl_controller controller;
    int64_t epoch;
    uint64_t ring_allocation, queue_allocation, phase_reservation;
    uint64_t total_ordered, phase_ordered_start, phase_consumer_start;
    int64_t phase_wall_ns, phase_poll_ns, phase_hol_ns, phase_ring_ns;
    uint64_t control_start, control_end;
    int64_t control_started_ns;
    bool control_phase, phase_invalid;
    int phase_http;
    int64_t next_controller_ns;
    bool receive_admitted;
    CURLM *pool;
    int64_t pool_id, grant_seen;
    int receive_mode, http_mode;
    int64_t receive_bytes;
    bool pool_transition;
    int http_limit; // monotonic downgrade: 0, 20 (TCP), 11 (HTTP/1.1)
    int http_fallbacks, last_reason;
    bool udp_limited;
    int64_t udp_requested, udp_effective;
    double urgency;
    int64_t startup_ns;
    bool seek_probe;
    bool keep_warm;
    int evidence_http;
    int64_t next_service_ns;
    char origin[512];
    struct mp_network_opts *net_opts;

    CURL *curl;
    struct curl_handle_ref primary_ref;
    struct curl_slist *headers;
    struct curl_slist *range_headers; // shared by dynamic handles; owns If-Range
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
    int num_ranges;
    char *etag;                // strong validator of the primary response
    uint64_t primary_end;
    uint64_t next_schedule;
    bool dynamic_started;
    bool dynamic_supported;
    bool primary_done;
    int selected_requests;
    int64_t stream_id;
    size_t segment_size;
    double ttfb_ms;
    uint64_t phase_bytes;
    uint64_t phase_delivered;
    int phase_completed;
    bool phase_blocked;
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
    int64_t next_consumer_cpu_ns;
    int consumer_cpu_slot; // index + 1, 0 unregistered; protected by state_lock
    int64_t consumer_wait_started_ns, consumer_wait_ns;
    int64_t gap_started_ns, gap_ns, hol_started_ns, hol_ns;
    uint64_t delivered_bytes, coalesced_wakeups, hol_events;
    bool unpause_pending;
    bool copying;        // consumer is copying queue bytes unlocked
    bool copy_waiter;    // curl thread waits for that copy to finish
    bool service_pending; // a CMD_PUMP is queued for the curl thread
    bool stream_eof;     // producer has delivered all data
    bool stream_error;   // unrecoverable error
    atomic_bool aborted; // canceled by user (mp_cancel)
    struct mp_icy *icy;  // ICY metadata state, dormant until Icy-MetaInt seen
};

// Process-wide origin failures survive closing one core. Keys never leave
// this table; diagnostics use only numeric states and opaque stream IDs.
struct origin_state {
    char key[512];
    int64_t id, h2_until, h3_until, used;
    bool h2_seen, h3_seen;
};
static int detect_hw_aes(void);
static mp_static_mutex origin_lock = MP_STATIC_MUTEX_INITIALIZER;
static struct origin_state origins[64];
static int64_t next_origin_id;

static bool canonical_origin(const char *url, char *out, size_t size)
{
    CURLU *u = curl_url();
    char *scheme = NULL, *host = NULL, *port = NULL;
    bool ok = u && !curl_url_set(u, CURLUPART_URL, url, 0) &&
        !curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) &&
        !curl_url_get(u, CURLUPART_HOST, &host, 0) &&
        !curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
    if (ok) {
        for (char *p = scheme; *p; p++)
            *p = (*p >= 'A' && *p <= 'Z' ? *p + ('a' - 'A') : *p);
        for (char *p = host; *p; p++)
            *p = (*p >= 'A' && *p <= 'Z' ? *p + ('a' - 'A') : *p);
        ok = !strcmp(scheme, "http") || !strcmp(scheme, "https");
        if (ok)
            ok = snprintf(out, size, "%s://%s:%s", scheme, host, port) < size;
    }
    if (!ok && size)
        out[0] = 0;
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_url_cleanup(u);
    return ok;
}

bool mp_curl_valid_origin(const char *origin)
{
    char canonical[512];
    if (!canonical_origin(origin, canonical, sizeof(canonical)))
        return false;
    // A host policy is an origin, never a URL bearing credentials or media.
    const char *authority = strstr(origin, "://");
    return authority && !strpbrk(authority + 3, "/?#@") && strlen(origin) < 512;
}

static int origin_limit(const char *key, int failed)
{
    if (!key[0])
        return 0;
    int64_t now = mp_time_ns();
    mp_mutex_lock(&origin_lock);
    int slot = 0;
    for (int n = 0; n < MP_ARRAY_SIZE(origins); n++) {
        if (!strcmp(origins[n].key, key)) {
            slot = n;
            break;
        }
        if (origins[n].used < origins[slot].used)
            slot = n;
    }
    struct origin_state *entry = &origins[slot];
    if (strcmp(entry->key, key)) {
        *entry = (struct origin_state){.id = ++next_origin_id};
        snprintf(entry->key, sizeof(entry->key), "%s", key);
    }
    entry->used = now;
    if (failed == 20)
        entry->h2_until = now + MP_TIME_S_TO_NS(600);
    if (failed == 30)
        entry->h3_until = now + MP_TIME_S_TO_NS(600);
    int limit = entry->h2_until > now ? 11 : entry->h3_until > now ? 20 : 0;
    mp_mutex_unlock(&origin_lock);
    return limit;
}

static void origin_supported(const char *key, int version)
{
    origin_limit(key, 0);
    mp_mutex_lock(&origin_lock);
    for (int n = 0; n < MP_ARRAY_SIZE(origins); n++) {
        if (!strcmp(origins[n].key, key)) {
            origins[n].h2_seen |= version == 20;
            origins[n].h3_seen |= version == 30;
            break;
        }
    }
    mp_mutex_unlock(&origin_lock);
}

static void snapshot_origins(struct mp_curl_metrics *metrics, int64_t now)
{
    mp_mutex_lock(&origin_lock);
    for (int n = 0; n < MP_ARRAY_SIZE(origins); n++) {
        const struct origin_state *origin = &origins[n];
        if (!origin->id)
            continue;
        metrics->origins[metrics->num_origins++] = (struct mp_curl_origin_metrics){
            .id = origin->id,
            .h2_state = origin->h2_until > now ? 2 : origin->h2_seen ? 1 : 0,
            .h3_state = origin->h3_until > now ? 2 : origin->h3_seen ? 1 : 0,
            .cooldown_ms = MPMAX(0, MPMAX(origin->h2_until, origin->h3_until) - now) / 1000000,
        };
    }
    mp_mutex_unlock(&origin_lock);
}

void mp_curl_get_capabilities(struct mp_curl_capabilities *c)
{
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    *c = (struct mp_curl_capabilities){
        .http2 = !!(v->features & CURL_VERSION_HTTP2),
        .http3 = !!(v->features & CURL_VERSION_HTTP3),
        .alt_svc = !!(v->features & CURL_VERSION_ALTSVC),
        // No public libcurl API proves backend GSO/GRO. Keep the automatic
        // H3 gate closed until the build supplies verified batching evidence.
#if defined(MP_CURL_QUIC_BATCHING) && MP_CURL_QUIC_BATCHING
        .quic_batching = true,
#else
        .quic_batching = false,
#endif
        .hw_aes = detect_hw_aes(),
        .version = v->version_num,
        .tls_backend = v->ssl_version && strstr(v->ssl_version, "OpenSSL") ? "openssl"
            : v->ssl_version && strstr(v->ssl_version, "Schannel") ? "schannel"
            : v->ssl_version && strstr(v->ssl_version, "SecureTransport") ? "securetransport" : "other",
    };
}

static void apply_http_policy(struct priv *p, CURL *curl)
{
    // Explicit raw curl version retains its documented precedence.
    long version = p->opts->http_version;
    if (version == CURL_HTTP_VERSION_NONE) {
        bool tls = !strncmp(p->url, "https://", 8);
        struct mp_curl_capabilities cap;
        mp_curl_get_capabilities(&cap);
        int limit = origin_limit(p->origin, 0);
        if (p->http_limit && (!limit || p->http_limit < limit))
            limit = p->http_limit;
        version = !tls || p->http_mode == 2 || limit == 11 || !cap.http2
            ? CURL_HTTP_VERSION_1_1 : CURL_HTTP_VERSION_2TLS;
        if (tls && p->http_mode == 3 && !limit && cap.http3 && !p->udp_limited)
            version = CURL_HTTP_VERSION_3;
    }
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, version);

}

static bool downgrade_http(struct priv *p, struct curl_handle_ref *ref, CURLcode code)
{
    if (p->opts->http_version != CURL_HTTP_VERSION_NONE)
        return false;
    bool transport_error = code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR ||
        code == CURLE_PARTIAL_FILE || code == CURLE_GOT_NOTHING ||
        code == CURLE_OPERATION_TIMEDOUT;
    int failed = (code == CURLE_HTTP3 || code == CURLE_QUIC_CONNECT_ERROR) ? 30
        : (code == CURLE_HTTP2 || code == CURLE_HTTP2_STREAM) ? 20
        : transport_error && ref->http_version >= 20 ? ref->http_version
        : ref->requested_h3 && (code == CURLE_COULDNT_CONNECT || code == CURLE_SSL_CONNECT_ERROR) ? 30 : 0;
    if (!failed || (p->http_limit && p->http_limit < failed))
        return false;
    char *effective = NULL;
    char origin[512];
    curl_easy_getinfo(ref->handle, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective && canonical_origin(effective, origin, sizeof(origin)))
        origin_limit(origin, failed);
    origin_limit(p->origin, failed);
    p->http_limit = failed == 30 ? 20 : 11;
    p->http_fallbacks++;
    ref->fresh_connection = true;
    p->last_reason = failed == 30 ? 37 : 38;
    return true;
}

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
static bool foreign_traffic(struct curl_ctx *ctx);
static void stop_dynamic(struct priv *p);
static void on_dynamic_done(struct dynamic_request *request, CURLcode code);
static void fallback_dynamic(struct priv *p);
static void request_failed(struct priv *p);
static void update_adaptation(struct priv *p);
static void clear_warm_requests(struct priv *p);
static void rescue_stalled_range(struct priv *p, int64_t now);
// All shared queue observations require p->mtx. A gap may exist while the
// consumer still has a readable prefix; only its demanded-empty subset is HOL.
static uint64_t stream_readable(struct priv *p)
{
    return p->count + (p->queue_open ? p->ordered_pos - p->queue_pos : 0);
}

static uint64_t stream_reorder(struct priv *p)
{
    uint64_t bytes = p->queued_bytes ? p->queued_bytes - (p->ordered_pos - p->queue_pos) : 0;
    // Without a strong validator the leading range's received prefix is
    // deliberately hidden until completion, not reordered behind a gap.
    if (!p->etag) {
        for (int n = 0; n < p->range_count; n++) {
            const struct range_desc *range = &p->ranges[(p->range_head + n) % p->num_ranges];
            if (range->end <= p->ordered_pos)
                continue;
            if (range->start <= p->ordered_pos)
                bytes -= range->retained - range->published;
            break;
        }
    }
    return bytes;
}

static void update_gap(struct priv *p, int64_t now)
{
    bool gap = stream_reorder(p) > 0;
    bool hol = gap && p->consumer_waiting && !stream_readable(p);
    if (gap == !!p->gap_started_ns && hol == !!p->hol_started_ns)
        return;
    if (!now)
        now = mp_time_ns();
    // Receipt timestamps precede mutex acquisition: a reader may have
    // entered its wait meanwhile. Never close an interval before it began.
    now = MPMAX(now, MPMAX(p->consumer_wait_started_ns, MPMAX(p->gap_started_ns, p->hol_started_ns)));
    if (gap && !p->gap_started_ns)
        p->gap_started_ns = now;
    if (!gap && p->gap_started_ns) {
        p->gap_ns += now - p->gap_started_ns;
        p->gap_started_ns = 0;
    }
    if (hol && !p->hol_started_ns) {
        p->hol_started_ns = now;
        p->hol_events++;
    }
    if (!hol && p->hol_started_ns) {
        p->hol_ns += now - p->hol_started_ns;
        p->hol_started_ns = 0;
    }
}

static int protocol_index(int version)
{
    return version == 11 ? 0 : version == 20 ? 1 : version == 30 ? 2 : 3;
}

static void refresh_handle(struct curl_handle_ref *ref)
{
    curl_off_t id = -1;
    long version = CURL_HTTP_VERSION_NONE;
    curl_socket_t fd = CURL_SOCKET_BAD;
    curl_easy_getinfo(ref->handle, CURLINFO_CONN_ID, &id);
    curl_easy_getinfo(ref->handle, CURLINFO_HTTP_VERSION, &version);
    curl_easy_getinfo(ref->handle, CURLINFO_ACTIVESOCKET, &fd);
#if HAVE_SOCKET_PROBES
    long local_port = 0, peer_port = 0;
    char *local_ip = NULL, *peer_ip = NULL;
    uint8_t local_address[16], peer_address[16];
    int family = 0;
    curl_easy_getinfo(ref->handle, CURLINFO_LOCAL_PORT, &local_port);
    curl_easy_getinfo(ref->handle, CURLINFO_PRIMARY_PORT, &peer_port);
    curl_easy_getinfo(ref->handle, CURLINFO_LOCAL_IP, &local_ip);
    curl_easy_getinfo(ref->handle, CURLINFO_PRIMARY_IP, &peer_ip);
    if (local_ip && peer_ip) {
        if (inet_pton(AF_INET, local_ip, local_address) == 1 && inet_pton(AF_INET, peer_ip, peer_address) == 1)
            family = AF_INET;
        else if (inet_pton(AF_INET6, local_ip, local_address) == 1 && inet_pton(AF_INET6, peer_ip, peer_address) == 1)
            family = AF_INET6;
    }
#endif
    ref->connection_id = id >= 0 ? (ref->pool_id << 32) | (id + 1) : 0;
    ref->http_version = version == CURL_HTTP_VERSION_1_0 ? 10
        : version == CURL_HTTP_VERSION_1_1               ? 11
        : version == CURL_HTTP_VERSION_2_0               ? 20
        : version == CURL_HTTP_VERSION_3                 ? 30
                                                         : 0;
    if (ref->http_version) {
        char *effective = NULL;
        char origin[512];
        curl_easy_getinfo(ref->handle, CURLINFO_EFFECTIVE_URL, &effective);
        if (effective && canonical_origin(effective, origin, sizeof(origin)))
            origin_supported(origin, ref->http_version);
    }
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        struct curl_socket_info *socket = &ref->p->ctx->sockets[n];
        bool matches = fd != CURL_SOCKET_BAD && socket->fd == fd;
#if HAVE_SOCKET_PROBES
        int address_size = family == AF_INET ? 4 : 16;
        matches |= family && socket->endpoints_valid && socket->family == family && socket->local_port == local_port
            && socket->peer_port == peer_port && memcmp(socket->local_address, local_address, address_size) == 0
            && memcmp(socket->peer_address, peer_address, address_size) == 0;
#endif
        if (socket->fd != CURL_SOCKET_BAD && matches) {
            socket->connection_id = ref->connection_id;
            socket->http_version = ref->http_version;
            break;
        }
    }
}

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
    ref->last_progress_ns = now;
    ref->requested_h3 = p->opts->http_version == CURL_HTTP_VERSION_NONE &&
        p->http_mode == 3 && (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP3) && !p->http_limit && !origin_limit(p->origin, 0) &&
        !strncmp(p->url, "https://", 8);
    ref->blocked = false;
    ref->measuring = true;
    ref->foreign = foreign_traffic(p->ctx);
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
    ref->foreign |= foreign_traffic(ctx);
    if (succeeded && !ref->blocked && !ref->foreign && !ctx->playback_paused &&
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
            p->ttfb_ms = p->ttfb_ms > 0 ? p->ttfb_ms * 0.75 + latency * 0.25 : latency;
            if (ctx->stream_id == p->stream_id)
                ctx->ttfb_ms = p->ttfb_ms;
        }
        p->phase_completed++;
    } else {
        p->phase_blocked = true;
    }
}

static int64_t record_received_bytes(struct curl_handle_ref *ref, size_t bytes)
{
    int64_t now = mp_time_ns();
    if (!ref->first_byte_ns)
        refresh_handle(ref);
    if (!ref->first_byte_ns)
        ref->first_byte_ns = now;
    ref->received += bytes;
    if (ref->http_version) {
        if (ref->p->phase_http && ref->p->phase_http != ref->http_version)
            ref->p->phase_invalid = true;
        ref->p->phase_http = ref->http_version;
        ref->p->evidence_http = ref->http_version;
    }
    ref->last_progress_ns = now;
    rate_add(&ref->p->ctx->traffic, bytes, now);
    rate_add(&ref->p->ctx->lanes[ref->lane], bytes, now);
    struct priv *p = ref->p;
    if (!p->transitioning && !p->startup && !p->ctx->playback_paused &&
        ref->generation == p->generation)
    {
        p->phase_bytes += bytes;
    }
    return now;
}

static void setup_curl(struct priv *p, CURL *curl);

// Transfers of every core in the process. Another core's traffic shares the
// link, so capacity measured meanwhile is not this core's alone.
static atomic_int process_transfers;

static bool foreign_traffic(struct curl_ctx *ctx)
{
    return ctx->background_network ||
           atomic_load_explicit(&process_transfers, memory_order_relaxed) >
               atomic_load_explicit(&ctx->active_requests, memory_order_relaxed);
}

static CURLMcode add_curl_handle(struct curl_ctx *ctx, CURL *handle)
{
    // Attached here, on the curl thread, while the handle is idle: streams
    // configure their handles before this core's share exists.
    if (ctx->share)
        curl_easy_setopt(handle, CURLOPT_SHARE, ctx->share);
    struct curl_handle_ref *ref = NULL;
    curl_easy_getinfo(handle, CURLINFO_PRIVATE, &ref);
    mp_assert(ref);
    ref->pool = ref->p->pool ? ref->p->pool : ctx->multi;
    ref->pool_id = ref->p->pool_id ? ref->p->pool_id : 1;
    CURLMcode result = curl_multi_add_handle(ref->pool, handle);
    if (result != CURLM_OK)
        return result;
    ref->handle = handle;
    ref->connection_id = 0;
    ref->http_version = 0;
    ref->next = ctx->handles;
    ctx->handles = ref;

    atomic_fetch_add_explicit(&process_transfers, 1, memory_order_relaxed);
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
    struct curl_handle_ref *ref = NULL;
    curl_easy_getinfo(handle, CURLINFO_PRIVATE, &ref);
    CURLMcode result = curl_multi_remove_handle(ref && ref->pool ? ref->pool : ctx->multi, handle);
    if (result == CURLM_OK) {
        struct curl_handle_ref **link = &ctx->handles;
        while (*link && (*link)->handle != handle)
            link = &(*link)->next;
        mp_assert(*link);
        *link = (*link)->next;
        int previous = atomic_fetch_sub_explicit(&ctx->active_requests, 1,
                                                  memory_order_relaxed);
        mp_assert(previous > 0);
        atomic_fetch_sub_explicit(&process_transfers, 1, memory_order_relaxed);
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

// Caller holds state_lock. Retiring an identity preserves lifetime CPU while
// freeing its slot; OS thread-ID reuse never joins unrelated CPU clocks.
static void release_consumer_cpu(struct priv *p)
{
    if (!p->consumer_cpu_slot)
        return;
    struct curl_ctx *ctx = p->ctx;
    struct curl_consumer_cpu *sample = &ctx->consumer_cpu[p->consumer_cpu_slot - 1];
    p->consumer_cpu_slot = 0;
    mp_assert(sample->users > 0);
    if (--sample->users == 0) {
        ctx->retired_consumer_cpu_ns += sample->total_ns;
        ctx->consumer_cpu_incomplete |= !sample->valid;
        *sample = (struct curl_consumer_cpu) { 0 };
    }
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

// One normal and at most one isolated profile pool share one worker/wait.
// A third live profile defers; it never steals another stream's connections.
static bool pool_has_handles(struct curl_ctx *ctx, CURLM *pool)
{
    for (struct curl_handle_ref *ref = ctx->handles; ref; ref = ref->next) {
        if (ref->pool == pool)
            return true;
    }
    return false;
}

static void retire_trial_pool(struct curl_ctx *ctx)
{
    if (!ctx->trial || ctx->trial_users || ctx->in_completions || pool_has_handles(ctx, ctx->trial))
        return;
    curl_multi_cleanup(ctx->trial);
    ctx->trial = NULL;
    mp_curl_budget_release_raw(&ctx->budget, ctx->trial_buffer_charge);
    ctx->trial_buffer_charge = 0;
}

static bool select_stream_pool(struct priv *p)
{
    struct curl_ctx *ctx = p->ctx;
    bool manual = p->opts->receive_mode == 2;
    bool enough = 2ULL * p->opts->receive_bytes <=
        (ctx->budget.desired_extra > ctx->budget.sockets ? ctx->budget.desired_extra - ctx->budget.sockets : 0);
    // Existing committed manual sockets remain valid under an unchanged
    // grant; pressure prevents creating another socket/profile, not delivery.
    if (p->pool == ctx->trial && p->receive_mode == 2 &&
        ctx->budget.desired_extra >= ctx->budget.sockets && p->receive_admitted)
        enough = true;
    int receive_mode = manual && enough ? 2 : 0;
    int http_mode = p->opts->http_mode;
    bool isolated = receive_mode == 2 || http_mode == 3;
    CURLM *selected = ctx->multi;
    int64_t id = 1;
    if (isolated) {
        bool compatible = ctx->trial && ctx->trial_receive_mode == receive_mode &&
            ctx->trial_receive_bytes == (receive_mode == 2 ? p->opts->receive_bytes : 0) &&
            ctx->trial_http_mode == http_mode;
        if (!compatible && ctx->trial) {
            retire_trial_pool(ctx);
            if (ctx->trial) {
                p->last_reason = 16;
                return false;
            }
        }
        if (!ctx->trial) {
            // Curl 8.21 owns one opaque shared receive buffer per multi.
            // It is reported separately, not miscounted as raw range storage.
            uint64_t charge = 0;
            if (!mp_curl_budget_reserve_raw(&ctx->budget, charge)) {
                p->last_reason = 26;
                return false;
            }
            ctx->trial = curl_multi_init();
            if (!ctx->trial) {
                mp_curl_budget_release_raw(&ctx->budget, charge);
                p->last_reason = 9;
                return false;
            }
            ctx->trial_buffer_charge = charge;
            ctx->trial_id = ++ctx->next_pool_id;
            ctx->trial_receive_mode = receive_mode;
            ctx->trial_receive_bytes = receive_mode == 2 ? p->opts->receive_bytes : 0;
            ctx->trial_http_mode = http_mode;
            curl_multi_setopt(ctx->trial, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
            curl_multi_setopt(ctx->trial, CURLMOPT_MAXCONNECTS, 64L);
            curl_multi_setopt(ctx->trial, CURLMOPT_MAX_HOST_CONNECTIONS, 18L);
        }
        selected = ctx->trial;
        id = ctx->trial_id;
    }
    if (p->pool != selected) {
        if (p->pool && p->pool == ctx->trial)
            ctx->trial_users--;
        if (selected == ctx->trial)
            ctx->trial_users++;
        p->pool = selected;
        p->pool_id = id;
    }
    p->receive_mode = receive_mode;
    p->receive_bytes = p->opts->receive_bytes;
    p->http_mode = http_mode;
    p->receive_admitted = !manual || receive_mode == 2;
    if (manual && receive_mode != 2)
        p->last_reason = 25;
    return true;
}

static void run_cmd(void *arg)
{
    struct cmd *c = arg;
    struct curl_ctx *ctx = c->ctx;
    switch (c->kind) {
    case CMD_ADD:
        if (!ctx->budget.baseline)
            ctx->budget.baseline = c->p->opts->buffer_size;
        c->p->buffer_size = MPMAX(2 * CURL_MAX_WRITE_SIZE,
            MPMIN(256 << 10, c->p->opts->buffer_size / 4));
        if (!mp_curl_budget_reserve_raw(&ctx->budget, c->p->buffer_size)) {
            request_failed(c->p);
            break;
        }
        c->p->ring_allocation = c->p->buffer_size;
        c->p->buffer = talloc_size(NULL, c->p->buffer_size);
        if (!c->p->buffer) {
            mp_curl_budget_release_raw(&ctx->budget, c->p->ring_allocation);
            c->p->ring_allocation = 0;
            request_failed(c->p);
            break;
        }
        c->p->receive_admitted = true;
        if (!ctx->epoch && !ctx->streams) {
            ctx->declared_bitrate = ctx->observed_bitrate = ctx->media_bitrate = 0;
            ctx->demand_started_ns = ctx->demand_window_ns = 0;
            ctx->state_updates = 0;
        }
        c->p->epoch = ctx->epoch ? ctx->epoch : ++ctx->raw_epoch;
        c->p->stream_id = ++ctx->next_stream_id;
        c->p->metric_next = ctx->streams;
        ctx->streams = c->p;
        c->p->metric_linked = true;
        if (ctx->epoch) {
            const struct mp_curl_control *policy = &ctx->configured;
            c->p->opts->parallel_requests = policy->max_requests;
            c->p->opts->fixed_requests = policy->fixed_requests;
            c->p->opts->explore = policy->explore;
            c->p->opts->receive_mode = policy->receive_mode;
            c->p->opts->receive_bytes = policy->receive_bytes;
            c->p->opts->http_mode = policy->http_mode;
            c->p->epoch = ctx->epoch;
        }
        if (c->p->opts->fixed_requests)
            c->p->selected_requests = MPMIN(c->p->opts->fixed_requests,
                                           c->p->opts->parallel_requests);
        // One stream never gets one full ring per lane. Shape the queue
        // against the same per-stream bound after host restrictions apply.
        c->p->segment_size = MPMIN(c->p->opts->max_request_size,
            MPMIN(512 << 10, c->p->buffer_size / c->p->opts->parallel_requests));
        if (ctx->epoch && ctx->configured.primary_origin[0]) {
            char origin[512];
            canonical_origin(ctx->configured.primary_origin, origin, sizeof(origin));
            if (!strcmp(origin, c->p->origin)) {
                int64_t wall_ms = (int64_t)time(NULL) * 1000;
                const struct mp_curl_control *policy = &ctx->configured;
                int source = 0;
                for (int n = 0; n < policy->num_samples; n++) {
                    const struct mp_curl_sample *sample = &policy->samples[n];
                    if (sample->receive_profile || sample->requests < 1 || sample->requests > policy->max_requests ||
                        sample->updated_ms > wall_ms || wall_ms - sample->updated_ms > INT64_C(2592000000) ||
                        sample->http_version == 30 || (sample->http_version == 0 && sample->source != 1))
                        continue;
                    if (source > sample->source)
                        continue;
                    if (source < sample->source) {
                        memset(c->p->controller.counts, 0, sizeof(c->p->controller.counts));
                        source = sample->source;
                    }
                    struct mp_curl_count_evidence *e = &c->p->controller.counts[sample->requests - 1];
                    e->capacity = sample->ordered_bps;
                    e->samples = 1;
                    e->sampled_ns = mp_time_ns() - MP_TIME_MS_TO_NS(wall_ms - sample->updated_ms);
                    e->updated_ms = sample->updated_ms;
                    e->source = sample->source;
                    e->profile = (struct mp_curl_profile){.requests = sample->requests,
                        .connections = sample->http_version == 11 ? sample->requests : 1,
                        .http_version = sample->http_version, .range_bytes = sample->range_bytes};
                    c->p->ttfb_ms = MPMAX(c->p->ttfb_ms, sample->ttfb_ms);
                }
                if (!c->p->opts->fixed_requests)
                    c->p->selected_requests = mp_curl_controller_best(&c->p->controller, policy->max_requests);
            }
        }
        c->p->receive_mode = 0;
        c->p->http_mode = c->p->opts->http_mode;
        c->p->receive_bytes = c->p->opts->receive_bytes;
        c->p->pool = ctx->multi;
        c->p->pool_id = 1;
        select_stream_pool(c->p);
        c->p->grant_seen = ctx->budget.desired_extra;
        c->p->startup_ns = mp_time_ns();
        curl_multi_setopt(ctx->multi, CURLMOPT_MAXCONNECTS, 64L);
        curl_multi_setopt(ctx->multi, CURLMOPT_MAX_HOST_CONNECTIONS, 18L);
        c->p->startup_start = c->p->request_start;
        c->p->startup_buffer_generation = ctx->buffer_generation;
        ctx->stream_id = c->p->stream_id;
        ctx->selected_requests = c->p->selected_requests;
        // Seed only the summary; learned concurrency points require real data.
        ctx->capacity = 0;
        ctx->capacity_samples = 0;
        ctx->ttfb_ms = c->p->ttfb_ms;
        memset(ctx->concurrency, 0, sizeof(ctx->concurrency));
        memset(ctx->lane_capacity, 0, sizeof(ctx->lane_capacity));
        memset(ctx->lane_samples, 0, sizeof(ctx->lane_samples));
        MP_TRACE(c->p, "starting curl request at %" PRIu64 "\n", c->p->request_start);
        start_request(c->p);
        break;
    case CMD_REMOVE:
        clear_warm_requests(c->p);
        if (c->p->pool && c->p->pool == ctx->trial) {
            ctx->trial_users--;
            c->p->pool = NULL;
        }
        mp_mutex_lock(&ctx->state_lock);
        release_consumer_cpu(c->p);
        mp_mutex_unlock(&ctx->state_lock);
        end_ring_wait(c->p);
        stop_dynamic(c->p);
        if (c->p->ring_allocation) {
            mp_curl_budget_release_raw(&ctx->budget, c->p->ring_allocation);
            c->p->ring_allocation = 0;
            talloc_free(c->p->buffer);
            c->p->buffer = NULL;
        }
        if (c->p->active) {
            MP_TRACE(c->p, "removing curl handle\n");
            remove_curl_handle(ctx, c->p->curl);
            end_measurement(&c->p->primary_ref, false);
            c->p->active = false;
        }
        if (c->p->metric_linked) {
            struct priv **link = &ctx->streams;
            while (*link != c->p)
                link = &(*link)->metric_next;
            *link = c->p->metric_next;
            c->p->metric_linked = false;
            mp_mutex_lock(&c->p->mtx);
            ctx->delivered_bytes += c->p->delivered_bytes;
            ctx->coalesced_wakeups += c->p->coalesced_wakeups;
            ctx->hol_events += c->p->hol_events;
            ctx->retired_consumer_wait_ns += c->p->consumer_wait_ns;
            ctx->retired_hol_ns += c->p->hol_ns;
            ctx->retired_gap_ns += c->p->gap_ns;
            mp_mutex_unlock(&c->p->mtx);
        }
        break;
    case CMD_UNPAUSE:
        end_ring_wait(c->p);
        // The consumer freed enough buffer space. Clear the pause flag and
        // resume the transfer.
        mp_mutex_lock(&c->p->mtx);
        MP_TRACE(c->p, "resuming curl transfer\n");
        learning_tick(c->p, mp_time_ns());
        c->p->unpause_pending = false;
        c->p->paused = false;
        mp_mutex_unlock(&c->p->mtx);
        curl_easy_pause(c->p->curl, CURLPAUSE_CONT);
        break;
    case CMD_SEEK:
        end_ring_wait(c->p);
        c->p->keep_warm = true;
        stop_dynamic(c->p);
        c->p->keep_warm = false;
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
        // Keep rejection and capacity evidence across seek.
        c->p->startup = c->p->opts->fixed_requests == 0 &&
                         c->p->opts->max_request_size > 0;
        c->p->startup_end = 0;
        c->p->startup_start = c->pos;
        c->p->startup_buffer_generation = ctx->buffer_generation;
        c->p->startup_ns = mp_time_ns();
        c->p->seek_probe = true;
        int best = mp_curl_controller_best(&c->p->controller, c->p->opts->parallel_requests);
        c->p->selected_requests = c->p->opts->fixed_requests ? c->p->opts->fixed_requests : best;
        c->p->urgency = 1;
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
    struct priv *p = clientp;
    struct curl_ctx *ctx = p->ctx;
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
    int udp_wanted = 0, udp_effective = -1, udp_status = 0;
    uint64_t udp_charge = 0;
    if (kind == SOCK_DGRAM) {
        double desired = ctx->media_bitrate / 8 * 1.75 * MPMAX(.05, ctx->lateness_p99_ms * .002);
        udp_wanted = desired > 0 ? (int)MPCLAMP(ceil(desired / (1 << 20)) * (1 << 20),
                                               256 << 10, 8 << 20) : 256 << 10;
        p->udp_requested = udp_wanted;
        udp_charge = 2ULL * udp_wanted;
        bool slot = false;
        for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++)
            slot |= ctx->sockets[n].fd == CURL_SOCKET_BAD;
        if (!slot || !mp_curl_budget_reserve_socket(&ctx->budget, udp_charge)) {
            p->last_reason = 30;
            p->udp_limited = true;
#if defined(_WIN32)
            closesocket(fd);
#else
            close(fd);
#endif
            return CURL_SOCKET_BAD;
        }
        udp_status = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void *)&udp_wanted,
                                sizeof(udp_wanted)) ? 3 : 1;
#if defined(_WIN32)
        int length = sizeof(udp_effective);
#else
        socklen_t length = sizeof(udp_effective);
#endif
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void *)&udp_effective, &length))
            udp_effective = -1;
        p->udp_effective = udp_effective;
        int accounted = udp_effective;
#if defined(__linux__)
        accounted = udp_effective < 0 ? -1 : udp_effective / 2;
#endif
        if (udp_status == 1 && accounted < udp_wanted)
            udp_status = 2;
        double minimum = ctx->media_bitrate / 8 * 1.75 * .025;
        if (udp_status == 3 || accounted < minimum) {
            mp_curl_budget_release_socket(&ctx->budget, udp_charge);
            p->last_reason = 30;
            p->udp_limited = true;
#if defined(_WIN32)
            closesocket(fd);
#else
            close(fd);
#endif
            return CURL_SOCKET_BAD;
        }
    }
    ctx->sockets_opened++;
    ctx->open_socket_count++;
    bool registered = false;
    // A full table only loses observation, never the connection.
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        struct curl_socket_info *info = &ctx->sockets[n];
        if (info->fd == CURL_SOCKET_BAD) {
            *info = (struct curl_socket_info) {
                .fd = fd,
                .tcp = kind == SOCK_STREAM,
                .receive_buffer = kind == SOCK_DGRAM ? udp_effective : -1,
                .receive_requested = udp_wanted,
                .receive_status = udp_status,
                .charge = udp_charge,
                .udp_drops = -1,
                .rtt_ms = -1,
                .generation = ctx->sockets_opened,
                .receive_window = -1,
            };
#if defined(__linux__)
            struct stat stat;
            if (fstat(fd, &stat) == 0)
                info->inode = stat.st_ino;
#endif
            registered = true;
            break;
        }
    }
    ctx->socket_snapshot_truncated |= !registered;
    return fd;
}

static int socket_options_cb(void *clientp, curl_socket_t fd, curlsocktype purpose)
{
    struct priv *p = clientp;
    bool tcp = false;
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        if (p->ctx->sockets[n].fd == fd)
            tcp = p->ctx->sockets[n].tcp;
    }
    if (!tcp)
        return CURL_SOCKOPT_OK;
    int requested = p->receive_mode == 2 ? p->receive_bytes
        : p->opts->receive_mode == 2 ? p->opts->receive_bytes : 0;
    int status = 0, effective = -1;
    // OS-auto and the measured baseline deliberately leave TCP autotuning
    // untouched. A fixed override applies before connect on each new socket.
    uint64_t charge = 0;
    if (purpose == CURLSOCKTYPE_IPCXN && requested > 0) {
        charge = 2ULL * requested;
        if (p->receive_mode != 2 || !mp_curl_budget_reserve_socket(&p->ctx->budget, charge)) {
            charge = 0;
            status = 3;
            p->last_reason = 25;
            p->receive_admitted = false;
        } else {
            status = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void *)&requested,
                                sizeof(requested)) == 0 ? 1 : 3;
            if (status == 3) {
                mp_curl_budget_release_socket(&p->ctx->budget, charge);
                charge = 0;
                p->receive_admitted = false;
            }
        }
    }
#if defined(_WIN32)
    int length = sizeof(effective);
#else
    socklen_t length = sizeof(effective);
#endif
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void *)&effective, &length))
        effective = -1;
    int accounted = effective;
#if defined(__linux__)
    // Linux reports twice the requested value, including kernel overhead.
    accounted = effective < 0 ? -1 : effective / 2;
#endif
    if (status == 1 && accounted < requested)
        status = 2;
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        struct curl_socket_info *info = &p->ctx->sockets[n];
        if (info->fd == fd && info->tcp) {
            info->charge = charge;
            info->receive_requested = requested;
            info->receive_status = status;
            info->receive_buffer = effective;
        }
    }
    if (status >= 2 && p->last_reason != 25)
        p->last_reason = 34;
    return CURL_SOCKOPT_OK; // A clamp or denied override never fails playback.
}

static int close_socket_cb(void *clientp, curl_socket_t fd)
{
    struct curl_ctx *ctx = clientp;
    ctx->open_socket_count--;
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        if (ctx->sockets[n].fd == fd) {
            mp_curl_budget_release_socket(&ctx->budget, ctx->sockets[n].charge);
            ctx->sockets[n].fd = CURL_SOCKET_BAD;
        }
    }
#if defined(_WIN32)
    return closesocket(fd);
#else
    return close(fd);
#endif
}

static void sample_udp_drops(struct curl_ctx *ctx)
{
#if defined(__linux__) && !defined(__ANDROID__)
    const char *paths[] = {"/proc/net/udp", "/proc/net/udp6"};
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++)
        ctx->sockets[n].udp_drops = -1;
    for (int path = 0; path < MP_ARRAY_SIZE(paths); path++) {
        FILE *file = fopen(paths[path], "r");
        if (!file)
            continue;
        char line[1024];
        while (fgets(line, sizeof(line), file)) {
            unsigned long long inode, drops;
            if (sscanf(line, "%*s %*s %*s %*s %*s %*s %*s %*s %*s %llu %*s %*s %llu",
                       &inode, &drops) != 2)
                continue;
            for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
                struct curl_socket_info *socket = &ctx->sockets[n];
                if (socket->fd != CURL_SOCKET_BAD && !socket->tcp && socket->inode == inode)
                    socket->udp_drops = drops <= INT64_MAX ? drops : -1;
            }
        }
        fclose(file);
    }
#endif
}

static void sample_sockets(struct curl_ctx *ctx)
{
    sample_udp_drops(ctx);
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        struct curl_socket_info *info = &ctx->sockets[n];
        if (info->fd == CURL_SOCKET_BAD)
            continue;
        int buffer = 0;
#if defined(_WIN32)
        int len = sizeof(buffer);
#else
        socklen_t len = sizeof(buffer);
#endif
        info->receive_buffer = getsockopt(info->fd, SOL_SOCKET, SO_RCVBUF, (void *)&buffer, &len) == 0 ? buffer : -1;
        struct sockaddr_storage local = { 0 }, peer = { 0 };
#if defined(_WIN32)
        int local_len = sizeof(local), peer_len = sizeof(peer);
#else
        socklen_t local_len = sizeof(local), peer_len = sizeof(peer);
#endif
        info->endpoints_valid = false;
        if (getsockname(info->fd, (struct sockaddr *)&local, &local_len) == 0
            && getpeername(info->fd, (struct sockaddr *)&peer, &peer_len) == 0 && local.ss_family == peer.ss_family) {
            info->family = local.ss_family;
            if (local.ss_family == AF_INET && local_len >= sizeof(struct sockaddr_in)
                && peer_len >= sizeof(struct sockaddr_in)) {
                const struct sockaddr_in *l = (const struct sockaddr_in *)&local;
                const struct sockaddr_in *r = (const struct sockaddr_in *)&peer;
                info->local_port = ntohs(l->sin_port);
                info->peer_port = ntohs(r->sin_port);
                memcpy(info->local_address, &l->sin_addr, 4);
                memcpy(info->peer_address, &r->sin_addr, 4);
                info->endpoints_valid = true;
            } else if (local.ss_family == AF_INET6 && local_len >= sizeof(struct sockaddr_in6)
                && peer_len >= sizeof(struct sockaddr_in6)) {
                const struct sockaddr_in6 *l = (const struct sockaddr_in6 *)&local;
                const struct sockaddr_in6 *r = (const struct sockaddr_in6 *)&peer;
                info->local_port = ntohs(l->sin6_port);
                info->peer_port = ntohs(r->sin6_port);
                memcpy(info->local_address, &l->sin6_addr, 16);
                memcpy(info->peer_address, &r->sin6_addr, 16);
                info->endpoints_valid = true;
            }
        }
        info->rtt_ms = -1;
        info->receive_window = -1;
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
#elif defined(_WIN32) && defined(SIO_TCP_INFO)
        ULONG version = 0;
        TCP_INFO_v0 tcp = { 0 };
        DWORD returned = 0;
        if (WSAIoctl(info->fd, SIO_TCP_INFO, &version, sizeof(version), &tcp, sizeof(tcp), &returned, NULL, NULL)
            == 0) {
            if (returned >= offsetof(TCP_INFO_v0, RttUs) + sizeof(tcp.RttUs))
                info->rtt_ms = tcp.RttUs / 1000.0;
            if (returned >= offsetof(TCP_INFO_v0, RcvWnd) + sizeof(tcp.RcvWnd))
                info->receive_window = tcp.RcvWnd;
            if (returned >= offsetof(TCP_INFO_v0, RcvBuf) + sizeof(tcp.RcvBuf))
                info->receive_buffer = tcp.RcvBuf;
        }
#endif
    }
}
#endif

// CPU clocks and rate scans run at most once per controller tick, never per
// body chunk or metrics request. A shared worker cannot attribute a mixed-
// protocol window, so those windows explicitly have no protocol CPU sample.
static double lateness_percentile(struct curl_ctx *ctx, unsigned percent)
{
    uint64_t rank = (ctx->lateness_samples * percent + 99) / 100;
    uint64_t count = 0;
    int64_t upper_ns = 0;
    for (int n = 0; n < MP_ARRAY_SIZE(ctx->lateness_bins); n++) {
        count += ctx->lateness_bins[n];
        if (count >= rank)
            return MPMAX(upper_ns, n == MP_ARRAY_SIZE(ctx->lateness_bins) - 1 ? ctx->lateness_window_max_ns : 0) / 1e6;
        upper_ns = n == 0 ? 1000 : upper_ns * 2;
    }
    return upper_ns / 1e6;
}

static void measurement_tick(struct curl_ctx *ctx, int64_t now)
{
    if (now < ctx->next_tick_ns)
        return;
    ctx->next_tick_ns = now + RATE_BUCKET_NS;
    if (now >= ctx->next_lateness_window_ns) {
        ctx->lateness_available = ctx->lateness_samples > 0;
        ctx->lateness_p50_ms = ctx->lateness_available ? lateness_percentile(ctx, 50) : 0;
        ctx->lateness_p99_ms = ctx->lateness_available ? lateness_percentile(ctx, 99) : 0;
        memset(ctx->lateness_bins, 0, sizeof(ctx->lateness_bins));
        ctx->lateness_samples = 0;
        ctx->lateness_window_max_ns = 0;
        ctx->next_lateness_window_ns = now + MP_TIME_S_TO_NS(1);
    }
    int64_t cpu = mp_thread_cpu_time_ns(mp_thread_current_id());
    ctx->thread_cpu_ns = cpu;
    if (ctx->cpu_window_ns && now > ctx->cpu_window_ns && ctx->last_cpu_ns >= 0 && cpu >= ctx->last_cpu_ns) {
        ctx->worker_cpu = (double)(cpu - ctx->last_cpu_ns) / (now - ctx->cpu_window_ns);
        ctx->worker_cpu_available = true;
    } else {
        ctx->worker_cpu_available = false;
    }
    ctx->cpu_window_ns = now;
    bool available = ctx->last_cpu_ns >= 0 && cpu >= ctx->last_cpu_ns;
    int protocol = -1;
    for (int n = 0; n < 4; n++) {
        if (ctx->protocol_ordered[n] != ctx->protocol_previous[n])
            protocol = protocol == -1 && n < 3 ? n : -2;
    }
    for (int n = 0; n < 3; n++) {
        uint64_t bytes = ctx->protocol_ordered[n] - ctx->protocol_previous[n];
        ctx->protocol_metrics[n] = (struct mp_curl_protocol_metrics) {
            .http_version = n == 0 ? 11
                : n == 1           ? 20
                                   : 30,
            .ordered_bytes = ctx->protocol_ordered[n],
            .cpu_available = available && protocol == n && bytes > 0,
            .cpu_ns_per_ordered_byte
            = available && protocol == n && bytes > 0 ? (double)(cpu - ctx->last_cpu_ns) / bytes : 0,
        };
    }
    memcpy(ctx->protocol_previous, ctx->protocol_ordered, sizeof(ctx->protocol_previous));
    ctx->last_cpu_ns = cpu;
    rate_current(&ctx->traffic, now);
    for (int n = 0; n < MP_CURL_MAX_LANES; n++)
        rate_current(&ctx->lanes[n], now);
}

static MP_THREAD_VOID curl_thread(void *arg)
{
    mp_thread_set_name("curl");
    struct curl_ctx *ctx = arg;
    ctx->next_pool_id = 1;
    ctx->thread_started_ns = mp_time_ns();
    ctx->thread_cpu_started_ns =
        mp_thread_cpu_time_ns(mp_thread_current_id());
    ctx->thread_cpu_ns = ctx->last_cpu_ns = ctx->thread_cpu_started_ns;
    ctx->next_tick_ns = ctx->thread_started_ns + RATE_BUCKET_NS;
    ctx->next_lateness_window_ns = ctx->thread_started_ns + MP_TIME_S_TO_NS(1);
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

        ctx->iterations++;
        int64_t dispatch_start = mp_time_ns();
        mp_dispatch_queue_process(ctx->dispatch, 0);
        apply_playback_state(ctx);
        ctx->dispatch_ns += mp_time_ns() - dispatch_start;
        measurement_tick(ctx, mp_time_ns());
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
        int64_t perform_start = mp_time_ns();
        CURLMcode mres = curl_multi_perform(ctx->multi, &running);
        if (mres == CURLM_OK && ctx->trial)
            mres = curl_multi_perform(ctx->trial, &running);
        ctx->perform_ns += mp_time_ns() - perform_start;
        if (mres != CURLM_OK && mres != CURLM_CALL_MULTI_PERFORM) {
            set_failed(ctx, curl_multi_strerror(mres));
            continue;
        }

        CURLMsg *msg;
        int left = 0;
        dispatch_start = mp_time_ns();
        CURLM *pools[] = {ctx->multi, ctx->trial};
        ctx->in_completions = true;
        for (int pool = 0; pool < MP_ARRAY_SIZE(pools); pool++) {
        while (pools[pool] && (msg = curl_multi_info_read(pools[pool], &left))) {
            if (msg->msg != CURLMSG_DONE)
                continue;
            struct curl_handle_ref *ref = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &ref);
            mp_assert(ref && ref->p);
            refresh_handle(ref);
            if (ref->requested_h3 && ref->http_version > 0 && ref->http_version < 30 &&
                !ref->p->udp_limited) {
                downgrade_http(ref->p, ref, CURLE_QUIC_CONNECT_ERROR);
                ref->fresh_connection = false; // the TCP winner is reusable
            }
            CURLcode result = msg->data.result;
            // A connection close must not turn a short validated HTTP body
            // into successful EOF even if a QUIC backend reports CURLE_OK.
            if (result == CURLE_OK && ref->kind == CURL_HANDLE_PRIMARY &&
                ref->p->seekable && ref->p->reply_ranged && ref->p->response_ok) {
                curl_off_t expected = -1;
                curl_easy_getinfo(ref->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &expected);
                if (expected >= 0 && ref->p->request_received < expected)
                    result = CURLE_PARTIAL_FILE;
            }
            if (result == CURLE_OK && ref->kind == CURL_HANDLE_DYNAMIC) {
                struct dynamic_request *request = (struct dynamic_request *)ref;
                if (request->response_ok && request->fetch_pos < request->length)
                    result = CURLE_PARTIAL_FILE;
            }
            bool succeeded = result == CURLE_OK;
            if (ref->kind == CURL_HANDLE_WARM) {
                succeeded = false;
            } else if (ref->kind == CURL_HANDLE_PRIMARY) {
                succeeded = succeeded && ref->p->response_ok &&
                            !atomic_load_explicit(&ref->p->aborted,
                                                  memory_order_relaxed);
            } else {
                struct dynamic_request *request = (struct dynamic_request *)ref;
                succeeded = succeeded && request->response_ok && request->range && request->fetch_pos == request->length
                    && !atomic_load_explicit(&request->ref.p->aborted, memory_order_relaxed);
            }
            remove_curl_handle(ctx, msg->easy_handle);
            end_measurement(ref, succeeded);
            finish_curl_request(ctx, succeeded);
            if (ref->kind == CURL_HANDLE_WARM) {
                struct warm_request **link = &ctx->warm;
                while (*link && &(*link)->ref != ref)
                    link = &(*link)->next;
                if (*link) {
                    struct warm_request *warm = *link;
                    *link = warm->next;
                    curl_easy_cleanup(ref->handle);
                    curl_slist_free_all(warm->headers);
                    talloc_free(warm);
                }
            } else if (ref->kind == CURL_HANDLE_PRIMARY) {
                ref->p->active = false;
                on_done(ref->p, result);
            } else {
                struct dynamic_request *request = (struct dynamic_request *)ref;
                request->active = false;
                on_dynamic_done(request, result);
            }
        }

        }
        ctx->in_completions = false;
        int64_t service_now = mp_time_ns();
        for (struct priv *p = ctx->streams; p; p = p->metric_next) {
            if (p->dynamic_started && service_now >= p->next_service_ns) {
                p->next_service_ns = service_now + RATE_BUCKET_NS;
                rescue_stalled_range(p, service_now);
                fill_dynamic(p);
            }
        }
        retire_trial_pool(ctx);
        ctx->dispatch_ns += mp_time_ns() - dispatch_start;
        // Wait until libcurl's own next deadline (at most 250 ms). A wait
        // that runs to its deadline and returns late measures how long this
        // thread waited for a CPU, the jitter a receive buffer must absorb.
        long timeout = -1;
        curl_multi_timeout(ctx->multi, &timeout);
        long trial_timeout = -1;
        if (ctx->trial) {
            curl_multi_timeout(ctx->trial, &trial_timeout);
            if (trial_timeout >= 0)
                timeout = timeout < 0 ? trial_timeout : MPMIN(timeout, trial_timeout);
        }
        if (timeout < 0 || timeout > 250)
            timeout = 250;
        int64_t poll_start = mp_time_ns();
        int64_t next = ctx->next_tick_ns;
#if HAVE_SOCKET_PROBES
        next = MPMIN(next, ctx->next_socket_sample_ns);
#endif
        timeout = MPMIN(timeout, MPMAX(0, (next - poll_start) / 1000000));
        int64_t deadline = poll_start + timeout * INT64_C(1000000);
        int ready = 0;
        struct curl_waitfd trial_fds[MP_CURL_MAX_SOCKETS];
        unsigned int trial_count = 0;
        if (ctx->trial) {
            CURLMcode wait_result = curl_multi_waitfds(ctx->trial, trial_fds,
                MP_ARRAY_SIZE(trial_fds), &trial_count);
            if (wait_result != CURLM_OK || trial_count > MP_ARRAY_SIZE(trial_fds)) {
                // Descriptor pressure never becomes a busy loop.
                trial_count = MPMIN(trial_count, MP_ARRAY_SIZE(trial_fds));
                timeout = MPMIN(timeout, 10);
            }
        }
        curl_multi_poll(ctx->multi, trial_fds, trial_count, timeout, &ready);
        int64_t poll_end = mp_time_ns();
        ctx->poll_ns += poll_end - poll_start;
        int64_t late = poll_end - deadline;
        if (!ready && late >= 0) {
            int bucket = 0;
            int64_t upper_ns = 0;
            while (late > upper_ns && bucket < MP_ARRAY_SIZE(ctx->lateness_bins) - 1) {
                upper_ns = bucket == 0 ? 1000 : upper_ns * 2;
                bucket++;
            }
            ctx->lateness_bins[bucket]++;
            ctx->lateness_samples++;
            ctx->lateness_window_max_ns = MPMAX(ctx->lateness_window_max_ns, late);
            atomic_store_explicit(&ctx->recent_lateness_ns,
                MPMAX(late, atomic_load_explicit(&ctx->recent_lateness_ns,
                                                 memory_order_relaxed)),
                memory_order_relaxed);
            if (late > ctx->peak_lateness_ns)
                ctx->peak_lateness_ns = late;
        }
    }

    if (ctx->trial)
        curl_multi_cleanup(ctx->trial);
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

// The active handle registry is owned by the worker and has no playback cap.
// Quadratic de-duplication is only on snapshots, never the body hot path.
static void read_connections(struct curl_ctx *ctx, struct mp_curl_metrics *metrics)
{
    for (struct curl_handle_ref *ref = ctx->handles; ref; ref = ref->next)
        refresh_handle(ref);
    for (struct curl_handle_ref *ref = ctx->handles; ref; ref = ref->next) {
        if (!ref->connection_id)
            continue;
        metrics->streams_per_connection++;
        bool seen = false;
        for (struct curl_handle_ref *other = ctx->handles; other != ref; other = other->next)
            seen |= other->connection_id == ref->connection_id;
        if (seen)
            continue;
        metrics->physical_connections++;
        if (metrics->num_connections == MP_CURL_MAX_METRIC_CONNECTIONS) {
            metrics->snapshot_truncated = true;
            continue;
        }
        struct mp_curl_connection_metrics *entry = &metrics->connections[metrics->num_connections++];
        *entry = (struct mp_curl_connection_metrics) {
            .id = ref->connection_id,
            .http_version = ref->http_version,
            .udp_drops = -1,
            .receive_buffer = -1,
            .receive_window = -1,
            .rtt_ms = -1,
        };
        for (struct curl_handle_ref *other = ref; other; other = other->next)
            entry->active_requests += other->connection_id == entry->id;
        for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
            struct curl_socket_info *socket = &ctx->sockets[n];
            if (socket->fd != CURL_SOCKET_BAD && socket->connection_id == entry->id) {
                entry->socket_generation = socket->generation;
                entry->transport = socket->tcp ? 1 : 2;
                entry->udp_drops = socket->udp_drops;
                entry->receive_requested = socket->receive_requested;
                entry->receive_status = socket->receive_status;
                entry->receive_buffer = socket->receive_buffer;
                entry->receive_window = socket->receive_window;
                entry->rtt_ms = socket->rtt_ms;
                break;
            }
        }
    }
    if (metrics->physical_connections)
        metrics->streams_per_connection /= metrics->physical_connections;
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
        // Aggregate socket counts include descriptors outside the snapshot.
        metrics->max_socket_receive_buffer =
            MPMAX(metrics->max_socket_receive_buffer, info->receive_buffer);
        if (info->rtt_ms >= 0) {
            metrics->tcp_rtt_min_ms = metrics->tcp_rtt_min_ms < 0
                ? info->rtt_ms : MPMIN(metrics->tcp_rtt_min_ms, info->rtt_ms);
            metrics->tcp_rtt_max_ms = MPMAX(metrics->tcp_rtt_max_ms, info->rtt_ms);
        }
    }
    metrics->open_sockets = ctx->open_socket_count;
    metrics->snapshot_truncated |= ctx->socket_snapshot_truncated;
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
    int64_t cpu = ctx->thread_cpu_ns;
    bool cpu_available = ctx->thread_cpu_started_ns >= 0 && cpu >= ctx->thread_cpu_started_ns;
    double elapsed = ctx->traffic.started_ns
        ? MP_TIME_NS_TO_S(now - ctx->traffic.started_ns) : 0;
    int64_t current = rate_current(&ctx->traffic, now);
    *metrics = (struct mp_curl_metrics) {
        .baseline_raw_bytes = ctx->budget.baseline,
        .allocated_raw_bytes = ctx->budget.raw,
        .reserved_socket_bytes = ctx->budget.sockets,
        .desired_extra_bytes = ctx->budget.desired_extra,
        .committed_extra_bytes = mp_curl_budget_committed(&ctx->budget),
        .active_requests = atomic_load_explicit(&ctx->active_requests, memory_order_relaxed),
        .peak_active_requests = atomic_load_explicit(&ctx->peak_active_requests, memory_order_relaxed),
        .active_streams = atomic_load_explicit(&ctx->active_streams, memory_order_relaxed),
        .peak_active_streams = atomic_load_explicit(&ctx->peak_active_streams, memory_order_relaxed),
        .requests_per_stream = atomic_load_explicit(&ctx->requests_per_stream, memory_order_relaxed),
        .range_bytes = atomic_load_explicit(&ctx->range_bytes, memory_order_relaxed),
        .ring_bytes = atomic_load_explicit(&ctx->ring_bytes, memory_order_relaxed),
        .started_requests = atomic_load_explicit(&ctx->started_requests, memory_order_relaxed),
        .finished_requests = atomic_load_explicit(&ctx->finished_requests, memory_order_relaxed),
        .failed_requests = atomic_load_explicit(&ctx->failed_requests, memory_order_relaxed),
        .retry_attempts = atomic_load_explicit(&ctx->retry_attempts, memory_order_relaxed),
        .received_bytes = ctx->traffic.bytes,
        .average_received_bytes_per_second = elapsed > 0 ? ctx->traffic.bytes / elapsed : 0,
        .peak_received_bytes_per_second = ctx->traffic.peak,
        .current_received_bytes_per_second = current,
        .elapsed_seconds = elapsed,
        .curl_thread_cpu_seconds = cpu_available ? MP_TIME_NS_TO_S(cpu - MPMAX(ctx->thread_cpu_started_ns, 0)) : 0,
        .curl_thread_wall_seconds = MP_TIME_NS_TO_S(now - ctx->thread_started_ns),
        .curl_thread_cpu_available = cpu_available,
        .ring_wait_seconds
        = MP_TIME_NS_TO_S(ctx->ring_wait.active_ns + ctx->ring_wait.users * (now - ctx->ring_wait.active_started_ns)),
        .consumer_wait_seconds = MP_TIME_NS_TO_S(ctx->retired_consumer_wait_ns),
        .head_of_line_seconds = MP_TIME_NS_TO_S(ctx->retired_hol_ns),
        .reorder_wait_seconds = MP_TIME_NS_TO_S(ctx->retired_gap_ns),
        .peak_reorder_bytes = ctx->peak_reorder_bytes,
        .ordered_available_bytes = ctx->ordered_bytes,
        .consumer_delivered_bytes = ctx->delivered_bytes,
        .copy_in_bytes = ctx->copy_in_bytes,
        .body_callbacks = ctx->body_callbacks,
        .worker_iterations = ctx->iterations,
        .coalesced_wakeups = ctx->coalesced_wakeups,
        .head_of_line_events = ctx->hol_events,
        .perform_wall_seconds = MP_TIME_NS_TO_S(ctx->perform_ns),
        .dispatch_wall_seconds = MP_TIME_NS_TO_S(ctx->dispatch_ns),
        .poll_wall_seconds = MP_TIME_NS_TO_S(ctx->poll_ns),
        .callback_time_included = true,
        .playback_epoch = ctx->epoch,
        .stale_controls = ctx->stale_controls,
        .rejected_controls = ctx->rejected_controls,
        .background_network = ctx->background_network,
        .declared_bitrate = ctx->declared_bitrate,
        .playback_state_updates = ctx->state_updates,
        .playback_buffer_seconds = ctx->buffer_seconds,
        .playback_demand_bitrate = MPMAX(0, ctx->media_bitrate),
        .playback_user_paused = ctx->playback_paused,
        .recent_lateness_ms = atomic_exchange_explicit(&ctx->recent_lateness_ns, 0, memory_order_relaxed) / 1e6,
        .peak_lateness_ms = ctx->peak_lateness_ns / 1e6,
        .lateness_p50_ms = ctx->lateness_p50_ms,
        .lateness_p99_ms = ctx->lateness_p99_ms,
        .lateness_available = ctx->lateness_available,
        .selected_requests = ctx->selected_requests,
        .ttfb_ms = ctx->ttfb_ms,
        .capacity_bytes_per_second = ctx->capacity,
        .capacity_samples = ctx->capacity_samples,
        .stream_id = ctx->stream_id,
    };
    mp_mutex_lock(&ctx->state_lock);
    metrics->consumer_cpu_seconds = MP_TIME_NS_TO_S(ctx->retired_consumer_cpu_ns);
    metrics->consumer_cpu_available = ctx->consumer_cpu_seen && !ctx->consumer_cpu_incomplete;
    for (int n = 0; n < MP_CURL_MAX_CONSUMERS; n++) {
        if (!ctx->consumer_cpu[n].users)
            continue;
        metrics->consumer_cpu_seconds += MP_TIME_NS_TO_S(ctx->consumer_cpu[n].total_ns);
        metrics->consumer_cpu_available &= ctx->consumer_cpu[n].valid;
    }
    metrics->snapshot_truncated |= ctx->consumer_snapshot_truncated;
    mp_mutex_unlock(&ctx->state_lock);
    snapshot_origins(metrics, now);
    read_socket_metrics(ctx, metrics);
    read_connections(ctx, metrics);
    memcpy(metrics->protocols, ctx->protocol_metrics, sizeof(metrics->protocols));
    for (int n = 0; n < 3; n++) {
        metrics->protocols[n].http_version = n == 0 ? 11 : n == 1 ? 20 : 30;
        metrics->protocols[n].ordered_bytes = ctx->protocol_ordered[n];
    }
    for (struct priv *p = ctx->streams; p; p = p->metric_next) {
        mp_mutex_lock(&p->mtx);
        now = mp_time_ns();
        uint64_t readable = stream_readable(p);
        uint64_t retained = p->count + p->queued_bytes;
        uint64_t reorder = stream_reorder(p);
        metrics->readable_bytes += readable;
        metrics->retained_bytes += retained;
        metrics->reorder_bytes += reorder;
        metrics->consumer_delivered_bytes += p->delivered_bytes;
        metrics->coalesced_wakeups += p->coalesced_wakeups;
        metrics->head_of_line_events += p->hol_events;
        metrics->consumer_wait_seconds += MP_TIME_NS_TO_S(
            p->consumer_wait_ns + (p->consumer_wait_started_ns ? now - p->consumer_wait_started_ns : 0));
        metrics->head_of_line_seconds += MP_TIME_NS_TO_S(p->hol_ns + (p->hol_started_ns ? now - p->hol_started_ns : 0));
        metrics->reorder_wait_seconds += MP_TIME_NS_TO_S(p->gap_ns + (p->gap_started_ns ? now - p->gap_started_ns : 0));
        if (p->gap_started_ns)
            metrics->oldest_gap_seconds = MPMAX(metrics->oldest_gap_seconds, MP_TIME_NS_TO_S(now - p->gap_started_ns));
        if (metrics->num_streams < MP_CURL_MAX_METRIC_STREAMS) {
            metrics->streams[metrics->num_streams++] = (struct mp_curl_stream_metrics) {
                .id = p->stream_id,
                .effective_http_version = p->primary_ref.http_version,
                .http_fallbacks = p->http_fallbacks,
                .urgency = p->urgency,
                .udp_requested = p->udp_requested,
                .udp_effective = p->udp_effective,
                .last_reason = p->last_reason,
                .controller_state = p->controller.state,
                .readable_bytes = readable,
                .retained_bytes = retained,
                .reorder_bytes = reorder,
                .prefix_publication_available = p->etag != NULL,
            };
        } else {
            metrics->snapshot_truncated = true;
        }
        mp_mutex_unlock(&p->mtx);
    }
    for (struct priv *p = ctx->streams; p; p = p->metric_next) {
        if (p->stream_id != ctx->stream_id)
            continue;
        char configured[512], actual[512];
        char *effective = NULL;
        curl_easy_getinfo(p->curl, CURLINFO_EFFECTIVE_URL, &effective);
        metrics->primary_origin_match = ctx->configured.primary_origin[0] && effective &&
            canonical_origin(ctx->configured.primary_origin, configured, sizeof(configured)) &&
            canonical_origin(effective, actual, sizeof(actual)) && !strcmp(configured, actual);
        for (int n = 0; n < p->opts->parallel_requests; n++) {
            const struct mp_curl_count_evidence *e = &p->controller.counts[n];
            if (!e->playback_samples || !p->receive_admitted || metrics->num_samples >= MP_CURL_MAX_SAMPLES)
                continue;
            metrics->samples[metrics->num_samples++] = (struct mp_curl_sample){
                .requests = n + 1, .http_version = e->profile.http_version,
                .receive_profile = e->profile.receive_profile,
                .receive_bytes = e->profile.receive_bytes,
                .range_bytes = e->profile.range_bytes, .ordered_bps = e->capacity,
                .ttfb_ms = p->ttfb_ms, .updated_ms = e->updated_ms,
                .source = 2, .samples = e->playback_samples,
            };
        }
    }
    metrics->copy_out_bytes = metrics->consumer_delivered_bytes;
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
    struct mp_curl_control control;
    bool ok;
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

// Average demand implied by the facts, best first: what is delivered, then
// the original only when the original is what is delivered.
static double declared_demand(const struct mp_curl_control *c)
{
    if (c->delivery_bitrate > 0)
        return c->delivery_bitrate;
    if (c->delivery_size > 0 && c->delivery_duration > 0)
        return c->delivery_size * 8.0 / c->delivery_duration;
    if (c->direct_file && c->source_bitrate > 0)
        return c->source_bitrate;
    if (c->direct_file && c->source_size > 0 && c->source_duration > 0)
        return c->source_size * 8.0 / c->source_duration;
    return 0;
}

static void set_background_network(struct curl_ctx *ctx, bool active)
{
    if (ctx->background_network != active)
        ctx->pause_serial++; // evidence straddling the change is invalid
    ctx->background_network = active;
}

static void set_control(void *arg)
{
    struct curl_control *call = arg;
    struct curl_ctx *ctx = call->ctx;
    const struct mp_curl_control *c = &call->control;
    call->ok = true;
    if (c->op == 0) {
        if (c->epoch < ctx->epoch) {
            ctx->stale_controls++;
            call->ok = false;
        } else if (c->epoch == ctx->epoch) {
            call->ok = false;
            ctx->rejected_controls++;
        } else {
            ctx->epoch = c->epoch;
            ctx->demand_started_ns = ctx->demand_window_ns = 0;
            ctx->demand_ewma = ctx->recent_demand = 0;
            ctx->budget.desired_extra = 0;
            ctx->configured = *c;
            ctx->declared_bitrate = declared_demand(c);
            ctx->media_bitrate = MPMAX(ctx->observed_bitrate, ctx->declared_bitrate);
            set_background_network(ctx, false);
        }
    } else if (c->epoch != ctx->epoch) {
        ctx->stale_controls++;
        call->ok = false;
    } else {
        ctx->budget.desired_extra = c->joint_extra_grant;
        set_background_network(ctx, c->background_network);
    }
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
    ctx->state_updates++;
    ctx->buffer_generation = ctx->state_updates;
    if (state.seeking != ctx->playback_seeking)
        ctx->pause_serial++; // a seek invalidates in-flight capacity evidence
    ctx->playback_seeking = state.seeking;
    set_playback_paused(ctx, state.user_paused);
    ctx->playback_state = state;
    ctx->buffer_seconds = MPMAX(0, state.buffer_seconds);
    double observed = MPMAX(0, state.audio_bitrate) + MPMAX(0, state.video_bitrate);
    int64_t now = mp_time_ns();
    if (observed > 0 && isfinite(observed)) {
        if (!ctx->demand_started_ns) {
            ctx->demand_started_ns = now;
            ctx->demand_window_ns = now;
            ctx->demand_ewma = observed;
        } else {
            ctx->demand_ewma += (1 - exp(-.25 / 5)) * (observed - ctx->demand_ewma);
        }
        if (now - ctx->demand_window_ns >= MP_TIME_S_TO_NS(1)) {
            ctx->recent_demand = observed;
            ctx->demand_window_ns = now;
        } else {
            ctx->recent_demand = MPMAX(ctx->recent_demand, observed);
        }
    }
    ctx->observed_bitrate = observed;
    ctx->media_bitrate = ctx->demand_started_ns && now - ctx->demand_started_ns >= MP_TIME_S_TO_NS(2)
        ? MPMAX(ctx->demand_ewma, ctx->recent_demand)
        : ctx->declared_bitrate * MPMAX(0, state.speed);

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

bool mp_curl_set_control(struct mpv_global *global,
                         const struct mp_curl_control *control)
{
    if (!global || !global->curl)
        return false;
    struct curl_control call = { .ctx = global->curl, .control = *control };
    mp_dispatch_run(global->curl->dispatch, set_control, &call);
    return call.ok;
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
    p->ctx->copy_in_bytes += len;
    p->ctx->ordered_bytes += len;
    p->total_ordered += len;
    p->ctx->protocol_ordered[protocol_index(p->primary_ref.http_version)] += len;
}


// Called per chunk of body data.
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct priv *p = userdata;
    size_t bytes = size * nmemb;
    p->ctx->body_callbacks++;

    // Reject error bodies and servers that returned the wrong byte range,
    // including a chunked range reply longer than the range.
    if (!p->response_ok
        || (p->reply_ranged && p->primary_end && bytes > p->primary_end - p->request_start - p->request_received)) {
        return CURL_WRITEFUNC_ERROR;
    }

    if (atomic_load_explicit(&p->aborted, memory_order_relaxed))
        return CURL_WRITEFUNC_ERROR;
    // libcurl replays a paused body callback. Account the fresh download at
    // first arrival (including a chunk that cannot enter the ring yet), never
    // the replay or the later ordered-buffer memcpy.
    int64_t now = p->paused_bytes ? mp_time_ns() : record_received_bytes(&p->primary_ref, bytes);

    mp_mutex_lock(&p->mtx);

    if (p->buffer_size - p->count < bytes) {
        // No room in the buffer. Pause the transfer and wait for the consumer.
        learning_tick(p, now);
        begin_ring_wait(p, now);
        p->paused = true;
        p->paused_bytes = bytes;
        p->primary_ref.blocked = true;
        MP_TRACE(p, "pausing curl transfer, buffer full (%zu bytes)\n", p->count);
        mp_mutex_unlock(&p->mtx);
        return CURL_WRITEFUNC_PAUSE;
    }

    mp_icy_process(p->icy, ptr, bytes, ring_write, p);
    update_gap(p, now);

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
        p->ctx->protocol_ordered[range->protocol_index] += p->ordered_pos - MPMAX(before, range->start);
        if (!range->complete)
            break;
    }
    return p->ordered_pos - before;
}

// Strong validators authorize prefix publication and exact suffix retry.
// Otherwise retain the body privately until libcurl confirms completion.
static void publish_range(struct priv *p, struct range_desc *range, int64_t now, bool finished)
{
    mp_mutex_lock(&p->mtx);
    int64_t old_reorder = stream_reorder(p);
    p->queued_bytes += range->received - range->retained;
    range->retained = range->received;
    if (p->etag || finished)
        range->published = range->received;
    range->complete = range->received == range->end - range->start && (p->etag || finished);
    uint64_t ordered = advance_ordered(p);
    p->ctx->ordered_bytes += ordered;
    p->total_ordered += ordered;
    p->ctx->reorder_bytes += (int64_t)stream_reorder(p) - old_reorder;
    p->ctx->peak_reorder_bytes = MPMAX(p->ctx->peak_reorder_bytes, p->ctx->reorder_bytes);
    update_gap(p, now);
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
    p->ctx->body_callbacks++;

    if (!range || !request->response_ok || atomic_load_explicit(&p->aborted, memory_order_relaxed)
        || bytes > request->length - request->fetch_pos) {
        return CURL_WRITEFUNC_ERROR;
    }
    int64_t now = record_received_bytes(&request->ref, bytes);

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
    p->ctx->copy_in_bytes += bytes - overlap;
    range->protocol_index = protocol_index(request->ref.http_version);
    request->fetch_pos = pos + bytes;
    if (request->fetch_pos > range->received) {
        range->received = request->fetch_pos;
        publish_range(p, range, now, false);
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
    // Every range belongs to the primary's representation. Once a strong
    // validator is known, its omission is not permission to splice bytes.
    const char *etag = header_value(request->curl, "ETag");
    bool same_entity = !p->etag || (etag && strcmp(etag, p->etag) == 0);
    // HTTP error documents may carry unrelated validators, or none. Reject
    // their bodies but retain the bounded retry path for transient errors.
    if ((resp == 200 || resp == 206) && (!same_entity || (resp == 200 && request->fetch_pos > 0)))
        request->changed = true;
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
    // Continuations must retain the primary's strong validator. Its omission
    // cannot authorize appending an unverified representation either.
    const char *etag = header_value(p->curl, "ETag");
    bool same_entity = !p->etag || (etag && strcmp(etag, p->etag) == 0);
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
    if (valid && resp == 206 && p->dynamic_supported && !p->dynamic_started) {
        // A seek request can be capped below its requested end. Schedule
        // later ranges only after learning the actual primary reply end;
        // using the planned end here would leave a permanent byte gap.
        p->primary_end = end;
        cmd_async(p, CMD_START_DYNAMIC);
    }
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
    case CURLE_HTTP3:
    case CURLE_QUIC_CONNECT_ERROR:
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
            if (!p->probed || p->seek_probe) {
                bytes = MPMIN(bytes, 256 << 10);
                p->seek_probe = false;
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
    apply_http_policy(p, p->curl);
    curl_easy_setopt(p->curl, CURLOPT_FRESH_CONNECT, (long)p->primary_ref.fresh_connection);
    p->primary_ref.fresh_connection = false;
    CURLMcode result = add_curl_handle(p->ctx, p->curl);
    if (result != CURLM_OK) {
        MP_ERR(p, "could not start curl request: %s\n",
               curl_multi_strerror(result));
        request_failed(p);
        return;
    }
    begin_measurement(&p->primary_ref);
    p->active = true;
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
    p->phase_wall_ns = now;
    p->phase_poll_ns = p->ctx->poll_ns;
    p->phase_ring_ns = p->ctx->ring_wait.active_ns;
    p->phase_hol_ns = p->hol_ns;
    p->phase_ordered_start = p->total_ordered;
    p->phase_consumer_start = p->delivered_bytes;
    p->phase_invalid = false;
    p->phase_http = 0;
}

static struct mp_curl_observation stream_observation(struct priv *p, int64_t now)
{
    struct curl_ctx *ctx = p->ctx;
    double wall = MPMAX(1e-6, MP_TIME_NS_TO_S(now - p->phase_wall_ns));
    double active = MPMAX(1e-6, MP_TIME_NS_TO_S(p->phase_active_ns));
    struct mp_curl_observation o = {
        .now_ns = now, .wall_ms = (int64_t)time(NULL) * 1000,
        .epoch = p->epoch, .profile_generation = p->generation,
        .requests = p->selected_requests, .max_requests = p->opts->parallel_requests,
        .fixed_requests = p->opts->fixed_requests,
        .capacity = (p->total_ordered - p->phase_ordered_start) / active,
        .demand_bps = ctx->media_bitrate, .buffer_seconds = ctx->buffer_seconds,
        .buffer_valid = ctx->state_updates && isfinite(ctx->playback_state.buffer_seconds) && ctx->playback_state.buffer_seconds >= 0,
        .paused = ctx->playback_paused, .seeking = ctx->playback_seeking,
        .underrun = ctx->playback_state.underrun, .eof = ctx->playback_state.eof,
        .foreign = foreign_traffic(ctx), .explore = p->opts->explore,
        .worker_cpu = ctx->worker_cpu, .worker_cpu_available = ctx->worker_cpu_available,
        .network_wait_available = p->phase_wall_ns && now > p->phase_wall_ns,
        .network_wait_ratio = MP_TIME_NS_TO_S(ctx->poll_ns - p->phase_poll_ns) / wall,
        .storage_ratio = MP_TIME_NS_TO_S(ctx->ring_wait.active_ns - p->phase_ring_ns) / wall,
        .hol_ratio = MP_TIME_NS_TO_S(p->hol_ns - p->phase_hol_ns) / wall,
        .blocked = p->phase_blocked || p->phase_invalid,
        .ordered_bytes = p->total_ordered, .next_offset = p->next_schedule,
        .playable_progress = ctx->buffer_seconds > 0,
        .rtt_ms = -1, .ttfb_ms = p->ttfb_ms,
        .resources_valid = !ctx->experiment_stream || ctx->experiment_stream == p->stream_id,
        .free_raw_bytes = mp_curl_budget_raw_available(&ctx->budget),
        .range_limit = p->opts->max_request_size,
        .batch_cap = ctx->budget.baseline <= (4 << 20) ? 8 << 20 : 32 << 20,
        .constrained = ctx->budget.baseline <= (4 << 20),
        .memory_pressure = mp_curl_budget_committed(&ctx->budget) > ctx->budget.desired_extra,
    };
    o.ordered_ratio = ctx->media_bitrate > 0 ? o.capacity * 8 / ctx->media_bitrate : 1;
    mp_mutex_lock(&p->mtx);
    o.free_descriptors = p->num_ranges - p->range_count;
    o.consumer_progress = p->delivered_bytes > p->phase_consumer_start;
    o.consumer_wanted = p->consumer_waiting || (!ctx->playback_state.idle && !ctx->playback_state.eof);
    o.prefix_incomplete = p->range_count && !p->ranges[p->range_head].complete;
    o.reorder_growing = stream_reorder(p) > 0;
    if (p->hol_started_ns)
        o.hol_ratio += MP_TIME_NS_TO_S(now - p->hol_started_ns) / wall;
    mp_mutex_unlock(&p->mtx);
    o.cache_target_reached = ctx->playback_state.max_fw_bytes > 0 &&
        ctx->playback_state.fw_bytes >= ctx->playback_state.max_fw_bytes * .9;
    int protocol = p->phase_http ? p->phase_http : p->evidence_http;
    if (!protocol)
        protocol = p->primary_ref.http_version;
    o.profile = (struct mp_curl_profile){
        .requests = p->selected_requests,
        .connections = protocol == 11 ? p->selected_requests : 1,
        .http_mode = p->http_mode, .http_version = protocol,
        .receive_profile = p->receive_mode == 2,
        .receive_bytes = p->receive_mode == 2 ? p->receive_bytes : 0,
        .range_bytes = p->segment_size, .udp_receive_bytes = p->udp_requested,
    };
#if HAVE_SOCKET_PROBES
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        const struct curl_socket_info *socket = &ctx->sockets[n];
        if (socket->fd != CURL_SOCKET_BAD && socket->tcp && socket->rtt_ms >= 0)
            o.rtt_ms = MPMAX(o.rtt_ms, socket->rtt_ms);
    }
#endif
    if (p->control_phase) {
        o.phase_start = p->control_start;
        o.phase_bytes = p->ordered_pos > p->control_start
            ? MPMIN(p->ordered_pos, p->control_end) - p->control_start : 0;
        o.phase_seconds = p->control_started_ns ? MP_TIME_NS_TO_S(now - p->control_started_ns) : 0;
        o.phase_short = p->control_end > dynamic_limit(p);
    }
    return o;
}

static void update_adaptation(struct priv *p)
{
    struct curl_ctx *ctx = p->ctx;
    int64_t now = mp_time_ns();
    learning_tick(p, now);
    if (!p->phase_wall_ns)
        reset_phase(p, now);
    struct mp_curl_observation o = stream_observation(p, now);
    mp_curl_controller_tick(&p->controller, &o);
    bool complete = p->control_phase && p->control_started_ns && p->ordered_pos >= p->control_end;
    bool ordinary = !p->control_phase && !p->startup && !p->transitioning &&
        p->phase_active_ns >= MP_TIME_S_TO_NS(1.5) && p->phase_completed >= p->selected_requests * 2;
    int selected = p->selected_requests;
    if (complete || ordinary) {
        if (ctx->stream_id == p->stream_id && !o.blocked && !o.foreign) {
            struct mp_curl_concurrency_metrics *point = &ctx->concurrency[selected - 1];
            point->capacity_bytes_per_second = o.capacity;
            point->ttfb_ms = p->ttfb_ms;
            point->samples++;
            ctx->capacity = o.capacity;
            ctx->capacity_samples = ++p->capacity_samples;
        }
        selected = mp_curl_controller_decide(&p->controller, &o);
    }
    if (p->controller.action == MP_CURL_CANCEL_SUFFIX)
        rescue_stalled_range(p, now);
    bool start = p->controller.action == MP_CURL_START_PHASE && p->controller.experiment &&
        (!p->control_phase || p->control_start != p->controller.phase_start);
    if (start) {
        if (ctx->experiment_stream && ctx->experiment_stream != p->stream_id) {
            p->controller.experiment = false;
            p->controller.action = MP_CURL_DEFER;
            p->controller.reason = MP_CURL_REASON_POOL;
        } else if (!p->control_phase && !mp_curl_budget_reserve_raw(&ctx->budget, p->controller.required_raw_bytes)) {
            p->controller.experiment = false;
            p->controller.action = MP_CURL_DEFER;
            p->controller.reason = MP_CURL_REASON_RAW_MEMORY;
        } else {
            if (!p->control_phase)
                p->phase_reservation = p->controller.required_raw_bytes;
            ctx->experiment_stream = p->stream_id;
            p->control_phase = true;
            p->control_start = p->controller.phase_start;
            p->control_end = p->controller.phase_end;
            p->control_started_ns = 0;
            p->segment_size = p->controller.trial_range_bytes;
            selected = p->controller.target_requests;
            p->generation++;
            p->transitioning = p->learning_active;
            reset_phase(p, now);
        }
    }
    if (p->controller.action == MP_CURL_RESTORE ||
        (p->control_phase && !p->controller.experiment)) {
        selected = p->controller.target_requests ? p->controller.target_requests : selected;
        p->control_phase = false;
        if (p->phase_reservation) {
            mp_curl_budget_release_raw(&ctx->budget, p->phase_reservation);
            p->phase_reservation = 0;
        }
        if (ctx->experiment_stream == p->stream_id)
            ctx->experiment_stream = 0;
    }
    p->urgency = p->controller.urgency;
    if (!p->http_limit)
        p->last_reason = p->controller.reason;
    if (selected != p->selected_requests) {
        MP_INFO(p, "streaming requests: %d -> %d (reason %d)\n",
                p->selected_requests, selected, p->last_reason);
        p->selected_requests = selected;
        if (!start) {
            p->generation++;
            p->transitioning = p->learning_active;
            reset_phase(p, now);
        }
    }
    if (complete || ordinary)
        reset_phase(p, now);
    if (ctx->stream_id == p->stream_id)
        ctx->selected_requests = p->selected_requests;
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
    request->length = range->end - range->start;
    request->response_ok = false;
    request->changed = false;
    range->request = request;
    if (p->control_phase && !p->control_started_ns && range->start >= p->control_start)
        p->control_started_ns = mp_time_ns();

    char text[64];
    snprintf(text, sizeof(text), "%" PRIu64 "-%" PRIu64,
             range->start + from, range->end - 1);
    curl_easy_setopt(request->curl, CURLOPT_RANGE, text);
    apply_http_policy(p, request->curl);
    curl_easy_setopt(request->curl, CURLOPT_FRESH_CONNECT, (long)request->ref.fresh_connection);
    request->ref.fresh_connection = false;
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

// Rescue only the required prefix, once per range. Existing validator and
// prefix verification rules still govern the suffix; no duplicate is published.
static void rescue_stalled_range(struct priv *p, int64_t now)
{
    if (p->ctx->playback_paused || !p->dynamic_started || p->selected_requests < 2)
        return;
    mp_mutex_lock(&p->mtx);
    struct range_desc *head = p->range_count ? &p->ranges[p->range_head] : NULL;
    bool needed = head && !head->complete && !head->rescued && p->queue_open;
    mp_mutex_unlock(&p->mtx);
    if (!needed || !head->request || !head->request->active)
        return;
    struct dynamic_request *request = head->request;
    double rates[MP_CURL_MAX_LANES];
    int count = 0;
    for (int n = 0; n < p->opts->parallel_requests; n++) {
        double rate = p->ctx->lane_capacity[n];
        if (n != request->ref.lane && rate > 0) {
            int at = count++;
            while (at > 0 && rates[at - 1] > rate) {
                rates[at] = rates[at - 1];
                at--;
            }
            rates[at] = rate;
        }
    }
    if (!count)
        return;
    double rtt = .05;
#if HAVE_SOCKET_PROBES
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        const struct curl_socket_info *socket = &p->ctx->sockets[n];
        if (socket->connection_id == request->ref.connection_id && socket->rtt_ms >= 0)
            rtt = socket->rtt_ms / 1000;
    }
#endif
    double remaining = head->end - head->start - head->received;
    double replacement = 2 * rtt + p->ttfb_ms / 1000 + remaining / rates[count / 2];
    double quiet = MP_TIME_NS_TO_S(now - request->ref.last_progress_ns);
    if (quiet < MPMAX(1, replacement))
        return;
    head->rescued = true;
    remove_curl_handle(p->ctx, request->curl);
    end_measurement(&request->ref, false);
    request->active = false;
    request->ref.fresh_connection = true;
    p->last_reason = 3;
    if (request->ref.http_version >= 20)
        downgrade_http(p, &request->ref, CURLE_OPERATION_TIMEDOUT);
    atomic_fetch_add_explicit(&p->ctx->retry_attempts, 1, memory_order_relaxed);
    start_range_transfer(request, head);
}

// Queue the next range at next_schedule, or return NULL when every queue
// entry still holds unread bytes.
static struct range_desc *queue_next_range(struct priv *p)
{
    uint64_t limit = dynamic_limit(p);
    if (p->control_phase)
        limit = MPMIN(limit, p->control_end);
    if (p->next_schedule >= limit)
        return NULL;

    // Full preallocated segments amortize range/header/TLS bookkeeping.
    // Shrinking ranges as throughput falls compounds slow-client overhead
    // and changes the workload in the middle of concurrency comparisons.
    uint64_t bytes = p->segment_size;

    mp_mutex_lock(&p->mtx);
    struct range_desc *range = NULL;
    if (p->range_count < p->num_ranges) {
        range = &p->ranges[(p->range_head + p->range_count) % p->num_ranges];
        uint64_t needed = MPMIN(bytes, limit - p->next_schedule);
        if (range->allocation < needed) {
            if (range->allocation) {
                mp_curl_budget_release_raw(&p->ctx->budget, range->allocation);
                p->queue_allocation -= range->allocation;
                talloc_free(range->data);
                range->data = NULL;
                range->allocation = 0;
            }
            uint64_t available = mp_curl_budget_raw_available(&p->ctx->budget);
            uint64_t sibling_reserve = MPMIN(1 << 20, p->ctx->budget.baseline / 4);
            // Demanded leading bytes may use the reserve; speculative refill
            // always leaves it for a newly blocked sibling transport.
            if (p->range_count > 0)
                available = available > sibling_reserve ? available - sibling_reserve : 0;
            if (p->control_phase)
                available += p->phase_reservation;
            uint64_t desired = needed;
            needed = MPMIN(needed, available);
            if (p->control_phase && needed < desired) {
                p->phase_invalid = true;
                mp_mutex_unlock(&p->mtx);
                return NULL;
            }
            uint64_t reserved = MPMIN(needed, p->phase_reservation);
            if (!needed || !mp_curl_budget_reserve_raw(&p->ctx->budget, needed - reserved)) {
                p->last_reason = 26;
                mp_mutex_unlock(&p->mtx);
                return NULL;
            }
            range->data = talloc_size(NULL, needed);
            if (!range->data) {
                mp_curl_budget_release_raw(&p->ctx->budget, needed - reserved);
                p->last_reason = 9;
                p->phase_invalid = true;
                mp_mutex_unlock(&p->mtx);
                return NULL;
            }
            p->phase_reservation -= reserved;
            range->allocation = needed;
            p->queue_allocation += needed;
        }
        p->range_count++;
        range->start = p->next_schedule;
        range->end = p->next_schedule + MPMIN(range->allocation, MPMIN(bytes, limit - p->next_schedule));
        range->published = 0;
        range->consumed = 0;
        range->retained = 0;
        range->complete = false;
    }
    mp_mutex_unlock(&p->mtx);
    if (!range)
        return NULL;
    range->received = 0;
    range->request = NULL;
    range->retry_count = 0;
    range->rescued = false;
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
    if ((int64_t)p->ctx->budget.desired_extra != p->grant_seen)
        p->pool_transition = true;
    if (p->pool_transition) {
        bool active = p->active;
        for (int n = 0; n < p->num_requests; n++)
            active |= p->requests[n].active;
        if (active)
            return;
        p->pool_transition = false;
        p->grant_seen = p->ctx->budget.desired_extra;
        p->udp_limited = false;
        select_stream_pool(p);
        memset(&p->controller, 0, sizeof(p->controller));
        reset_phase(p, mp_time_ns());
    }
    if (p->startup) {
        int64_t now = mp_time_ns();
        int desired = p->opts->fixed_requests;
        if (!desired) {
            desired = mp_curl_controller_best(&p->controller, p->opts->parallel_requests);
            // TTFB controls pacing only. Protocol/window decisions use TCP RTT.
            double rtt_ms = MPMAX(50, p->ttfb_ms);
            int64_t age = now - p->startup_ns;
            int ladder = age >= MP_TIME_MS_TO_NS(2 * rtt_ms) ? 4
                : age >= MP_TIME_MS_TO_NS(rtt_ms) ? 2 : 1;
            desired = MPMAX(desired, ladder);
        }
        p->selected_requests = MPMIN(desired, p->opts->parallel_requests);
        p->urgency = 1;
        if (p->ctx->stream_id == p->stream_id)
            p->ctx->selected_requests = p->selected_requests;
        if (p->ctx->buffer_seconds >= .5 || now - p->startup_ns >= MP_TIME_S_TO_NS(1)) {
            p->startup = false;
            reset_phase(p, now);
        }
    }
    {
        mp_mutex_lock(&p->mtx);
        for (int n = p->range_count; n < p->num_ranges; n++) {
            struct range_desc *range = &p->ranges[(p->range_head + n) % p->num_ranges];
            if (range->allocation) {
                mp_curl_budget_release_raw(&p->ctx->budget, range->allocation);
                p->queue_allocation -= range->allocation;
                talloc_free(range->data);
                range->data = NULL;
                range->allocation = 0;
            }
        }
        mp_mutex_unlock(&p->mtx);
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

static size_t discard_warm_bytes(char *data, size_t size, size_t count, void *opaque)
{
    return size * count;
}

static void clear_warm_requests(struct priv *p)
{
    struct warm_request **link = &p->ctx->warm;
    while (*link) {
        struct warm_request *warm = *link;
        if (warm->ref.p != p) {
            link = &warm->next;
            continue;
        }
        *link = warm->next;
        remove_curl_handle(p->ctx, warm->ref.handle);
        end_measurement(&warm->ref, false);
        curl_easy_cleanup(warm->ref.handle);
        curl_slist_free_all(warm->headers);
        talloc_free(warm);
    }
}

static bool retain_warm_request(struct dynamic_request *request)
{
    struct priv *p = request->ref.p;
    if (!p->keep_warm || !request->active || request->ref.http_version != 11 ||
        !request->range || request->ref.blocked || p->ctx->playback_paused)
        return false;
    double rtt = -1;
#if HAVE_SOCKET_PROBES
    for (int n = 0; n < MP_CURL_MAX_SOCKETS; n++) {
        const struct curl_socket_info *socket = &p->ctx->sockets[n];
        if (socket->connection_id == request->ref.connection_id)
            rtt = socket->rtt_ms / 1000;
    }
#endif
    double capacity = p->ctx->lane_capacity[request->ref.lane];
    size_t remaining = request->length - request->fetch_pos;
    if (rtt <= 0 || capacity <= 0 || remaining > capacity * rtt)
        return false;
    struct warm_request *warm = talloc_zero(p->ctx, struct warm_request);
    if (!warm)
        return false;
    for (struct curl_slist *header = p->range_headers; header; header = header->next) {
        struct curl_slist *next = curl_slist_append(warm->headers, header->data);
        if (!next) {
            curl_slist_free_all(warm->headers);
            talloc_free(warm);
            return false;
        }
        warm->headers = next;
    }
    warm->ref = request->ref;
    warm->ref.kind = CURL_HANDLE_WARM;
    warm->ref.blocked = true;
    end_measurement(&request->ref, false);
    warm->ref.measuring = false;
    struct curl_handle_ref **link = &p->ctx->handles;
    while (*link && *link != &request->ref)
        link = &(*link)->next;
    mp_assert(*link);
    *link = &warm->ref;
    warm->next = p->ctx->warm;
    p->ctx->warm = warm;
    curl_easy_setopt(request->curl, CURLOPT_PRIVATE, &warm->ref);
    curl_easy_setopt(request->curl, CURLOPT_HTTPHEADER, warm->headers);
    curl_easy_setopt(request->curl, CURLOPT_WRITEFUNCTION, discard_warm_bytes);
    curl_easy_setopt(request->curl, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(request->curl, CURLOPT_HEADERFUNCTION, discard_warm_bytes);
    curl_easy_setopt(request->curl, CURLOPT_HEADERDATA, NULL);
    curl_easy_setopt(request->curl, CURLOPT_NOPROGRESS, 1L);
    request->active = false;
    request->curl = NULL;
    return true;
}

static void stop_dynamic(struct priv *p)
{
    end_ring_wait(p);
    if (p->phase_reservation) {
        mp_curl_budget_release_raw(&p->ctx->budget, p->phase_reservation);
        p->phase_reservation = 0;
    }
    p->control_phase = false;
    if (p->ctx->experiment_stream == p->stream_id)
        p->ctx->experiment_stream = 0;
    // The consumer copies queue bytes without the lock; wait before the
    // storage goes away. Seek and close come from the consumer itself, but
    // a fallback is raised asynchronously on this thread.
    mp_mutex_lock(&p->mtx);
    while (p->copying) {
        p->copy_waiter = true;
        mp_cond_wait(&p->cond, &p->mtx);
    }
    p->copy_waiter = false;
    p->ctx->reorder_bytes -= stream_reorder(p);
    p->queue_open = false;
    p->range_head = p->range_count = 0;
    p->queued_bytes = 0;
    update_gap(p, 0);
    mp_mutex_unlock(&p->mtx);
    for (int n = 0; n < p->num_requests; n++) {
        struct dynamic_request *request = &p->requests[n];
        if (request->active && !retain_warm_request(request)) {
            remove_curl_handle(p->ctx, request->curl);
            end_measurement(&request->ref, false);
            request->active = false;
        }
        if (request->curl)
            curl_easy_cleanup(request->curl);
    }
    curl_slist_free_all(p->range_headers);
    p->range_headers = NULL;
    for (int n = 0; n < p->num_ranges; n++)
        talloc_free(p->ranges[n].data);
    talloc_free(p->ranges);
    talloc_free(p->requests);
    if (p->queue_allocation) {
        mp_curl_budget_release_raw(&p->ctx->budget, p->queue_allocation);
        p->queue_allocation = 0;
    }
    p->ranges = NULL;
    p->requests = NULL;
    p->num_ranges = 0;
    p->num_requests = 0;
    p->dynamic_started = false;
}

static bool prepare_range_headers(struct priv *p)
{
    if (!p->etag)
        return true;
    struct curl_slist *headers = NULL;
    for (struct curl_slist *h = p->headers; h; h = h->next) {
        // The transport owns the validator for these exact byte ranges.
        if (strncasecmp(h->data, "If-Range:", 9) == 0)
            continue;
        struct curl_slist *next = curl_slist_append(headers, h->data);
        if (!next)
            goto fail;
        headers = next;
    }
    char *condition = talloc_asprintf(NULL, "If-Range: %s", p->etag);
    if (!condition)
        goto fail;
    struct curl_slist *next = curl_slist_append(headers, condition);
    talloc_free(condition);
    if (!next)
        goto fail;
    p->range_headers = next;
    return true;

fail:
    curl_slist_free_all(headers);
    return false;
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
    if (mp_curl_budget_raw_available(&p->ctx->budget) < CURL_MAX_WRITE_SIZE) {
        p->last_reason = 26;
        p->dynamic_supported = false;
        return;
    }
    int ranges = MP_CURL_MAX_RANGES;
    uint64_t limit = dynamic_limit(p);
    p->primary_end = MPMIN(p->primary_end, limit);
    p->next_schedule = p->primary_end;
    p->num_requests = count;
    p->num_ranges = ranges;
    // Descriptor slots and active request slots are independent. Storage is
    // admitted per range, so the configured maximum never multiplies memory.
    p->requests = talloc_zero_array(NULL, struct dynamic_request, count);
    p->ranges = talloc_zero_array(NULL, struct range_desc, ranges);
    if (!p->requests || !p->ranges) {
        talloc_free(p->ranges);
        talloc_free(p->requests);
        p->ranges = NULL;
        p->requests = NULL;
        p->num_requests = p->num_ranges = 0;
        p->dynamic_supported = false;
        return;
    }
    mp_mutex_lock(&p->mtx);
    p->range_head = p->range_count = 0;
    p->ordered_pos = p->queue_pos = p->primary_end;
    p->queued_bytes = 0;
    p->queue_open = false;
    mp_mutex_unlock(&p->mtx);
    p->dynamic_started = true;
    if (!prepare_range_headers(p)) {
        MP_WARN(p, "could not allocate parallel range headers\n");
        p->dynamic_supported = false;
        stop_dynamic(p);
        return;
    }

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
        if (p->range_headers)
            curl_easy_setopt(request->curl, CURLOPT_HTTPHEADER, p->range_headers);
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
    // A strong-validator range is published, readable and recyclable once
    // its last byte arrives. How its connection then ends cannot change
    // those bytes, and must neither retry nor touch a recycled descriptor.
    if (range->request != request || range->complete) {
        if (range->request == request)
            range->request = NULL;
        if (!aborted) {
            fill_dynamic(p);
            check_dynamic_eof(p);
        }
        return;
    }
    // Every byte of the validated range arrived in one response, whatever
    // the result code: a lost terminator, a reset, or our refusal of bytes
    // past the range is not a reason to fetch it again.
    bool whole = request->response_ok && request->fetch_pos == request->length;
    if (whole && !aborted) {
        publish_range(p, range, mp_time_ns(), true);
        range->request = NULL;
        fill_dynamic(p);
        check_dynamic_eof(p);
        return;
    }
    range->request = NULL;

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
    bool downgraded = !aborted && downgrade_http(p, &request->ref, code);
    bool recoverable = !aborted && (downgraded ||
        ((response_rejected || is_recoverable_error(code)) &&
         range->retry_count < p->opts->max_retries));
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

    if (!p->probed && !aborted && downgrade_http(p, &p->primary_ref, code)) {
        start_request(p);
        return;
    }
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
    if (received)
        p->retry_count = 0;
    p->request_start += received;
    p->request_received = 0;
    // A reply that delivered its whole range is complete, even if its
    // connection then failed or it overran the range: retrying from its end
    // would fetch bytes the parallel queue also delivers.
    bool whole = p->response_ok && p->primary_end && p->request_start == p->primary_end;

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

    if ((code == CURLE_OK || whole) && p->response_ok && !aborted) {
        p->retry_count = 0;
        if (p->dynamic_supported) {
            p->primary_done = true;
            if (!p->dynamic_started)
                start_dynamic(p);
            if (p->dynamic_started) {
                if (p->request_start != p->primary_end) {
                    // A valid capped suffix may need several primary replies
                    // before reaching the already scheduled queue boundary.
                    if (received && p->request_start < p->primary_end) {
                        start_request(p);
                    } else {
                        cmd_async(p, CMD_FALLBACK_DYNAMIC);
                    }
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
    bool downgraded = !aborted && downgrade_http(p, &p->primary_ref, code);
    bool recoverable = !aborted && p->seekable && (downgraded ||
        ((response_rejected || is_recoverable_error(code)) &&
         p->retry_count < p->opts->max_retries));
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
    apply_http_policy(p, c);
    curl_easy_setopt(c, CURLOPT_PIPEWAIT, 1L);
    // Alt-Svc is evidence, never permission for curl to silently upgrade.
    curl_easy_setopt(c, CURLOPT_ALTSVC_CTRL, 0L);
    curl_easy_setopt(c, CURLOPT_HSTS_CTRL, (long)CURLHSTS_ENABLE);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    if (p->ctx->hw_aes == 0)
        curl_easy_setopt(c, CURLOPT_TLS13_CIPHERS,
            "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384");
    // Larger reads may reduce receive calls. Curl still splits body output
    // at CURL_MAX_WRITE_SIZE; this does not enlarge write callbacks.
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
    curl_easy_setopt(c, CURLOPT_OPENSOCKETDATA, p);
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, socket_options_cb);
    curl_easy_setopt(c, CURLOPT_SOCKOPTDATA, p);
    curl_easy_setopt(c, CURLOPT_CLOSESOCKETFUNCTION, close_socket_cb);
    curl_easy_setopt(c, CURLOPT_CLOSESOCKETDATA, p->ctx);
#endif
}

// stream_curl implementation

static void sample_consumer_cpu(struct priv *p)
{
    int64_t now = mp_time_ns();
    if (now < p->next_consumer_cpu_ns)
        return;
    p->next_consumer_cpu_ns = now + RATE_BUCKET_NS;
    struct curl_ctx *ctx = p->ctx;
    if (!consumer_thread_generation)
        consumer_thread_generation = atomic_fetch_add_explicit(&next_consumer_generation, 1, memory_order_relaxed);
    mp_mutex_lock(&ctx->state_lock);
    struct curl_consumer_cpu *sample = p->consumer_cpu_slot ? &ctx->consumer_cpu[p->consumer_cpu_slot - 1] : NULL;
    if (sample && sample->generation != consumer_thread_generation) {
        release_consumer_cpu(p);
        sample = NULL;
    }
    if (!sample) {
        struct curl_consumer_cpu *free_slot = NULL;
        for (int n = 0; n < MP_CURL_MAX_CONSUMERS; n++) {
            struct curl_consumer_cpu *slot = &ctx->consumer_cpu[n];
            if (slot->users && slot->generation == consumer_thread_generation) {
                sample = slot;
                break;
            }
            if (!slot->users && !free_slot)
                free_slot = slot;
        }
        if (!sample && free_slot) {
            sample = free_slot;
            *sample = (struct curl_consumer_cpu) { .generation = consumer_thread_generation, .last_cpu_ns = -1 };
        }
        if (sample) {
            sample->users++;
            p->consumer_cpu_slot = sample - ctx->consumer_cpu + 1;
        }
    }
    if (!sample) {
        ctx->consumer_snapshot_truncated = true;
        ctx->consumer_cpu_incomplete = true;
    } else if (now >= sample->next_sample_ns) {
        int64_t cpu = mp_thread_cpu_time_ns(mp_thread_current_id());
        sample->valid = cpu >= 0;
        ctx->consumer_cpu_seen = true;
        ctx->consumer_cpu_incomplete |= !sample->valid;
        if (sample->valid && sample->last_cpu_ns >= 0 && cpu >= sample->last_cpu_ns)
            sample->total_ns += cpu - sample->last_cpu_ns;
        sample->last_cpu_ns = cpu;
        sample->next_sample_ns = now + RATE_BUCKET_NS;
    }
    mp_mutex_unlock(&ctx->state_lock);
}

static int curl_fill_buffer(struct stream *s, void *buffer, int max_len)
{
    struct priv *p = s->priv;
    if (max_len <= 0)
        return 0;
    sample_consumer_cpu(p);

    mp_mutex_lock(&p->mtx);

    // The ring holds bytes before the queue, so it is always drained first.
    while (p->count == 0 && !(p->queue_open && p->ordered_pos > p->queue_pos) &&
           !p->stream_eof && !p->stream_error &&
           !atomic_load_explicit(&p->aborted, memory_order_relaxed))
    {
        int64_t started = mp_time_ns();
        p->consumer_wait_started_ns = started;
        p->consumer_waiting = true;
        update_gap(p, started);
        mp_cond_wait(&p->cond, &p->mtx);
        int64_t now = mp_time_ns();
        p->consumer_wait_ns += now - started;
        p->consumer_wait_started_ns = 0;
        p->consumer_waiting = false;
        update_gap(p, now);
        sample_consumer_cpu(p);
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
            p->coalesced_wakeups += !service;
            p->service_pending = true;
        }
    }

    bool unpause = p->paused && !p->stream_eof && !p->stream_error &&
                   p->buffer_size - p->count >= p->buffer_size / 2;
    if (unpause) {
        p->coalesced_wakeups += p->unpause_pending;
        unpause = !p->unpause_pending;
        p->unpause_pending = true;
    }
    p->delivered_bytes += copy;
    update_gap(p, 0);

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
    p->receive_mode = opts->receive_mode;
    p->receive_bytes = opts->receive_bytes;
    p->http_mode = opts->http_mode;
    p->net_opts = mp_get_config_group(p, s->global, &mp_network_conf);
    p->url = talloc_strdup(p, s->url);
    p->scheme = curl_scheme_lookup(bstr0(p->url));
    canonical_origin(p->url, p->origin, sizeof(p->origin));
    // Only supported URLs are supposed to reach here.
    mp_assert(p->scheme);
    p->content_size = -1;
    p->buffer_size = p->opts->buffer_size;
    p->selected_requests = p->opts->fixed_requests
        ? MPMIN(p->opts->fixed_requests, p->opts->parallel_requests) : 1;
    p->startup = !p->opts->fixed_requests && p->opts->max_request_size > 0;
    // Speculative memory is one ring, not max-request-size times the cap.
    p->segment_size = MPMIN(p->opts->max_request_size,
                           p->buffer_size / p->opts->parallel_requests);
    // The worker reserves and allocates the ring in the core-wide ledger.
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
