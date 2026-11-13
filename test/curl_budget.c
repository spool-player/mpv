#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include "stream/curl_budget.h"
#define MIB (UINT64_C(1) << 20)

int main(void)
{
    struct mp_curl_budget b = {.baseline = 8 * MIB};
    // Nested streams share one baseline, rather than each receiving 8 MiB.
    assert(mp_curl_budget_reserve_raw(&b, 5 * MIB));
    assert(mp_curl_budget_reserve_raw(&b, 3 * MIB));
    assert(!mp_curl_budget_reserve_raw(&b, 1));
    assert(!mp_curl_budget_reserve_socket(&b, 1));
    b.desired_extra = 32 * MIB;
    assert(mp_curl_budget_reserve_socket(&b, 16 * MIB));
    assert(mp_curl_budget_reserve_raw(&b, 16 * MIB));
    assert(!mp_curl_budget_reserve_raw(&b, 1));
    assert(!mp_curl_budget_reserve_socket(&b, 1));
    // A pressure update stops admission immediately; valid buffered bytes
    // remain committed until consumption/retirement physically releases them.
    b.desired_extra = 0;
    assert(mp_curl_budget_committed(&b) == 32 * MIB);
    assert(mp_curl_budget_raw_available(&b) == 0);
    assert(!mp_curl_budget_reserve_raw(&b, 1));
    mp_curl_budget_release_raw(&b, 16 * MIB);
    assert(mp_curl_budget_committed(&b) == 16 * MIB);
    assert(!mp_curl_budget_reserve_socket(&b, 1));
    mp_curl_budget_release_socket(&b, 16 * MIB);
    assert(mp_curl_budget_committed(&b) == 0);
    mp_curl_budget_release_raw(&b, 8 * MIB);
    assert(mp_curl_budget_raw_available(&b) == 8 * MIB);
    // Caller-controlled quantities cannot wrap addition into an admission.
    assert(!mp_curl_budget_reserve_raw(&b, UINT64_MAX));
    assert(!mp_curl_budget_reserve_socket(&b, UINT64_MAX));
    return 0;
}
