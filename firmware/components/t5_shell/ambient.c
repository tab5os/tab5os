/* SPDX-License-Identifier: Apache-2.0
 * Standby cards apps set (t5_ambient_set) and the standby setting: a small
 * table by app id, kept in <sys dir>/ambient.bin so cards outlive their
 * app and a restart. Written by the power service's autosave (a system
 * task), never from the app's task. Expiry is wall-clock time when the
 * clock is set, else time since boot (and such cards go at a restart).
 */
#include <stdio.h>
#include <string.h>
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_power/autosave.h"
#include "t5_shell/shell.h"
#include "t5_shell/standby.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/t5_time.h"
#include "shell_priv.h"

static const char *TAG = "t5.shell";

#define CARDS_FILE "ambient.bin"
#define CARDS_VERSION 1
#define MAX_CARDS 8

typedef struct {
    char app[T5_APP_ID_LEN];          /* "" = free */
    t5_ambient_card_t card;
    int64_t set_at;                   /* order: higher is newer */
    int64_t expires;                  /* 0 = never; wall seconds, or -(boot ms) when the clock wasn't set */
} entry_t;

typedef struct {
    uint8_t standby_off;              /* the person turned standby off */
    uint8_t pad[3];
    entry_t e[MAX_CARDS];
} store_t;

static store_t s_st;
static bool s_loaded, s_dirty;
static t5_mutex_t s_mx;

static t5_err_t save(void *arg)
{
    (void)arg;
    if (!s_dirty) return T5_OK;
    char p[96];
    T5_RETURN_ON_ERR(t5_sys_path(p, sizeof p, CARDS_FILE));
    t5_mutex_lock(s_mx);
    store_t copy = s_st;
    /* Expiry by uptime means nothing after a restart: those go now. */
    for (int i = 0; i < MAX_CARDS; i++) {
        if (copy.e[i].expires < 0) memset(&copy.e[i], 0, sizeof copy.e[i]);
    }
    s_dirty = false;
    t5_mutex_unlock(s_mx);
    return t5_storage_blob_write(p, CARDS_VERSION, &copy, sizeof copy);
}

/* Once, before the first use. */
static void load(void)
{
    t5_mutex_t m = t5_mutex_once(&s_mx);
    if (!m) return;
    t5_mutex_lock(m);
    if (!s_loaded) {
        s_loaded = true;
        char p[96];
        size_t len = 0;
        if (t5_sys_path(p, sizeof p, CARDS_FILE) != T5_OK ||
            t5_storage_blob_read(p, CARDS_VERSION, &s_st, sizeof s_st, &len) != T5_OK || len != sizeof s_st) {
            memset(&s_st, 0, sizeof s_st);
        }
        T5_LOG_ON_ERR(TAG, t5_autosave_register("ambient", save, NULL));
    }
    t5_mutex_unlock(m);
}

static bool expired(const entry_t *e)
{
    if (!e->expires) return false;
    if (e->expires < 0) return (int64_t)t5_time_ms() > -e->expires;
    return t5_time_valid() && (int64_t)t5_time_now() > e->expires;
}

/* The calling app's id, or NULL for system callers. */
static const char *caller(void)
{
    const t5_app_ctx_hdr_t *h = t5_app_ctx_get();
    return h && h->magic == T5_APP_CTX_MAGIC ? h->id : NULL;
}

t5_err_t t5_ambient_set(const t5_ambient_card_t *card)
{
    if (!card) return T5_ERR_INVALID_ARG;
    const char *id = caller();
    if (!id) return T5_ERR_STATE;
    load();
    t5_mutex_lock(s_mx);
    int at = -1, oldest = 0;
    for (int i = 0; i < MAX_CARDS; i++) {
        if (!strcmp(s_st.e[i].app, id)) at = i;
        if (s_st.e[i].set_at < s_st.e[oldest].set_at) oldest = i;
    }
    for (int i = 0; at < 0 && i < MAX_CARDS; i++) {
        if (!s_st.e[i].app[0] || expired(&s_st.e[i])) at = i;
    }
    if (at < 0) at = oldest;                  /* full: the least recently set goes */
    entry_t *e = &s_st.e[at];
    memset(e, 0, sizeof *e);
    snprintf(e->app, sizeof e->app, "%s", id);
    e->card = *card;
    e->card.icon[sizeof e->card.icon - 1] = 0;
    e->card.title[sizeof e->card.title - 1] = 0;
    e->card.value[sizeof e->card.value - 1] = 0;
    e->card.detail[sizeof e->card.detail - 1] = 0;
    e->set_at = t5_time_us();
    if (card->expires_s) {
        e->expires = t5_time_valid() ? (int64_t)t5_time_now() + card->expires_s
                                     : -((int64_t)t5_time_ms() + (int64_t)card->expires_s * 1000);
    }
    s_dirty = true;
    t5_mutex_unlock(s_mx);
    return T5_OK;
}

t5_err_t t5_ambient_clear(void)
{
    const char *id = caller();
    if (!id) return T5_ERR_STATE;
    load();
    t5_mutex_lock(s_mx);
    for (int i = 0; i < MAX_CARDS; i++) {
        if (!strcmp(s_st.e[i].app, id)) {
            memset(&s_st.e[i], 0, sizeof s_st.e[i]);
            s_dirty = true;
        }
    }
    t5_mutex_unlock(s_mx);
    return T5_OK;
}

int ambient_list(shell_card_t *out, int max, bool (*name)(const char *id, char *buf, size_t n))
{
    load();
    entry_t copy[MAX_CARDS];
    t5_mutex_lock(s_mx);
    memcpy(copy, s_st.e, sizeof copy);
    t5_mutex_unlock(s_mx);
    int n = 0;
    while (n < max) {                         /* newest first: pick the next newest each pass */
        int best = -1;
        for (int i = 0; i < MAX_CARDS; i++) {
            if (!copy[i].app[0] || expired(&copy[i])) continue;
            if (best < 0 || copy[i].set_at > copy[best].set_at) best = i;
        }
        if (best < 0) break;
        shell_card_t *c = &out[n];
        memset(c, 0, sizeof *c);
        if (!name(copy[best].app, c->who, sizeof c->who)) {
            copy[best].app[0] = 0;            /* removed since: skip */
            continue;
        }
        n++;
        snprintf(c->icon, sizeof c->icon, "%s", copy[best].card.icon);
        snprintf(c->title, sizeof c->title, "%s", copy[best].card.title);
        snprintf(c->value, sizeof c->value, "%s", copy[best].card.value);
        snprintf(c->detail, sizeof c->detail, "%s", copy[best].card.detail);
        copy[best].app[0] = 0;
    }
    return n;
}

int64_t ambient_set_at(const char *id)
{
    load();
    int64_t at = 0;
    t5_mutex_lock(s_mx);
    for (int i = 0; i < MAX_CARDS; i++) {
        if (!strcmp(s_st.e[i].app, id)) at = s_st.e[i].set_at;
    }
    t5_mutex_unlock(s_mx);
    /* A time from before this boot (set_at counts from boot) says nothing. */
    return at > 0 && at <= t5_time_us() ? at : 0;
}

bool t5_shell_standby_enabled(void)
{
    load();
    return !s_st.standby_off;
}

void t5_shell_set_standby_enabled(bool on)
{
    load();
    t5_mutex_lock(s_mx);
    s_st.standby_off = !on;
    s_dirty = true;
    t5_mutex_unlock(s_mx);
    if (!on) T5_IGNORE(t5_shell_standby(false));   /* not shown, or it goes: nothing to report */
}
