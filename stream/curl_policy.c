#include <math.h>

#include "curl_policy.h"

#define MIB UINT64_C(1048576)
#define SECOND INT64_C(1000000000)

static bool positive(double value)
{
    return isfinite(value) && value > 0;
}

static uint64_t rounded_mib(double bytes, uint64_t minimum, uint64_t maximum)
{
    if (!positive(bytes))
        return minimum;
    if (bytes >= maximum)
        return maximum;
    uint64_t rounded = (uint64_t)ceil(bytes / MIB) * MIB;
    return rounded < minimum ? minimum : rounded > maximum ? maximum : rounded;
}

static struct mp_curl_policy_decision denied(struct mp_curl_policy_decision decision, int reason)
{
    decision.eligible = false;
    decision.reason = reason;
    return decision;
}

uint64_t mp_curl_udp_receive_bytes(double demand_bps, double scheduling_lateness_p99_ms)
{
    double stall = positive(scheduling_lateness_p99_ms) ? 2 * scheduling_lateness_p99_ms / 1000 : 0;
    double bytes = positive(demand_bps) ? 1.75 * demand_bps / 8 * fmax(.05, stall) : 0;
    return rounded_mib(bytes, 256 * 1024, 8 * MIB);
}

struct mp_curl_policy_decision mp_curl_h3_eligible(const struct mp_curl_h3_input *i)
{
    struct mp_curl_policy_decision result = {
        .requested_bytes = mp_curl_udp_receive_bytes(i->demand_bps, i->scheduling_lateness_p99_ms),
        .projected_cpu = -1,
    };
    result.reserved_bytes = result.requested_bytes * 2;
    if (!i->https || !i->http3_supported)
        return denied(result, MP_CURL_REASON_UNSUPPORTED);
    if (i->cooldown_until_ns > i->now_ns)
        return denied(
            result, i->cooldown_reason == MP_CURL_REASON_H3_DROPS ? i->cooldown_reason : MP_CURL_REASON_H3_FAILED);
    if (!i->prefer_h3) {
        bool deficit = i->counts_settled && i->ranges_settled && positive(i->demand_bps)
            && positive(i->tcp_capacity_bytes_per_second)
            && i->tcp_capacity_bytes_per_second < 1.75 * i->demand_bps / 8;
        if (!i->capability_evidence || !i->rtt_available || !positive(i->rtt_ms) || i->rtt_ms < 50
            || !(i->out_of_order_growth || i->h2_head_of_line || deficit))
            return denied(result, MP_CURL_REASON_H3_PATH);
        if ((i->worker_cpu_available && (!isfinite(i->worker_cpu) || i->worker_cpu < 0))
            || (i->tcp_cpu_available && (!isfinite(i->tcp_cpu_ns_per_byte) || i->tcp_cpu_ns_per_byte < 0)))
            return denied(result, MP_CURL_REASON_H3_CPU);
        bool worker = i->worker_cpu_available && isfinite(i->worker_cpu) && i->worker_cpu >= 0;
        bool byte_cpu = i->tcp_cpu_available && isfinite(i->tcp_cpu_ns_per_byte) && i->tcp_cpu_ns_per_byte >= 0;
        if ((i->constrained && (!worker || !byte_cpu)) || (worker && i->worker_cpu >= .5))
            return denied(result, MP_CURL_REASON_H3_CPU);
        if (byte_cpu) {
            if (!positive(i->demand_bps))
                return denied(result, MP_CURL_REASON_H3_CPU);
            double penalty = positive(i->cpu_penalty) ? i->cpu_penalty : 1.7;
            result.projected_cpu = i->tcp_cpu_ns_per_byte * penalty * 1.75 * i->demand_bps / 8 / 1e9;
            if (!isfinite(result.projected_cpu) || result.projected_cpu > .3)
                return denied(result, MP_CURL_REASON_H3_CPU);
        }
        if (!i->hw_aes)
            return denied(result, MP_CURL_REASON_AES);
    }
    double minimum = positive(i->demand_bps) ? 1.75 * i->demand_bps / 8 * .025 : 256 * 1024;
    if (!i->udp_buffer_available || (double)i->udp_effective_bytes < minimum)
        return denied(result, MP_CURL_REASON_H3_BUFFER);
    if (!i->quic_batching)
        return denied(result, MP_CURL_REASON_BATCHING);
    result.eligible = true;
    return result;
}

struct mp_curl_policy_decision mp_curl_window_candidate(const struct mp_curl_window_input *i)
{
    struct mp_curl_policy_decision result = { 0 };
    if (!i->rtt_available || !positive(i->rtt_ms) || i->rtt_ms < 100 || i->physical_connections < 1
        || i->physical_connections > 64 || !i->consumer_draining || !isfinite(i->producer_backpressure)
        || i->producer_backpressure < 0 || i->producer_backpressure >= .05)
        return denied(result, MP_CURL_REASON_INVALID);
    double average = fmax(positive(i->file_average_bps) ? i->file_average_bps : 0,
        positive(i->delivery_average_bps) ? i->delivery_average_bps : 0);
    double burst = fmax(2 * average, positive(i->observed_peak_bps) ? i->observed_peak_bps : 0);
    if (!positive(burst) && positive(i->observed_steady_bps))
        burst = 2 * i->observed_steady_bps;
    // A lone instantaneous peak is not an average-demand prior.
    if (!positive(average) && !positive(i->observed_steady_bps))
        return denied(result, MP_CURL_REASON_INVALID);
    if (!positive(burst))
        return denied(result, MP_CURL_REASON_INVALID);
    double speed = positive(i->playback_speed) ? i->playback_speed : 1;
    double bdp = burst * speed / 8 * i->rtt_ms / 1000 / i->physical_connections;
    if (!positive(bdp))
        return denied(result, MP_CURL_REASON_INVALID);
    double target = 1.5 * bdp;
    result.requested_bytes = rounded_mib(target, MIB, 16 * MIB);
    result.policy_limited = target > 16 * MIB;
    result.reserved_bytes = result.requested_bytes * 2 * i->physical_connections;
    bool suspicion
        = (i->receive_window_available && (double)i->receive_window_bytes < bdp) || i->receive_ceiling_suspected;
    if (!suspicion)
        return denied(result, MP_CURL_REASON_AUTOTUNE);
    if (!i->manual_ceiling_available || !i->autotune_ceiling_available)
        return denied(result, MP_CURL_REASON_UNSUPPORTED);
    if (result.requested_bytes > i->manual_ceiling_bytes)
        return denied(result, MP_CURL_REASON_RECEIVE_CLAMP);
    bool short_range = i->range_bytes > 0 && (double)i->range_bytes < 8 * bdp;
    if (i->windows && !i->slow_connection_ramp)
        return denied(result, MP_CURL_REASON_AUTOTUNE);
    if (i->autotune_ceiling_bytes >= result.requested_bytes && !short_range && !i->slow_connection_ramp)
        return denied(result, MP_CURL_REASON_AUTOTUNE);
    if (!i->socket_budget_available || result.reserved_bytes > i->available_socket_bytes)
        return denied(result, MP_CURL_REASON_SOCKET_BUDGET);
    result.eligible = true;
    result.reason = MP_CURL_REASON_WINDOW;
    return result;
}

int mp_curl_h3_revert(struct mp_curl_h3_watch *watch, const struct mp_curl_h3_window *window)
{
    if (window->end_ns <= 0 || window->end_ns < watch->last_observed_ns)
        return MP_CURL_REASON_NONE;
    watch->last_observed_ns = window->end_ns;
    if (window->udp_drops_available) {
        bool dropped = watch->udp_drops_seen && window->udp_drops > watch->udp_drops;
        watch->udp_drops = window->udp_drops;
        watch->udp_drops_seen = true;
        if (dropped)
            return MP_CURL_REASON_H3_DROPS;
    } else
        watch->udp_drops_seen = false;
    if (watch->last_cpu_window_ns && window->end_ns - watch->last_cpu_window_ns < SECOND)
        return MP_CURL_REASON_NONE;
    if (window->end_ns <= 0)
        return MP_CURL_REASON_NONE;
    bool adjacent = !watch->last_cpu_window_ns || window->end_ns - watch->last_cpu_window_ns <= 3 * SECOND / 2;
    watch->last_cpu_window_ns = window->end_ns;
    bool over = window->worker_cpu_available && isfinite(window->worker_cpu) && window->worker_cpu > .3;
    watch->cpu_over_budget_windows = over ? (adjacent ? 1 + (watch->cpu_over_budget_windows > 0) : 1) : 0;
    return watch->cpu_over_budget_windows >= 2 ? MP_CURL_REASON_H3_CPU : MP_CURL_REASON_NONE;
}

bool mp_curl_h3_comparison_accepted(const struct mp_curl_h3_comparison *c, int *reason)
{
    *reason = MP_CURL_REASON_INCONCLUSIVE;
    if (!positive(c->tcp_before_bytes_per_second) || !positive(c->tcp_after_bytes_per_second)
        || !positive(c->h3_bytes_per_second))
        return false;
    double baseline = (c->tcp_before_bytes_per_second + c->tcp_after_bytes_per_second) / 2;
    double spread = fabs(c->tcp_before_bytes_per_second - c->tcp_after_bytes_per_second) / baseline;
    if (spread > .15)
        return false;
    if (c->udp_drops_available && c->udp_drops_after > c->udp_drops_before) {
        *reason = MP_CURL_REASON_H3_DROPS;
        return false;
    }
    if (c->cache_pause) {
        *reason = MP_CURL_REASON_DEADLINE;
        return false;
    }
    if (c->cpu_available) {
        bool cost = isfinite(c->tcp_cpu_ns_per_byte) && c->tcp_cpu_ns_per_byte >= 0 && isfinite(c->h3_cpu_ns_per_byte)
            && c->h3_cpu_ns_per_byte >= 0;
        double projected = positive(c->demand_bps) ? c->h3_cpu_ns_per_byte * 1.75 * c->demand_bps / 8 / 1e9 : INFINITY;
        if (!cost || (c->h3_cpu_ns_per_byte > 2 * c->tcp_cpu_ns_per_byte && projected > .3)) {
            *reason = MP_CURL_REASON_H3_CPU;
            return false;
        }
    }
    bool accepted = c->h3_bytes_per_second >= baseline * (1 + fmax(.15, 2 * spread));
    *reason = accepted ? MP_CURL_REASON_ACCEPTED : MP_CURL_REASON_REJECTED;
    return accepted;
}
