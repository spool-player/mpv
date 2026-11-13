#include <assert.h>
#include <stdint.h>
#include "curl_budget.h"

static uint64_t extra_raw(const struct mp_curl_budget *b)
{
    return b->raw > b->baseline ? b->raw - b->baseline : 0;
}

uint64_t mp_curl_budget_committed(const struct mp_curl_budget *b)
{
    uint64_t used = extra_raw(b) + b->sockets;
    return used > b->desired_extra ? used : b->desired_extra;
}

uint64_t mp_curl_budget_raw_available(const struct mp_curl_budget *b)
{
    uint64_t extra = b->desired_extra > b->sockets ? b->desired_extra - b->sockets : 0;
    uint64_t total = b->baseline + extra;
    return total > b->raw ? total - b->raw : 0;
}

bool mp_curl_budget_reserve_raw(struct mp_curl_budget *b, uint64_t bytes)
{
    if (bytes > mp_curl_budget_raw_available(b))
        return false;
    b->raw += bytes;
    return true;
}

bool mp_curl_budget_reserve_socket(struct mp_curl_budget *b, uint64_t bytes)
{
    uint64_t used = extra_raw(b) + b->sockets;
    if (used > b->desired_extra || bytes > b->desired_extra - used)
        return false;
    b->sockets += bytes;
    return true;
}

void mp_curl_budget_release_raw(struct mp_curl_budget *b, uint64_t bytes)
{
    assert(bytes <= b->raw);
    b->raw -= bytes;
}

void mp_curl_budget_release_socket(struct mp_curl_budget *b, uint64_t bytes)
{
    assert(bytes <= b->sockets);
    b->sockets -= bytes;
}
