#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "curl_controller.h"

// All rates below are bits/s except explicitly named byte rates. CPU is a
// fraction of one worker core. No function opens sockets or changes policy.
struct mp_curl_policy_decision {
    bool eligible, policy_limited;
    int reason;
    uint64_t requested_bytes, reserved_bytes;
    double projected_cpu;
};
struct mp_curl_h3_input {
    int64_t now_ns, cooldown_until_ns;
    int cooldown_reason;
    bool prefer_h3, https, http3_supported, capability_evidence;
    bool rtt_available, out_of_order_growth, h2_head_of_line;
    bool counts_settled, ranges_settled;
    double rtt_ms, tcp_capacity_bytes_per_second, demand_bps;
    bool worker_cpu_available, tcp_cpu_available, constrained;
    double worker_cpu, tcp_cpu_ns_per_byte, cpu_penalty;
    bool hw_aes, quic_batching, udp_buffer_available;
    uint64_t udp_effective_bytes;
    double scheduling_lateness_p99_ms;
};
struct mp_curl_window_input {
    double file_average_bps, delivery_average_bps, observed_peak_bps, observed_steady_bps;
    double playback_speed, rtt_ms, producer_backpressure;
    bool rtt_available, consumer_draining, receive_ceiling_suspected;
    bool receive_window_available, manual_ceiling_available, autotune_ceiling_available;
    bool windows, slow_connection_ramp, socket_budget_available;
    uint64_t receive_window_bytes, manual_ceiling_bytes, autotune_ceiling_bytes;
    uint64_t range_bytes, available_socket_bytes;
    int physical_connections;
};
struct mp_curl_h3_watch {
    int64_t last_cpu_window_ns, last_observed_ns;
    uint64_t udp_drops;
    unsigned cpu_over_budget_windows;
    bool udp_drops_seen;
};
struct mp_curl_h3_window {
    int64_t end_ns;
    double worker_cpu;
    uint64_t udp_drops;
    bool worker_cpu_available, udp_drops_available;
};
struct mp_curl_h3_comparison {
    double tcp_before_bytes_per_second, tcp_after_bytes_per_second;
    double h3_bytes_per_second, tcp_cpu_ns_per_byte, h3_cpu_ns_per_byte, demand_bps;
    bool cpu_available, udp_drops_available, cache_pause;
    uint64_t udp_drops_before, udp_drops_after;
};

uint64_t mp_curl_udp_receive_bytes(double demand_bps, double scheduling_lateness_p99_ms);
struct mp_curl_policy_decision mp_curl_h3_eligible(const struct mp_curl_h3_input *input);
struct mp_curl_policy_decision mp_curl_window_candidate(const struct mp_curl_window_input *input);
// Return a numeric revert reason, or NONE. Only independent completed 1-second
// CPU windows count; loss counters can trigger a revert immediately.
int mp_curl_h3_revert(struct mp_curl_h3_watch *watch, const struct mp_curl_h3_window *window);
bool mp_curl_h3_comparison_accepted(const struct mp_curl_h3_comparison *comparison, int *reason);
