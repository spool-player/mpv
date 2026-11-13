#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "stream/curl_policy.h"

#define MIB UINT64_C(1048576)
#define SECOND INT64_C(1000000000)

static struct mp_curl_h3_input remote(void)
{
    return (struct mp_curl_h3_input) {
        .now_ns = SECOND,
        .https = true,
        .http3_supported = true,
        .capability_evidence = true,
        .rtt_available = true,
        .rtt_ms = 150,
        .counts_settled = true,
        .ranges_settled = true,
        .tcp_capacity_bytes_per_second = 10000000,
        .demand_bps = 80000000,
        .worker_cpu_available = true,
        .tcp_cpu_available = true,
        .worker_cpu = .1,
        .tcp_cpu_ns_per_byte = 2,
        .hw_aes = true,
        .quic_batching = true,
        .udp_buffer_available = true,
        .udp_effective_bytes = 2 * MIB,
    };
}

static void h3_eligibility(void)
{
    struct mp_curl_h3_input i = remote();
    struct mp_curl_policy_decision decision = mp_curl_h3_eligible(&i);
    assert(decision.eligible && decision.projected_cpu > .05 && decision.projected_cpu < .1);
    i.rtt_ms = 1;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_H3_PATH);
    i.rtt_ms = 49.99;
    assert(!mp_curl_h3_eligible(&i).eligible);
    i.rtt_ms = 50;
    assert(mp_curl_h3_eligible(&i).eligible);
    i.counts_settled = false;
    assert(!mp_curl_h3_eligible(&i).eligible);
    i.out_of_order_growth = true;
    assert(mp_curl_h3_eligible(&i).eligible);
    i = remote();
    i.tcp_cpu_ns_per_byte = 20;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_H3_CPU);
    i = remote();
    i.worker_cpu = .5;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_H3_CPU);
    i = remote();
    i.worker_cpu_available = i.tcp_cpu_available = false;
    i.constrained = true;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_H3_CPU);
    i.constrained = false;
    assert(mp_curl_h3_eligible(&i).eligible);
    i = remote();
    i.tcp_cpu_ns_per_byte = NAN;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_H3_CPU);
    i = remote();
    i.hw_aes = false;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_AES);
    i = remote();
    i.udp_effective_bytes = 400000; // 25 ms requires 437500 bytes on this path.
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_H3_BUFFER);
    i.udp_effective_bytes = 437500;
    assert(mp_curl_h3_eligible(&i).eligible);
    i.quic_batching = false;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_BATCHING);
    i = remote();
    i.cooldown_until_ns = 600 * SECOND;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_H3_FAILED);
    i.now_ns = i.cooldown_until_ns;
    assert(mp_curl_h3_eligible(&i).eligible);
    // Explicit preference skips path/CPU/crypto, but never the receive budget,
    // verified batching, unsupported build or failed-origin cooldown.
    i = remote();
    i.prefer_h3 = true;
    i.rtt_ms = 1;
    i.hw_aes = false;
    i.worker_cpu = 1;
    i.capability_evidence = false;
    assert(mp_curl_h3_eligible(&i).eligible);
    i.udp_buffer_available = false;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_H3_BUFFER);
    i.udp_buffer_available = true;
    i.http3_supported = false;
    assert(mp_curl_h3_eligible(&i).reason == MP_CURL_REASON_UNSUPPORTED);
    assert(mp_curl_udp_receive_bytes(0, 0) == 256 * 1024);
    assert(mp_curl_udp_receive_bytes(80000000, 10) == MIB);
    assert(mp_curl_udp_receive_bytes(80000000, 100) == 4 * MIB);
    assert(mp_curl_udp_receive_bytes(1000000000, 1000) == 8 * MIB);
}

static struct mp_curl_window_input window(void)
{
    return (struct mp_curl_window_input) {
        .file_average_bps = 75000000,
        .playback_speed = 1,
        .rtt_ms = 300,
        .rtt_available = true,
        .consumer_draining = true,
        .receive_window_available = true,
        .receive_window_bytes = MIB,
        .manual_ceiling_available = true,
        .manual_ceiling_bytes = 32 * MIB,
        .autotune_ceiling_available = true,
        .autotune_ceiling_bytes = 16 * MIB,
        .range_bytes = MIB,
        .physical_connections = 1,
        .socket_budget_available = true,
        .available_socket_bytes = 64 * MIB,
    };
}

static void receive_window(void)
{
    struct mp_curl_window_input i = window();
    struct mp_curl_policy_decision decision = mp_curl_window_candidate(&i);
    assert(decision.eligible && decision.requested_bytes == 9 * MIB);
    assert(decision.reserved_bytes == 18 * MIB && !decision.policy_limited);
    i.file_average_bps = 150000000;
    decision = mp_curl_window_candidate(&i);
    assert(decision.eligible && decision.requested_bytes == 16 * MIB && decision.policy_limited);
    i = window();
    i.physical_connections = 4;
    decision = mp_curl_window_candidate(&i);
    assert(decision.requested_bytes == 3 * MIB && decision.reserved_bytes == 24 * MIB);
    i = window();
    i.manual_ceiling_bytes = 208 * 1024;
    assert(mp_curl_window_candidate(&i).reason == MP_CURL_REASON_RECEIVE_CLAMP);
    i = window();
    i.manual_ceiling_available = false;
    assert(mp_curl_window_candidate(&i).reason == MP_CURL_REASON_UNSUPPORTED);
    i = window();
    i.range_bytes = 0;
    assert(mp_curl_window_candidate(&i).reason == MP_CURL_REASON_AUTOTUNE);
    i.range_bytes = 64 * MIB;
    assert(mp_curl_window_candidate(&i).reason == MP_CURL_REASON_AUTOTUNE);
    i.slow_connection_ramp = true;
    assert(mp_curl_window_candidate(&i).eligible);
    i = window();
    i.available_socket_bytes = 17 * MIB;
    assert(mp_curl_window_candidate(&i).reason == MP_CURL_REASON_SOCKET_BUDGET);
    i = window();
    i.receive_window_bytes = 32 * MIB;
    assert(mp_curl_window_candidate(&i).reason == MP_CURL_REASON_AUTOTUNE);
    i.receive_ceiling_suspected = true;
    assert(mp_curl_window_candidate(&i).eligible);
    i = window();
    i.windows = true;
    assert(!mp_curl_window_candidate(&i).eligible);
    i.slow_connection_ramp = true;
    assert(mp_curl_window_candidate(&i).eligible);
    i.rtt_ms = 99;
    assert(!mp_curl_window_candidate(&i).eligible);
    i = window();
    i.producer_backpressure = .05;
    assert(!mp_curl_window_candidate(&i).eligible);
    i = window();
    i.file_average_bps = 0;
    i.observed_peak_bps = 150000000;
    assert(!mp_curl_window_candidate(&i).eligible);
    i.observed_steady_bps = 75000000;
    assert(mp_curl_window_candidate(&i).eligible);
}

static void h3_adoption(void)
{
    struct mp_curl_h3_watch watch = { 0 };
    struct mp_curl_h3_window w = {
        .end_ns = SECOND, .worker_cpu = .4, .worker_cpu_available = true, .udp_drops_available = true, .udp_drops = 10
    };
    assert(mp_curl_h3_revert(&watch, &w) == MP_CURL_REASON_NONE);
    w.end_ns += SECOND / 4;
    assert(mp_curl_h3_revert(&watch, &w) == MP_CURL_REASON_NONE);
    w.end_ns = 2 * SECOND;
    assert(mp_curl_h3_revert(&watch, &w) == MP_CURL_REASON_H3_CPU);
    watch = (struct mp_curl_h3_watch) { 0 };
    w.end_ns = SECOND;
    w.worker_cpu = .2;
    assert(mp_curl_h3_revert(&watch, &w) == MP_CURL_REASON_NONE);
    w.end_ns += SECOND / 4;
    w.udp_drops++;
    assert(mp_curl_h3_revert(&watch, &w) == MP_CURL_REASON_H3_DROPS);
    struct mp_curl_h3_comparison c = { .tcp_before_bytes_per_second = 10000000,
        .tcp_after_bytes_per_second = 11000000,
        .h3_bytes_per_second = 14000000,
        .tcp_cpu_ns_per_byte = 1,
        .h3_cpu_ns_per_byte = 1.7,
        .demand_bps = 80000000,
        .cpu_available = true,
        .udp_drops_available = true };
    int reason;
    assert(mp_curl_h3_comparison_accepted(&c, &reason));
    c.udp_drops_after = 1;
    assert(!mp_curl_h3_comparison_accepted(&c, &reason) && reason == MP_CURL_REASON_H3_DROPS);
    c.udp_drops_after = 0;
    c.h3_cpu_ns_per_byte = 40;
    assert(!mp_curl_h3_comparison_accepted(&c, &reason) && reason == MP_CURL_REASON_H3_CPU);
    c.h3_cpu_ns_per_byte = 3;
    assert(mp_curl_h3_comparison_accepted(&c, &reason)); // absolute CPU remains below 30%.
    c.cache_pause = true;
    assert(!mp_curl_h3_comparison_accepted(&c, &reason) && reason == MP_CURL_REASON_DEADLINE);
    c.cache_pause = false;
    c.tcp_after_bytes_per_second = 15000000;
    assert(!mp_curl_h3_comparison_accepted(&c, &reason) && reason == MP_CURL_REASON_INCONCLUSIVE);
}

int main(void)
{
    h3_eligibility();
    receive_window();
    h3_adoption();
    puts("curl HTTP/3 and receive-window policy passed");
    return 0;
}
