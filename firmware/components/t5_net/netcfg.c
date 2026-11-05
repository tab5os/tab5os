/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_net/netcfg.h"

void t5_netcfg_init(t5_netcfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->version = T5_NETCFG_VERSION;
}

bool t5_netcfg_valid(const t5_netcfg_t *c)
{
    if (c->version != T5_NETCFG_VERSION || c->n > T5_NETCFG_MAX) return false;
    for (int i = 0; i < c->n; i++) {
        if (!c->e[i].ssid[0] || memchr(c->e[i].ssid, 0, T5_NET_SSID_LEN) == NULL ||
            memchr(c->e[i].pass, 0, T5_NET_PASS_LEN) == NULL) {
            return false;
        }
    }
    return true;
}

int t5_netcfg_find(const t5_netcfg_t *c, const char *ssid)
{
    for (int i = 0; i < c->n; i++) {
        if (!strncmp(c->e[i].ssid, ssid, T5_NET_SSID_LEN)) return i;
    }
    return -1;
}

int t5_netcfg_add(t5_netcfg_t *c, const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= T5_NET_SSID_LEN || (pass && strlen(pass) >= T5_NET_PASS_LEN)) {
        return -1;
    }
    int i = t5_netcfg_find(c, ssid);
    if (i < 0) {
        if (c->n < T5_NETCFG_MAX) {
            i = c->n++;
        } else {
            /* Replace the one used longest ago (never-used counts as oldest). */
            i = 0;
            for (int k = 1; k < c->n; k++) {
                if (c->e[k].last_ok < c->e[i].last_ok) i = k;
            }
        }
        memset(&c->e[i], 0, sizeof c->e[i]);
        strcpy(c->e[i].ssid, ssid);
    }
    memset(c->e[i].pass, 0, sizeof c->e[i].pass);
    if (pass) strcpy(c->e[i].pass, pass);
    return i;
}

bool t5_netcfg_remove(t5_netcfg_t *c, const char *ssid)
{
    int i = t5_netcfg_find(c, ssid);
    if (i < 0) return false;
    memmove(&c->e[i], &c->e[i + 1], (size_t)(c->n - i - 1) * sizeof c->e[0]);
    c->n--;
    memset(&c->e[c->n], 0, sizeof c->e[0]);
    return true;
}

void t5_netcfg_mark_ok(t5_netcfg_t *c, const char *ssid)
{
    int i = t5_netcfg_find(c, ssid);
    if (i >= 0) c->e[i].last_ok = ++c->seq;
}

uint32_t t5_netcfg_backoff_ms(uint8_t failures)
{
    if (!failures) return 0;
    uint32_t ms = T5_NETCFG_BACKOFF_MIN_MS;
    for (int i = 1; i < failures && ms < T5_NETCFG_BACKOFF_MAX_MS; i++) ms *= 2;
    return ms > T5_NETCFG_BACKOFF_MAX_MS ? T5_NETCFG_BACKOFF_MAX_MS : ms;
}

static t5_netcfg_fail_t *fail_slot(t5_netcfg_backoff_t *b, const char *ssid, bool create)
{
    t5_netcfg_fail_t *free_slot = NULL;
    for (int i = 0; i < T5_NETCFG_MAX; i++) {
        if (b->f[i].ssid[0] && !strncmp(b->f[i].ssid, ssid, T5_NET_SSID_LEN)) return &b->f[i];
        if (!b->f[i].ssid[0] && !free_slot) free_slot = &b->f[i];
    }
    if (!create) return NULL;
    if (!free_slot) free_slot = &b->f[0];
    memset(free_slot, 0, sizeof *free_slot);
    strncpy(free_slot->ssid, ssid, T5_NET_SSID_LEN - 1);
    return free_slot;
}

void t5_netcfg_fail(t5_netcfg_backoff_t *b, const char *ssid, uint32_t now_ms)
{
    t5_netcfg_fail_t *f = fail_slot(b, ssid, true);
    if (f->failures < 255) f->failures++;
    f->retry_at_ms = now_ms + t5_netcfg_backoff_ms(f->failures);
}

void t5_netcfg_clear_fail(t5_netcfg_backoff_t *b, const char *ssid)
{
    t5_netcfg_fail_t *f = fail_slot(b, ssid, false);
    if (f) memset(f, 0, sizeof *f);
}

static bool in_backoff(const t5_netcfg_backoff_t *b, const char *ssid, uint32_t now_ms)
{
    for (int i = 0; i < T5_NETCFG_MAX; i++) {
        if (b->f[i].ssid[0] && !strncmp(b->f[i].ssid, ssid, T5_NET_SSID_LEN)) {
            return (int32_t)(now_ms - b->f[i].retry_at_ms) < 0;
        }
    }
    return false;
}

uint32_t t5_netcfg_next_retry(const t5_netcfg_backoff_t *b, uint32_t now_ms)
{
    uint32_t best = 0;
    bool any = false;
    for (int i = 0; i < T5_NETCFG_MAX; i++) {
        if (!b->f[i].ssid[0]) continue;
        uint32_t at = b->f[i].retry_at_ms;
        if ((int32_t)(at - now_ms) <= 0) return now_ms;
        if (!any || (int32_t)(at - best) < 0) best = at;
        any = true;
    }
    return any ? best : now_ms;
}

int t5_netcfg_pick(const t5_netcfg_t *c, const t5_netcfg_backoff_t *b, const t5_net_ap_t *aps, int n,
                   uint32_t now_ms, int *ap_idx)
{
    int best = -1, best_ap = -1, best_score = -1000;
    uint32_t newest = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->e[i].last_ok > newest) newest = c->e[i].last_ok;
    }
    for (int a = 0; a < n; a++) {
        if (aps[a].rssi < T5_NETCFG_MIN_RSSI) continue;
        int i = t5_netcfg_find(c, aps[a].ssid);
        if (i < 0 || (b && in_backoff(b, c->e[i].ssid, now_ms))) continue;
        int score = aps[a].rssi + (newest && c->e[i].last_ok == newest ? 10 : 0);
        if (score > best_score) {
            best_score = score;
            best = i;
            best_ap = a;
        }
    }
    if (ap_idx) *ap_idx = best_ap;
    return best;
}
