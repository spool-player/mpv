#pragma once

#include <stdbool.h>
#include <stdint.h>

// All streams and both connection pools in one core share this ledger.
// Kernel charges are conservative reservations; this is not process RSS.
struct mp_curl_budget {
    uint64_t baseline, desired_extra, raw, sockets;
};
uint64_t mp_curl_budget_committed(const struct mp_curl_budget *b);
uint64_t mp_curl_budget_raw_available(const struct mp_curl_budget *b);
bool mp_curl_budget_reserve_raw(struct mp_curl_budget *b, uint64_t bytes);
bool mp_curl_budget_reserve_socket(struct mp_curl_budget *b, uint64_t bytes);
void mp_curl_budget_release_raw(struct mp_curl_budget *b, uint64_t bytes);
void mp_curl_budget_release_socket(struct mp_curl_budget *b, uint64_t bytes);
