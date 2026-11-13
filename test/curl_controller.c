#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "stream/curl_controller.h"

#define SECOND INT64_C(1000000000)
#define MIB UINT64_C(1048576)

static struct mp_curl_observation observation(int requests)
{
    return (struct mp_curl_observation) {
        .now_ns = SECOND,
        .wall_ms = 100000,
        .epoch = 1,
        .profile_generation = 1,
        .requests = requests,
        .max_requests = 4,
        .capacity = 8000000,
        .ordered_ratio = .8,
        .demand_bps = 80000000,
        .buffer_seconds = 2.5,
        .buffer_valid = true,
        .network_wait_available = true,
        .network_wait_ratio = .8,
        .resources_valid = true,
        .free_raw_bytes = 64 * MIB,
        .free_descriptors = 32,
        .range_limit = 16 * MIB,
        .batch_cap = 32 * MIB,
        .next_offset = MIB,
        .rtt_ms = 100,
        .ttfb_ms = 120,
        .consumer_wanted = true,
        .consumer_progress = true,
        .profile = { .requests = requests, .connections = requests, .http_version = 11, .range_bytes = 512 * 1024 },
    };
}

static void start_recovery(struct mp_curl_controller *c, struct mp_curl_observation *o)
{
    mp_curl_controller_decide(c, o);
    o->now_ns += SECOND;
    mp_curl_controller_decide(c, o);
    assert(c->experiment && c->state == MP_CURL_BASELINE_BEFORE);
    assert(c->phase_end - c->phase_start == c->batch_bytes);
    assert(c->required_raw_bytes <= o->free_raw_bytes);
    assert(c->required_descriptors <= o->free_descriptors);
}

// Complete the exact adjacent interval the public policy asked the worker to
// download. Rates are independent link inputs, never expected values copied
// out of the controller under test.
static int complete(struct mp_curl_controller *c, struct mp_curl_observation *o, double rate)
{
    o->requests = c->target_requests;
    o->profile.requests = o->requests;
    o->profile.connections = o->requests;
    o->profile.range_bytes = c->trial_range_bytes;
    o->phase_start = c->phase_start;
    o->phase_bytes = c->batch_bytes;
    o->phase_seconds = o->phase_bytes / rate;
    o->capacity = rate;
    o->ordered_ratio = o->demand_bps > 0 ? rate / (o->demand_bps / 8) : 1;
    o->buffer_seconds += o->phase_seconds * (o->ordered_ratio - 1);
    o->now_ns += (int64_t)(o->phase_seconds * SECOND);
    o->wall_ms += (int64_t)(o->phase_seconds * 1000);
    o->ordered_bytes += o->phase_bytes;
    o->next_offset = c->phase_end;
    return mp_curl_controller_decide(c, o);
}

static void acceptance_and_noise(void)
{
    struct mp_curl_controller c = { 0 };
    struct mp_curl_observation o = observation(1);
    start_recovery(&c, &o);
    uint64_t end = c.phase_end;
    assert(complete(&c, &o, 8000000) == 2);
    assert(c.state == MP_CURL_CANDIDATE && c.phase_start == end);
    uint64_t next = c.phase_end;
    assert(mp_curl_controller_decide(&c, &o) == 2);
    assert(c.state == MP_CURL_CANDIDATE && c.phase_end == next && c.experiment);
    end = c.phase_end;
    assert(complete(&c, &o, 20000000) == 1);
    assert(c.state == MP_CURL_BASELINE_AFTER && c.phase_start == end);
    assert(complete(&c, &o, 8100000) == 2);
    assert(c.reason == MP_CURL_REASON_ACCEPTED && !c.experiment);
    assert(c.counts[1].playback_samples == 1 && c.counts[1].profile.http_version == 11);
    assert(c.counts[1].profile.connections == 2);
    assert(c.counts[1].profile.range_bytes == o.profile.range_bytes && c.counts[1].updated_ms == o.wall_ms);
    assert(mp_curl_controller_best(&c, 4) == 2);
    o.seeking = true;
    o.now_ns++;
    o.requests = 1;
    assert(mp_curl_controller_decide(&c, &o) == 2);
    assert(c.urgency == 1 && c.counts[1].playback_samples == 1);

    memset(&c, 0, sizeof(c));
    o = observation(1);
    start_recovery(&c, &o);
    complete(&c, &o, 8000000);
    complete(&c, &o, 20000000);
    complete(&c, &o, 11000000);
    assert(c.reason == MP_CURL_REASON_INCONCLUSIVE);
    assert(c.target_requests == 1 && c.counts[1].playback_samples == 0);
    assert(c.counts[1].rejected_ns == 0);
}

static void constraints_and_invalidations(void)
{
    struct mp_curl_controller c = { 0 };
    struct mp_curl_observation o = observation(1);
    o.network_wait_available = false;
    mp_curl_controller_decide(&c, &o);
    o.now_ns += SECOND;
    assert(mp_curl_controller_decide(&c, &o) == 1 && !c.experiment);
    o.now_ns++;
    o.network_wait_available = true;
    o.free_raw_bytes = 256 * 1024;
    mp_curl_controller_decide(&c, &o);
    assert(c.action == MP_CURL_DEFER && c.reason == MP_CURL_REASON_RAW_MEMORY);
    assert(c.counts[1].rejected_ns == 0);
    o.now_ns++;
    o.free_raw_bytes = 64 * MIB;
    o.free_descriptors = 1;
    mp_curl_controller_decide(&c, &o);
    assert(c.action == MP_CURL_DEFER && c.reason == MP_CURL_REASON_DESCRIPTORS);
    o.now_ns++;
    o.free_descriptors = 32;
    mp_curl_controller_decide(&c, &o);
    assert(c.experiment);
    o.paused = true;
    mp_curl_controller_tick(&c, &o);
    assert(!c.experiment && c.state == MP_CURL_PAUSED && !c.counts[1].playback_samples);
    unsigned samples = c.counts[0].playback_samples;
    o.epoch = 0;
    mp_curl_controller_decide(&c, &o);
    assert(c.reason == MP_CURL_REASON_EPOCH && c.counts[0].playback_samples == samples);

    memset(&c, 0, sizeof(c));
    o = observation(15);
    o.max_requests = 16;
    o.free_descriptors = 17;
    o.buffer_seconds = 1;
    o.capacity = 50000000;
    o.demand_bps = 1000000000;
    o.ordered_ratio = .4;
    mp_curl_controller_decide(&c, &o);
    assert(c.experiment && c.candidate == 16 && c.required_descriptors == 17);

    memset(&c, 0, sizeof(c));
    o = observation(1);
    start_recovery(&c, &o);
    o.phase_short = true;
    o.now_ns++;
    mp_curl_controller_decide(&c, &o);
    assert(!c.experiment && c.reason == MP_CURL_REASON_INCONCLUSIVE);
    assert(c.counts[1].playback_samples == 0 && c.counts[1].rejected_ns == 0);

    memset(&c, 0, sizeof(c));
    o = observation(1);
    start_recovery(&c, &o);
    o.now_ns = c.completion_deadline_ns + 1;
    mp_curl_controller_tick(&c, &o);
    assert(c.action == MP_CURL_RESTORE && c.reason == MP_CURL_REASON_DEADLINE);
    assert(!c.experiment && c.counts[1].rejected_ns == 0);
}

static void fixed_and_stall(void)
{
    struct mp_curl_controller c = { 0 };
    struct mp_curl_observation o = observation(1);
    o.fixed_requests = 3;
    assert(mp_curl_controller_decide(&c, &o) == 3);
    assert(!c.experiment && c.reason == MP_CURL_REASON_FIXED);
    assert(c.counts[0].playback_samples == 1);
    o.fixed_requests = 0;
    o.prefix_incomplete = true;
    o.now_ns += 2 * SECOND;
    mp_curl_controller_tick(&c, &o);
    assert(c.action == MP_CURL_CANCEL_SUFFIX && c.urgency >= .7);
    o.ordered_bytes += 1;
    o.now_ns += SECOND / 4;
    mp_curl_controller_tick(&c, &o);
    // A progressed prefix allows the caller to acknowledge its rescue by
    // consuming the next valid baseline observation.
    assert(c.prefix_progress_ns == o.now_ns && c.action == MP_CURL_HOLD);
}

static void hysteresis(void)
{
    struct mp_curl_controller c = { 0 };
    struct mp_curl_observation o = observation(2);
    o.buffer_seconds = 20;
    o.ordered_ratio = 2;
    o.capacity = 20000000;
    o.cache_target_reached = true;
    o.explore = true;
    c.counts[0] = (struct mp_curl_count_evidence) {
        .samples = 1,
        .playback_samples = 1,
        .capacity = 5000000,
        .profile = { .http_version = 11, .range_bytes = 2 * MIB },
    };
    mp_curl_controller_decide(&c, &o);
    o.now_ns += 9 * SECOND;
    mp_curl_controller_decide(&c, &o);
    assert(c.experiment && c.candidate == 1);
    complete(&c, &o, 20000000);
    complete(&c, &o, 8000000);
    complete(&c, &o, 20000000);
    assert(c.reason == MP_CURL_REASON_REJECTED && c.target_requests == 2);
    int64_t rejected = c.counts[0].rejected_ns;
    o.requests = 2;
    o.capacity = 20000000;
    o.ordered_ratio = 2;
    o.now_ns += 31 * SECOND;
    mp_curl_controller_decide(&c, &o); // retire back to the baseline
    o.now_ns++;
    mp_curl_controller_decide(&c, &o);
    assert(c.counts[0].rejected_ns == rejected);
    assert(!c.experiment || c.candidate != 1);
    // Even after the cooldown, identical link conditions cannot repeatedly
    // re-run a rejected downshift. A material path change permits it again.
    c.experiment = false;
    c.state = MP_CURL_OBSERVE;
    c.action = MP_CURL_HOLD;
    c.last_trial_ns = 0;
    o.capacity = 32000000;
    o.ordered_ratio = 3.2;
    o.now_ns += 31 * SECOND;
    mp_curl_controller_decide(&c, &o);
    assert(c.experiment && c.candidate == 1);
}

static void simulated_link(const double rates[4], double demand, int expected)
{
    struct mp_curl_controller c = { 0 };
    struct mp_curl_observation o = observation(1);
    o.demand_bps = demand;
    o.explore = true;
    int selected = 1;
    int last_changes = 0;
    for (int step = 0; step < 240; step++) {
        int previous = selected;
        if (c.experiment) {
            // A little independent deterministic Wi-Fi noise must not cause
            // perpetual count oscillation or promote a near-tie higher count.
            double noise = 1 + .02 * sin(step * 1.7);
            selected = complete(&c, &o, rates[c.target_requests - 1] * noise);
        } else {
            o.requests = selected;
            o.profile.requests = selected;
            o.profile.connections = selected;
            o.capacity = rates[selected - 1];
            o.ordered_ratio = o.capacity / (demand / 8);
            o.buffer_seconds = fmax(0, fmin(24, o.buffer_seconds + 1.5 * (o.ordered_ratio - 1)));
            o.cache_target_reached = o.buffer_seconds >= 20;
            o.now_ns += 3 * SECOND / 2;
            o.ordered_bytes += (uint64_t)(o.capacity * 1.5);
            o.phase_bytes = 0;
            o.phase_seconds = 0;
            selected = mp_curl_controller_decide(&c, &o);
        }
        assert(selected >= 1 && selected <= 4);
        if (step > 200 && selected != previous)
            last_changes++;
    }
    assert(selected == expected);
    assert(last_changes <= 2);
}

int main(void)
{
    acceptance_and_noise();
    constraints_and_invalidations();
    fixed_and_stall();
    hysteresis();
    const double lan[4] = { 30000000, 15000000, 12000000, 9000000 };
    const double capped[4] = { 8000000, 20000000, 20500000, 16000000 };
    const double remote[4] = { 8000000, 16000000, 24000000, 30000000 };
    simulated_link(lan, 80000000, 1);
    simulated_link(capped, 80000000, 2);
    simulated_link(remote, 160000000, 4);
    puts("curl controller policy and A/B/A simulator passed");
    return 0;
}
