#include <float.h>
#include <math.h>
#include <string.h>

#include "curl_controller.h"

#define SECOND INT64_C(1000000000)
#define MIB UINT64_C(1048576)

static const double demand_headroom = 1.75;
static const double recovery_urgency = 0.7;

static int bounded_count(int count, int maximum)
{
    maximum = maximum < 1 ? 1 : maximum > 16 ? 16 : maximum;
    return count < 1 ? 1 : count > maximum ? maximum : count;
}

static double clamp(double value, double minimum, double maximum)
{
    return fmax(minimum, fmin(value, maximum));
}

static bool same_shape(const struct mp_curl_profile *a, const struct mp_curl_profile *b)
{
    return a->http_version == b->http_version && a->http_mode == b->http_mode
        && (a->http_version == 11 || a->connections == b->connections) && a->receive_profile == b->receive_profile
        && a->receive_bytes == b->receive_bytes && a->range_bytes == b->range_bytes
        && a->udp_receive_bytes == b->udp_receive_bytes;
}

static bool compatible_evidence(const struct mp_curl_count_evidence *e, const struct mp_curl_profile *p)
{
    // Only explicit Qt probe priors have unknown protocol/range. They may
    // bootstrap an untouched TCP baseline, never stand in for a known native
    // profile or a manual receive/QUIC measurement.
    if (e->source == 1 && !e->profile.http_version && !e->profile.range_bytes)
        return !p->receive_profile && !p->udp_receive_bytes && p->http_version != 30;
    return same_shape(&e->profile, p);
}

static double confidence(const struct mp_curl_count_evidence *e, int64_t now)
{
    if (!e->samples || !isfinite(e->capacity) || e->capacity <= 0)
        return 0;
    double age = now > e->sampled_ns ? (now - e->sampled_ns) / (double)SECOND : 0;
    if (age > 30.0 * 24 * 60 * 60)
        return 0;
    return fmin(e->samples, 64) * exp2(-age / 600) * (e->playback_samples ? 4 : 1);
}

int mp_curl_controller_best(const struct mp_curl_controller *c, int maximum)
{
    maximum = bounded_count(maximum, 16);
    double best = 0;
    bool actual = false;
    for (int n = 0; n < maximum; n++) {
        const struct mp_curl_count_evidence *e = &c->counts[n];
        if (confidence(e, c->last_now_ns) < .125 || !compatible_evidence(e, &c->active_profile))
            continue;
        if (e->playback_samples && !actual) {
            actual = true;
            best = 0;
        }
        if (!actual || e->playback_samples)
            best = fmax(best, e->capacity);
    }
    for (int n = 0; n < maximum; n++) {
        const struct mp_curl_count_evidence *e = &c->counts[n];
        if (confidence(e, c->last_now_ns) >= .125 && compatible_evidence(e, &c->active_profile)
            && (!actual || e->playback_samples) && e->capacity >= best * .9)
            return n + 1;
    }
    return 1;
}

int mp_curl_controller_startup_requests(const struct mp_curl_controller *c,
                                        int maximum, double demand_bps,
                                        double ttfb_ms, int64_t now_ns,
                                        int64_t age_ns, int64_t prefix_wait_ns)
{
    const struct mp_curl_count_evidence *single = &c->counts[0];
    double pacing_ms = fmax(50, ttfb_ms);
    // Give a proven single request time to open the file. A stalled prefix
    // still needs the startup ramp, even when the previous run was fast.
    double grace_ns = clamp(4 * pacing_ms, 250, 500) * (SECOND / 1000);
    if (prefix_wait_ns < grace_ns && demand_bps > 0
        && confidence(single, now_ns) >= .125 && compatible_evidence(single, &c->active_profile)
        && single->capacity >= demand_headroom * demand_bps / 8)
        return 1;

    int ladder = age_ns >= 2 * pacing_ms * (SECOND / 1000) ? 4
        : age_ns >= pacing_ms * (SECOND / 1000) ? 2 : 1;
    int best = mp_curl_controller_best(c, maximum);
    return bounded_count(ladder > best ? ladder : best, maximum);
}

static void retire(struct mp_curl_controller *c, int reason)
{
    // Only a completed comparison earns the full pause before the next one;
    // a disturbed trial may be retried after eight seconds.
    if (c->experiment && (reason == MP_CURL_REASON_INCONCLUSIVE || reason == MP_CURL_REASON_DEADLINE)) {
        c->last_trial_ns = c->last_now_ns - 22 * SECOND;
        // A downward trial that could not finish is not retried on the same
        // path either: each attempt starves an already waiting consumer.
        if (c->candidate && c->candidate < c->baseline) {
            c->counts[c->candidate - 1].rejected_ns = c->last_now_ns;
            c->counts[c->candidate - 1].rejected_capacity = c->previous_capacity;
        }
    }
    if (c->experiment) {
        c->target_requests = c->baseline;
        c->state = MP_CURL_RETIRING;
        c->action = MP_CURL_RESTORE;
    }
    c->experiment = false;
    c->candidate = 0;
    c->reason = reason;
}

static bool current_epoch(struct mp_curl_controller *c, const struct mp_curl_observation *o)
{
    if (c->epoch && o->epoch < c->epoch) {
        c->reason = MP_CURL_REASON_EPOCH;
        return false;
    }
    if (c->epoch && o->epoch > c->epoch) {
        memset(c, 0, sizeof(*c));
    }
    c->epoch = o->epoch;
    if (c->profile_generation && o->profile_generation != c->profile_generation) {
        struct mp_curl_profile expected = c->active_profile;
        expected.range_bytes = c->trial_range_bytes;
        // A new transfer shape ends the experiment, not what was learned:
        // evidence is matched against the shape whenever it is used.
        if (!c->experiment || !same_shape(&expected, &o->profile)) {
            retire(c, MP_CURL_REASON_INCONCLUSIVE);
            c->stable_since_ns = 0;
        }
    }
    c->profile_generation = o->profile_generation;
    return true;
}

void mp_curl_controller_tick(struct mp_curl_controller *c, const struct mp_curl_observation *o)
{
    if (!current_epoch(c, o) || o->now_ns < c->last_now_ns)
        return;
    if (!c->started_ns)
        c->started_ns = o->now_ns;
    if (o->ordered_bytes != c->last_ordered_bytes) {
        if (c->action == MP_CURL_CANCEL_SUFFIX) {
            c->action = c->state == MP_CURL_RETIRING ? MP_CURL_RESTORE : MP_CURL_HOLD;
            if (c->state != MP_CURL_RETIRING)
                c->state = MP_CURL_OBSERVE;
        }
        c->prefix_progress_ns = o->now_ns;
        c->last_ordered_bytes = o->ordered_bytes;
    } else if (!c->prefix_progress_ns) {
        c->prefix_progress_ns = o->now_ns;
    }
    c->last_now_ns = o->now_ns;
    c->runway_seconds = INFINITY;
    c->urgency = 0;
    bool buffer = o->buffer_valid && isfinite(o->buffer_seconds) && o->buffer_seconds >= 0;
    if (buffer && !o->seeking && !o->paused) {
        if (c->buffer_seen && o->now_ns > c->last_buffer_ns) {
            double seconds = (o->now_ns - c->last_buffer_ns) / (double)SECOND;
            double slope = (o->buffer_seconds - c->last_buffer_seconds) / seconds;
            double alpha = 1 - exp(-seconds / 5);
            c->buffer_slope += alpha * (slope - c->buffer_slope);
        }
        c->buffer_seen = true;
        c->last_buffer_ns = o->now_ns;
        c->last_buffer_seconds = o->buffer_seconds;
        if (c->buffer_slope < -.01)
            c->runway_seconds = o->buffer_seconds / -c->buffer_slope;
        if (isfinite(o->ordered_ratio) && o->ordered_ratio >= 0 && o->ordered_ratio < 1)
            c->runway_seconds = fmin(c->runway_seconds, o->buffer_seconds / (1 - o->ordered_ratio));
        c->urgency = fmax(clamp((8 - o->buffer_seconds) / 8, 0, 1), clamp((20 - c->runway_seconds) / 20, 0, 1));
    }
    if (o->underrun || o->seeking)
        c->urgency = 1;
    if (!buffer && !o->playable_progress && !o->paused
        && (o->now_ns - c->started_ns >= SECOND || o->ordered_bytes >= 512 * 1024))
        c->urgency = fmax(c->urgency, recovery_urgency);
    // The head range is stalled once it has taken longer than fetching it
    // afresh would: a new request's round trips plus one range at the
    // current per-lane rate, with half again for jitter. A flat timeout
    // would keep cancelling healthy ranges on every slow path.
    // A new phase has no capacity of its own yet; the count's evidence does.
    int lanes = o->requests < 1 ? 1 : o->requests > 16 ? 16 : o->requests;
    double rate = c->counts[lanes - 1].capacity > 0 ? c->counts[lanes - 1].capacity : o->capacity;
    double lane_rate = isfinite(rate) && rate > 0 ? rate / lanes : 0;
    double range_seconds = lane_rate > 0 ? o->profile.range_bytes / lane_rate : 0;
    double prefix_seconds = fmax(1, 2 * fmax(0, o->rtt_ms) / 1000 + fmax(0, o->ttfb_ms) / 1000
                                        + 1.5 * range_seconds);
    bool stalled = o->prefix_incomplete && !o->blocked && !o->paused
        && o->now_ns - c->prefix_progress_ns >= prefix_seconds * SECOND;
    if (stalled)
        c->urgency = fmax(c->urgency, recovery_urgency);
    if (c->buffer_slope < -.01 && !o->paused && !o->seeking) {
        if (!c->negative_since_ns)
            c->negative_since_ns = o->now_ns;
    } else
        c->negative_since_ns = 0;
    bool deficit = o->demand_bps > 0 && isfinite(o->capacity) && o->capacity < o->demand_bps / 8;
    if (deficit) {
        if (!c->deficit_since_ns)
            c->deficit_since_ns = o->now_ns;
    } else
        c->deficit_since_ns = 0;
    bool stable = c->urgency <= .2 && buffer && o->buffer_seconds >= 8 && o->cache_target_reached && !o->paused
        && !o->seeking && !o->eof && !o->foreign && !o->blocked && !o->memory_pressure;
    if (stable) {
        if (!c->stable_since_ns)
            c->stable_since_ns = o->now_ns;
    } else
        c->stable_since_ns = 0;
    if (o->paused || o->seeking || o->retrying || o->overlap || o->memory_pressure || o->foreign || o->blocked) {
        retire(c,
            o->memory_pressure ? MP_CURL_REASON_MEMORY
                : o->foreign   ? MP_CURL_REASON_FOREIGN
                               : MP_CURL_REASON_INCONCLUSIVE);
        c->buffer_seen = false;
        c->buffer_slope = 0;
        if (o->paused) {
            c->state = MP_CURL_PAUSED;
            c->urgency = 0;
        }
    } else if (c->experiment && !c->trial_clock && isfinite(o->phase_seconds) && o->phase_seconds > 0) {
        // The trial's ranges queue behind those already in flight. Its time
        // runs from its first measured byte, not from the decision.
        c->trial_clock = true;
        c->completion_deadline_ns = o->now_ns - (int64_t)(o->phase_seconds * SECOND) + c->trial_duration_ns;
    } else if (c->experiment
        && ((c->completion_deadline_ns && o->now_ns > c->completion_deadline_ns)
            || (c->safety_deadline_ns && o->now_ns > c->safety_deadline_ns)
            || (buffer && c->trial_buffer_seconds - o->buffer_seconds > 2 - c->urgency)
            // An underrun ends a trial that had buffer to lose. A consumer that
            // was already starving underruns throughout the very trial that
            // may get it out.
            || (o->underrun && c->trial_buffer_seconds >= 1))) {
        retire(c, MP_CURL_REASON_DEADLINE);
    }
    if (stalled) {
        bool trial = c->experiment;
        retire(c, MP_CURL_REASON_HOL);
        c->action = MP_CURL_CANCEL_SUFFIX;
        if (!trial)
            c->target_requests = bounded_count(o->requests, o->max_requests);
        // A rescue receives its own prefix-progress deadline; repeated ticks
        // must not continuously cancel a freshly reissued suffix.
        c->prefix_progress_ns = o->now_ns;
    }
}

static bool rejected(const struct mp_curl_count_evidence *e, const struct mp_curl_observation *o)
{
    return e->rejected_ns
        && (o->now_ns - e->rejected_ns < 30 * SECOND
            || (o->capacity > e->rejected_capacity * .67 && o->capacity < e->rejected_capacity * 1.5));
}

static void record(struct mp_curl_controller *c, const struct mp_curl_observation *o)
{
    struct mp_curl_count_evidence *e = &c->counts[o->requests - 1];
    if (!same_shape(&e->profile, &o->profile))
        memset(e, 0, sizeof(*e));
    e->capacity = e->playback_samples ? e->capacity * .75 + o->capacity * .25 : o->capacity;
    e->sampled_ns = o->now_ns;
    e->updated_ms = o->wall_ms;
    e->source = 2;
    e->profile = o->profile;
    e->profile.requests = o->requests;
    if (e->samples < UINT32_MAX)
        e->samples++;
    if (e->playback_samples < UINT32_MAX)
        e->playback_samples++;
}

static uint64_t power_floor(uint64_t value)
{
    uint64_t result = value ? 1 : 0;
    while (result && result <= value / 2)
        result *= 2;
    return result;
}

static uint64_t minimum(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static bool shape(struct mp_curl_controller *c, const struct mp_curl_observation *o, int candidate, bool consuming,
                  bool recycled)
{
    if (!o->resources_valid) {
        c->reason = MP_CURL_REASON_HEADROOM;
        return false;
    }
    int k = candidate > o->requests ? candidate : o->requests;
    // Consuming phases recycle descriptors as the ordered prefix drains;
    // reserve one lane set plus a leading safety descriptor, not every range
    // in all three batches. Full-cache comparisons retain all six lane sets.
    int descriptors = consuming ? k + 1 : 6 * k;
    // An exploration while playback drains a full queue reuses descriptors
    // as they free, so none need be free when it is decided.
    if (!recycled && o->free_descriptors < descriptors) {
        c->reason = MP_CURL_REASON_DESCRIPTORS;
        return false;
    }
    uint64_t cap = o->batch_cap ? o->batch_cap : (o->constrained ? 8 : 32) * MIB;
    uint64_t limit = minimum(
        o->range_limit ? o->range_limit : 16 * MIB, o->profile.range_bytes ? o->profile.range_bytes : 512 * 1024);
    uint64_t smallest = power_floor(minimum(consuming ? 64 * 1024 : 256 * 1024, limit));
    uint64_t range = power_floor(minimum(limit, minimum(cap / (2 * k), o->free_raw_bytes / ((consuming ? 4 : 6) * k))));
    // A trial while playback is waiting must finish within its deadline, so
    // its batches shrink with the path: about a second each at the measured
    // capacity, never below the smallest useful range.
    if (consuming && isfinite(o->capacity) && o->capacity > 0) {
        uint64_t units = 1;
        while (units < (uint64_t)(2 * k))
            units *= 2;
        uint64_t paced = power_floor((uint64_t)(o->capacity / units));
        if (paced < range)
            range = paced < smallest ? smallest : paced;
    }
    if (!range || range < smallest) {
        c->reason = MP_CURL_REASON_RAW_MEMORY;
        return false;
    }
    uint64_t units = 1;
    while (units < (uint64_t)(2 * k))
        units *= 2;
    uint64_t minimum_batch = units * range;
    uint64_t budget = minimum(cap, o->free_raw_bytes / (consuming ? 2 : 3));
    if (minimum_batch > budget) {
        c->reason = MP_CURL_REASON_BATCH;
        return false;
    }
    uint64_t aim = (uint64_t)clamp(o->capacity * .5, minimum_batch, budget);
    uint64_t batch = power_floor(aim / range) * range;
    if (batch < minimum_batch)
        batch = minimum_batch;
    if (o->next_offset > UINT64_MAX - 3 * batch) {
        c->reason = MP_CURL_REASON_BATCH;
        return false;
    }
    c->trial_range_bytes = range;
    c->batch_bytes = batch;
    c->required_raw_bytes = batch * (consuming ? 2 : 3);
    c->required_descriptors = descriptors;
    c->consuming = consuming;
    return true;
}

static void phase(struct mp_curl_controller *c, int state, int requests)
{
    c->state = state;
    c->action = MP_CURL_START_PHASE;
    c->target_requests = requests;
    c->phase_end = c->phase_start + c->batch_bytes;
}

int mp_curl_controller_decide(struct mp_curl_controller *c, const struct mp_curl_observation *o)
{
    int selected = bounded_count(o->requests, o->max_requests);
    if (c->epoch == o->epoch && c->last_decision_ns && o->now_ns <= c->last_decision_ns)
        return c->target_requests ? bounded_count(c->target_requests, o->max_requests) : selected;
    if (o->requests != selected || o->max_requests < 1 || o->max_requests > 16 || !current_epoch(c, o)) {
        c->reason = c->reason == MP_CURL_REASON_EPOCH ? c->reason : MP_CURL_REASON_INVALID;
        return selected;
    }
    c->last_decision_ns = o->now_ns;
    mp_curl_controller_tick(c, o);
    if (c->action == MP_CURL_CANCEL_SUFFIX)
        return selected;
    if (o->seeking) {
        c->active_profile = o->profile;
        int best = mp_curl_controller_best(c, o->max_requests);
        c->target_requests = o->fixed_requests ? bounded_count(o->fixed_requests, o->max_requests) : best;
        c->action = MP_CURL_RESTORE;
        c->recovery_adoption = true;
        c->reason = MP_CURL_REASON_STARTUP;
        c->state = MP_CURL_OBSERVE;
        return c->target_requests;
    }
    if (c->state == MP_CURL_RETIRING || c->state == MP_CURL_PAUSED) {
        int target = c->target_requests ? bounded_count(c->target_requests, o->max_requests) : selected;
        c->state = o->paused ? MP_CURL_PAUSED : MP_CURL_OBSERVE;
        c->action = c->state == MP_CURL_PAUSED ? MP_CURL_HOLD : MP_CURL_RESTORE;
        return o->fixed_requests ? bounded_count(o->fixed_requests, o->max_requests) : target;
    }
    if (o->paused || o->seeking || o->blocked || o->foreign || o->retrying || o->overlap || o->memory_pressure
        || o->phase_short || !isfinite(o->capacity) || o->capacity <= 0) {
        retire(c, o->foreign ? MP_CURL_REASON_FOREIGN : MP_CURL_REASON_INCONCLUSIVE);
        return selected;
    }
    c->active_profile = o->profile;
    if (c->experiment) {
        if (o->phase_start != c->phase_start || o->phase_bytes != c->batch_bytes
            || o->profile.range_bytes != c->trial_range_bytes || !isfinite(o->phase_seconds) || o->phase_seconds <= 0
            || selected != c->target_requests || (c->consuming && o->consumer_wanted && !o->consumer_progress)) {
            retire(c, MP_CURL_REASON_INCONCLUSIVE);
            return c->target_requests;
        }
        double capacity = o->phase_bytes / o->phase_seconds;
        if (c->state == MP_CURL_BASELINE_BEFORE) {
            c->baseline_capacity = capacity;
            c->baseline_buffer_slope = (o->buffer_seconds - c->phase_buffer_seconds) / o->phase_seconds;
            c->phase_buffer_seconds = o->buffer_seconds;
            c->phase_start = c->phase_end;
            phase(c, MP_CURL_CANDIDATE, c->candidate);
            return c->candidate;
        }
        if (c->state == MP_CURL_CANDIDATE) {
            c->candidate_capacity = capacity;
            c->candidate_profile = o->profile;
            c->candidate_buffer_gain = o->buffer_seconds - c->phase_buffer_seconds;
            c->candidate_buffer_slope = c->candidate_buffer_gain / o->phase_seconds;
            c->phase_buffer_seconds = o->buffer_seconds;
            c->phase_start = c->phase_end;
            phase(c, MP_CURL_BASELINE_AFTER, c->baseline);
            return c->baseline;
        }
        // The first baseline drains whatever the stream had in flight. When
        // that was cut to another range size, only the second one compares.
        double reference = c->baseline_comparable ? (c->baseline_capacity + capacity) / 2 : capacity;
        double spread = reference > 0 ? fabs(c->baseline_capacity - capacity) / reference : INFINITY;
        if (!c->baseline_comparable)
            spread = reference > 0 ? 0 : INFINITY;
        double margin = fmax(.15, 2 * spread);
        bool fewer = c->candidate < c->baseline;
        bool adequate = o->demand_bps > 0 && c->candidate_capacity >= demand_headroom * o->demand_bps / 8;
        double after_slope = (o->buffer_seconds - c->phase_buffer_seconds) / o->phase_seconds;
        // When even the best path cannot yet satisfy demand, a measured
        // reduction of the buffer deficit is useful recovery. Do not reject
        // every upward step just because several steps are needed to catch up.
        bool deficit_improved = c->candidate_capacity > reference * (1 + margin)
            && c->candidate_buffer_slope > (c->baseline_buffer_slope + after_slope) / 2 + .01;
        // Fewer requests at no loss of capacity cannot have cost the consumer
        // anything, whatever an already empty buffer did meanwhile.
        bool recovered = !c->consuming || !o->consumer_wanted || c->trial_buffer_seconds >= 8
            || c->candidate_buffer_gain > 0 || deficit_improved
            || (fewer && c->candidate_capacity >= reference * .95);
        // Fewer requests are cheaper for both ends. They are kept when they
        // still carry the media with headroom, or, short of that, when they
        // lose no more than noise.
        bool accepted = spread <= .15 && recovered
            && (c->candidate_capacity >= reference * (1 + margin)
                || (fewer && (adequate || c->candidate_capacity >= reference * .95)));
        int candidate = c->candidate;
        if (spread > .15) {
            retire(c, MP_CURL_REASON_INCONCLUSIVE);
            // An unsettled path is no reason to keep starving playback with
            // the same downward trial; wait for it to change, as on rejection.
            if (fewer) {
                c->counts[candidate - 1].rejected_ns = o->now_ns;
                c->counts[candidate - 1].rejected_capacity = reference;
            }
        } else {
            retire(c, accepted ? MP_CURL_REASON_ACCEPTED : MP_CURL_REASON_REJECTED);
            c->target_requests = accepted ? candidate : c->baseline;
            // Only complete, stable A/B/A observations enter reusable evidence.
            struct mp_curl_observation baseline = *o;
            baseline.capacity = reference;
            record(c, &baseline);
            struct mp_curl_observation trial = *o;
            trial.requests = candidate;
            trial.profile = c->candidate_profile;
            trial.capacity = c->candidate_capacity;
            record(c, &trial);
            // The count that lost is not tried again until the path changes:
            // a rejected candidate, or the count a downshift moved away from.
            // A downshift also rules out the counts it skipped.
            int first = accepted ? (fewer ? candidate + 1 : 0) : candidate;
            int last = accepted ? (fewer ? c->baseline : 0) : candidate;
            for (int n = first; n && n <= last; n++) {
                struct mp_curl_count_evidence *e = &c->counts[n - 1];
                e->rejected_ns = o->now_ns;
                e->rejected_capacity = accepted ? c->candidate_capacity : reference;
            }
        }
        c->last_trial_ns = o->now_ns;
        return c->target_requests;
    }
    c->action = MP_CURL_HOLD;
    c->target_requests = selected;
    double previous = c->previous_capacity;
    record(c, o);
    c->previous_capacity = o->capacity;
    if (o->fixed_requests) {
        c->reason = MP_CURL_REASON_FIXED;
        return c->target_requests = bounded_count(o->fixed_requests, o->max_requests);
    }
    bool recovery = c->urgency >= recovery_urgency || (c->negative_since_ns && o->now_ns - c->negative_since_ns >= SECOND)
        || (c->deficit_since_ns && o->now_ns - c->deficit_since_ns >= SECOND);
    int best = mp_curl_controller_best(c, o->max_requests);
    if (recovery && best != selected && c->counts[best - 1].playback_samples
        && c->counts[best - 1].capacity > o->capacity / .9) {
        c->target_requests = best;
        c->action = MP_CURL_RESTORE;
        c->recovery_adoption = true;
        c->reason = MP_CURL_REASON_ACCEPTED;
        return best;
    }
    int candidate = selected;
    int reason = MP_CURL_REASON_NONE;
    bool short_of_demand = recovery && o->demand_bps > 0 && o->capacity < 1.75 * o->demand_bps / 8;
    // Head-of-line waits grow with range size on long paths. Short of demand
    // they are the price of throughput, not a reason to shed requests.
    bool hol_pressure = o->hol_ratio > .2 && o->reorder_growing && !short_of_demand;
    bool pressure = hol_pressure
        || (o->worker_cpu_available && o->worker_cpu >= .85 && previous > 0 && o->capacity < previous * .95);
    bool more = short_of_demand && o->network_wait_available
        && o->network_wait_ratio >= .5 && o->storage_ratio < .05 && o->hol_ratio < (short_of_demand ? .5 : .1)
        && (!o->worker_cpu_available || o->worker_cpu < .7) && selected < o->max_requests
        && !rejected(&c->counts[selected], o);
    // Short of demand with no useful step up, a learned count must still be
    // able to decay: on a contended or serialising origin fewer requests do
    // as well or better. Two seconds between trials let a descent finish
    // within a short playback; rejected counts stay rejected.
    bool fewer = short_of_demand && selected > 1 && !rejected(&c->counts[selected / 2 - 1], o)
        && (!c->last_trial_ns || o->now_ns - c->last_trial_ns >= 2 * SECOND);
    if (pressure && selected > 1) {
        candidate--;
        reason = hol_pressure ? MP_CURL_REASON_HOL : MP_CURL_REASON_CPU;
        recovery = true;
    } else if (more) {
        candidate++;
        reason = MP_CURL_REASON_DEMAND;
    } else if (fewer) {
        // Halve rather than step: a starving consumer cannot wait out one
        // trial per request. A rejected half is then retried no sooner than
        // any other rejected count.
        candidate = selected / 2;
        reason = MP_CURL_REASON_DEMAND;
    } else if (o->explore && c->stable_since_ns && o->now_ns - c->stable_since_ns >= 8 * SECOND
        && (!c->last_trial_ns || o->now_ns - c->last_trial_ns >= 30 * SECOND)) {
        if (selected > 1 && !rejected(&c->counts[selected - 2], o))
            candidate--;
        else if (selected < o->max_requests && !rejected(&c->counts[selected], o))
            candidate++;
        reason = MP_CURL_REASON_EXPLORE;
    }
    if (candidate == selected || rejected(&c->counts[candidate - 1], o))
        return selected;
    // A trial while playback runs drains with the consumer, which is what a
    // full ring of buffered ranges needs: its descriptors free as it plays.
    bool playing = !o->paused && !o->eof && o->buffer_valid;
    if (!shape(c, o, candidate, recovery || playing, playing && !recovery)) {
        c->action = MP_CURL_DEFER;
        return selected;
    }
    c->baseline = selected;
    c->candidate = candidate;
    c->baseline_comparable = o->profile.range_bytes == c->trial_range_bytes;
    c->experiment = true;
    c->recovery_adoption = false;
    c->reason = reason;
    c->phase_start = o->next_offset;
    c->trial_buffer_seconds = o->buffer_seconds;
    c->phase_buffer_seconds = o->buffer_seconds;
    c->candidate_buffer_gain = 0;
    // Twice the expected time of three batches. A trial while playback waits
    // already has nothing buffered to protect, so a slow path gets longer.
    // The ranges already in flight ahead of the trial count as well: its
    // first phase cannot complete before they do.
    double queued = (double)o->requests * o->profile.range_bytes;
    // With a full buffer the phases advance only as playback consumes them.
    double pace = fmax(o->capacity, 1);
    if (!recovery && o->demand_bps > 0)
        pace = fmin(pace, o->demand_bps / 8);
    // Each phase also waits for its new requests' first bytes, which on a
    // far or slow-to-answer server is seconds the byte count cannot show.
    double setup = fmax(0, o->ttfb_ms) / 1000 + 2 * fmax(0, o->rtt_ms) / 1000;
    double deadline = clamp(2 * (3 * c->batch_bytes + queued) / pace + 6 * setup, 2, recovery ? 16 : 30);
    // Until its first phase starts, the trial may wait out one more deadline
    // for the requests already in flight to drain.
    c->trial_duration_ns = (int64_t)(deadline * SECOND);
    c->trial_clock = false;
    c->completion_deadline_ns = o->now_ns + 2 * c->trial_duration_ns;
    // The safety deadline protects buffered playback. With nothing buffered
    // there is no runway left to spend, and a trial is the way out.
    double safety = isfinite(c->runway_seconds) && o->buffer_seconds >= 1
        ? fmax(.25, fmin(deadline, c->runway_seconds / 2)) : deadline;
    c->safety_deadline_ns = o->now_ns + (int64_t)(safety * SECOND);
    c->last_trial_ns = o->now_ns;
    phase(c, MP_CURL_BASELINE_BEFORE, selected);
    return selected;
}
