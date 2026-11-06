/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_power/rail.h"
#include "priv.h"

static const char *TAG = "t5.rail";

struct t5_rail_hold {
    bool used;
    uint8_t rail;
    uint32_t flags;
    void *app;
    char owner[T5_RAIL_OWNER_LEN];
};

static struct t5_rail_hold s_holds[T5_RAIL_MAX_HOLDS];
static bool s_on[T5_RAIL_COUNT];          /* hardware state */
static bool s_suspended[T5_RAIL_COUNT];   /* dropped for Sleep, holds kept */
static uint32_t s_errors[T5_RAIL_COUNT];
static t5_mutex_t s_lock;

static void lock(void)
{
    if (!s_lock) s_lock = t5_mutex_create();
    t5_mutex_lock(s_lock);
}

static void unlock(void)
{
    t5_mutex_unlock(s_lock);
}

const char *t5_rail_name(t5_rail_t r)
{
    switch (r) {
    case T5_RAIL_USB5V: return "usb5v";
    case T5_RAIL_EXT5V: return "ext5v";
    case T5_RAIL_WLAN: return "wlan";
    case T5_RAIL_SPK: return "spk";
    case T5_RAIL_BACKLIGHT: return "backlight";
    default: return "?";
    }
}

static int count_locked(t5_rail_t r, bool keep_only)
{
    int n = 0;
    for (int i = 0; i < T5_RAIL_MAX_HOLDS; i++) {
        if (s_holds[i].used && s_holds[i].rail == r && (!keep_only || (s_holds[i].flags & T5_RAIL_KEEP_IN_SLEEP))) {
            n++;
        }
    }
    return n;
}

/* Bring the hardware in line with holds and suspension. Lock held. */
static t5_err_t apply(t5_rail_t r)
{
    bool want = count_locked(r, false) > 0 && !s_suspended[r];
    if (want == s_on[r]) {
        return T5_OK;
    }
    t5_err_t err = t5_power_hw_rail_set(r, want);
    if (err != T5_OK) {
        s_errors[r]++;
        T5_LOGE(TAG, "%s %s failed: %s", t5_rail_name(r), want ? "on" : "off", t5_err_str(err));
        return err;
    }
    s_on[r] = want;
    T5_LOGI(TAG, "%s %s", t5_rail_name(r), want ? "on" : "off");
    return T5_OK;
}

static void release_hold(void *h)
{
    t5_rail_release(h);
}

t5_err_t t5_rail_acquire(t5_rail_t rail, const char *owner, uint32_t flags, t5_rail_hold_t **out)
{
    if (rail >= T5_RAIL_COUNT || !out) {
        return T5_ERR_INVALID_ARG;
    }
    lock();
    t5_rail_hold_t *h = NULL;
    for (int i = 0; i < T5_RAIL_MAX_HOLDS; i++) {
        if (!s_holds[i].used) {
            h = &s_holds[i];
            break;
        }
    }
    if (!h) {
        unlock();
        return T5_ERR_NO_MEM;
    }
    *h = (struct t5_rail_hold){ .used = true, .rail = (uint8_t)rail, .flags = flags, .app = t5_app_ctx_get() };
    strncpy(h->owner, owner ? owner : "?", sizeof h->owner - 1);
    t5_err_t err = apply(rail);
    if (err != T5_OK) {
        h->used = false;
        h = NULL;
    }
    unlock();
    *out = h;
    if (h) t5_res_add(T5_RES_RAIL, h, release_hold);
    return err;
}

t5_err_t t5_rail_release(t5_rail_hold_t *h)
{
    if (!h || h < s_holds || h >= s_holds + T5_RAIL_MAX_HOLDS || !h->used) {
        return T5_ERR_INVALID_ARG;
    }
    lock();
    t5_rail_t r = (t5_rail_t)h->rail;
    void *app = h->app;
    h->used = false;
    t5_err_t err = apply(r);
    unlock();
    t5_res_remove_app(app, h);
    return err;
}

bool t5_rail_is_on(t5_rail_t r)
{
    return r < T5_RAIL_COUNT && s_on[r];
}

int t5_rail_count(t5_rail_t r)
{
    if (r >= T5_RAIL_COUNT) return 0;
    lock();
    int n = count_locked(r, false);
    unlock();
    return n;
}

int t5_rail_owners(t5_rail_t r, const char **owners, int max)
{
    int n = 0;
    lock();
    for (int i = 0; i < T5_RAIL_MAX_HOLDS; i++) {
        if (s_holds[i].used && s_holds[i].rail == r) {
            if (n < max) owners[n] = s_holds[i].owner;
            n++;
        }
    }
    unlock();
    return n;
}

uint32_t t5_rail_errors(t5_rail_t r)
{
    return r < T5_RAIL_COUNT ? s_errors[r] : 0;
}

t5_err_t t5_rail_suspend(bool keep_all)
{
    t5_err_t err = T5_OK;
    lock();
    for (int r = 0; r < T5_RAIL_COUNT; r++) {
        /* The backlight follows the display state, not Sleep. */
        if (r == T5_RAIL_BACKLIGHT || keep_all || count_locked((t5_rail_t)r, true)) {
            continue;
        }
        s_suspended[r] = true;
        t5_err_t e = apply((t5_rail_t)r);
        if (e != T5_OK) err = e;
    }
    unlock();
    return err;
}

t5_err_t t5_rail_resume(void)
{
    t5_err_t err = T5_OK;
    lock();
    for (int r = 0; r < T5_RAIL_COUNT; r++) {
        if (!s_suspended[r]) continue;
        s_suspended[r] = false;
        t5_err_t e = apply((t5_rail_t)r);
        if (e != T5_OK) err = e;
    }
    unlock();
    return err;
}

void t5_rail_reset_for_test(void)
{
    memset(s_holds, 0, sizeof s_holds);
    memset(s_on, 0, sizeof s_on);
    memset(s_suspended, 0, sizeof s_suspended);
    memset(s_errors, 0, sizeof s_errors);
}
