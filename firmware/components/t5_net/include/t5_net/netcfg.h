/* SPDX-License-Identifier: Apache-2.0
 * Saved networks and connection policy: pure logic, host-tested (plan 3.3).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_net/net.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_NETCFG_MAX 8
#define T5_NETCFG_VERSION 1
#define T5_NETCFG_MIN_RSSI (-88)
#define T5_NETCFG_BACKOFF_MIN_MS 5000u
#define T5_NETCFG_BACKOFF_MAX_MS (5u * 60 * 1000)

typedef struct {
    char ssid[T5_NET_SSID_LEN];
    char pass[T5_NET_PASS_LEN];
    uint32_t last_ok;          /* sequence number of the last success, 0 = never */
} t5_netcfg_entry_t;

typedef struct {
    uint16_t version;
    uint8_t n;
    uint8_t flags;             /* T5_NETCFG_FLAG_* */
    uint8_t antenna;
    uint8_t reserved[3];
    uint32_t seq;
    t5_netcfg_entry_t e[T5_NETCFG_MAX];
} t5_netcfg_t;

#define T5_NETCFG_FLAG_DISABLED 0x01   /* radio off by the user */

void t5_netcfg_init(t5_netcfg_t *c);
bool t5_netcfg_valid(const t5_netcfg_t *c);
int t5_netcfg_find(const t5_netcfg_t *c, const char *ssid);
/* Add or update; the oldest-used entry goes when full. Returns the index. */
int t5_netcfg_add(t5_netcfg_t *c, const char *ssid, const char *pass);
bool t5_netcfg_remove(t5_netcfg_t *c, const char *ssid);
/* Record a successful connection (moves it up the order). */
void t5_netcfg_mark_ok(t5_netcfg_t *c, const char *ssid);

/* Per-network failures and backoff, kept in RAM only. */
typedef struct {
    char ssid[T5_NET_SSID_LEN];
    uint8_t failures;
    uint32_t retry_at_ms;
} t5_netcfg_fail_t;

typedef struct {
    t5_netcfg_fail_t f[T5_NETCFG_MAX];
} t5_netcfg_backoff_t;

uint32_t t5_netcfg_backoff_ms(uint8_t failures);
void t5_netcfg_fail(t5_netcfg_backoff_t *b, const char *ssid, uint32_t now_ms);
void t5_netcfg_clear_fail(t5_netcfg_backoff_t *b, const char *ssid);
/* Earliest time any saved network may be tried again (now if one is free). */
uint32_t t5_netcfg_next_retry(const t5_netcfg_backoff_t *b, uint32_t now_ms);

/* From scan results, the saved network to try: strongest usable signal, a
 * bonus for the most recently successful, skipping ones in backoff. -1 if
 * none. Returns the index into c->e; *ap_idx gets the scan entry. */
int t5_netcfg_pick(const t5_netcfg_t *c, const t5_netcfg_backoff_t *b, const t5_net_ap_t *aps, int n,
                   uint32_t now_ms, int *ap_idx);

#ifdef __cplusplus
}
#endif
