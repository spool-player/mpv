#pragma once

#include <stdbool.h>
#include <stdint.h>

// Pure transport policy. Curl, clocks, admission and publication stay with the
// worker. A decision describes bytes the worker must reserve before applying it.
enum mp_curl_controller_state {
    MP_CURL_OBSERVE,
    MP_CURL_BASELINE_BEFORE,
    MP_CURL_CANDIDATE,
    MP_CURL_BASELINE_AFTER,
    MP_CURL_RETIRING,
    MP_CURL_PAUSED,
};
enum mp_curl_controller_action {
    MP_CURL_HOLD,
    MP_CURL_START_PHASE,
    MP_CURL_RESTORE,
    MP_CURL_DEFER,
    MP_CURL_CANCEL_SUFFIX,
};
enum mp_curl_reason {
    MP_CURL_REASON_NONE = 0,
    MP_CURL_REASON_STARTUP = 1,
    MP_CURL_REASON_DEMAND = 2,
    MP_CURL_REASON_HOL = 3,
    MP_CURL_REASON_CPU = 4,
    MP_CURL_REASON_EXPLORE = 5,
    MP_CURL_REASON_WINDOW = 6,
    MP_CURL_REASON_PROTOCOL = 7,
    MP_CURL_REASON_RANGE = 8,
    MP_CURL_REASON_MEMORY = 9,
    MP_CURL_REASON_EPOCH = 10,
    MP_CURL_REASON_HEADROOM = 11,
    MP_CURL_REASON_INVALID = 12,
    MP_CURL_REASON_ACCEPTED = 13,
    MP_CURL_REASON_REJECTED = 14,
    MP_CURL_REASON_INCONCLUSIVE = 15,
    MP_CURL_REASON_POOL = 16,
    MP_CURL_REASON_DEADLINE = 17,
    MP_CURL_REASON_FIXED = 18,
    MP_CURL_REASON_OFF = 19,
    MP_CURL_REASON_UNSUPPORTED = 20,
    MP_CURL_REASON_REPRESENTATION = 21,
    MP_CURL_REASON_VALIDATOR = 22,
    MP_CURL_REASON_DESCRIPTORS = 23,
    MP_CURL_REASON_BATCH = 24,
    MP_CURL_REASON_SOCKET_BUDGET = 25,
    MP_CURL_REASON_RAW_MEMORY = 26,
    MP_CURL_REASON_FOREIGN = 27,
    MP_CURL_REASON_H3_PATH = 28,
    MP_CURL_REASON_H3_CPU = 29,
    MP_CURL_REASON_H3_BUFFER = 30,
    MP_CURL_REASON_H3_DROPS = 31,
    MP_CURL_REASON_AES = 32,
    MP_CURL_REASON_BATCHING = 33,
    MP_CURL_REASON_RECEIVE_CLAMP = 34,
    MP_CURL_REASON_AUTOTUNE = 35,
    MP_CURL_REASON_FLOW_CONTROL = 36,
    MP_CURL_REASON_H3_FAILED = 37,
    MP_CURL_REASON_H2_FAILED = 38,
};
struct mp_curl_profile {
    int requests, connections, http_mode, http_version, receive_profile;
    uint64_t receive_bytes, range_bytes, udp_receive_bytes;
};
struct mp_curl_count_evidence {
    double capacity;
    int64_t sampled_ns, rejected_ns, updated_ms;
    double rejected_capacity;
    unsigned samples, playback_samples;
    int source;
    struct mp_curl_profile profile;
};
struct mp_curl_controller {
    struct mp_curl_count_evidence counts[16];
    int state, reason, baseline, candidate, action, target_requests;
    int64_t epoch, profile_generation, last_trial_ns, started_ns, last_now_ns;
    int64_t stable_since_ns, negative_since_ns, deficit_since_ns, last_decision_ns;
    int64_t prefix_progress_ns, last_buffer_ns;
    int64_t completion_deadline_ns, safety_deadline_ns, trial_duration_ns;
    uint64_t last_ordered_bytes, phase_start, phase_end, trial_range_bytes;
    uint64_t batch_bytes, required_raw_bytes;
    int required_descriptors;
    double baseline_capacity, candidate_capacity, urgency, runway_seconds;
    double buffer_slope, last_buffer_seconds, trial_buffer_seconds;
    double previous_capacity, phase_buffer_seconds, candidate_buffer_gain;
    double baseline_buffer_slope, candidate_buffer_slope;
    bool buffer_seen, experiment, consuming, recovery_adoption, trial_clock, baseline_comparable;
    struct mp_curl_profile active_profile, candidate_profile;
};
struct mp_curl_observation {
    int64_t now_ns, wall_ms, epoch, profile_generation;
    int requests, max_requests, fixed_requests;
    double capacity, ordered_ratio, demand_bps, buffer_seconds;
    double hol_ratio, worker_cpu, network_wait_ratio, storage_ratio;
    double phase_seconds, rtt_ms, ttfb_ms;
    bool worker_cpu_available, network_wait_available;
    bool foreign, blocked, paused, explore, seeking, eof, retrying, overlap;
    bool memory_pressure, underrun, buffer_valid, cache_target_reached;
    bool reorder_growing, playable_progress, prefix_incomplete, consumer_wanted;
    bool consumer_progress, phase_short, constrained, resources_valid;
    uint64_t ordered_bytes, next_offset, phase_start, phase_bytes;
    uint64_t range_limit, free_raw_bytes, batch_cap;
    int free_descriptors;
    struct mp_curl_profile profile;
};
// Called on the worker's 250 ms tick. May request restoration/rescue, but never
// advances a comparison or starts speculative work without completed feedback.
void mp_curl_controller_tick(struct mp_curl_controller *c, const struct mp_curl_observation *o);
// A completed ordinary observation or one exact requested phase. START_PHASE
// is applied only after its raw/descriptor shape is admitted by the worker.
int mp_curl_controller_decide(struct mp_curl_controller *c, const struct mp_curl_observation *o);
int mp_curl_controller_best(const struct mp_curl_controller *c, int maximum);
int mp_curl_controller_startup_requests(const struct mp_curl_controller *c,
                                        int maximum, double demand_bps,
                                        double ttfb_ms, int64_t now_ns,
                                        int64_t age_ns, int64_t prefix_wait_ns);
