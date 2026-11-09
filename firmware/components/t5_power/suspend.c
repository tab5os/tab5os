/* SPDX-License-Identifier: Apache-2.0
 * Ordered suspend and resume hooks with time budgets (plan 2.3).
 */
#include <string.h>
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "priv.h"

static const char *TAG = "t5.suspend";

typedef struct {
    t5_suspend_hook_t h;
    t5_suspend_stats_t st;
    bool used;
    bool suspended;      /* suspend ran (or had nothing to do): resume it */
} slot_t;

static slot_t s_slots[T5_SUSPEND_MAX_HOOKS];
static t5_mutex_t s_mtx;

static void lock(void)
{
    t5_mutex_lock(t5_mutex_once(&s_mtx));
}

t5_err_t t5_power_register_hook(const t5_suspend_hook_t *h)
{
    if (!h || !h->name || (!h->suspend && !h->resume)) {
        return T5_ERR_INVALID_ARG;
    }
    t5_err_t err = T5_ERR_NO_MEM;
    lock();
    for (int i = 0; i < T5_SUSPEND_MAX_HOOKS; i++) {
        if (s_slots[i].used && !strcmp(s_slots[i].h.name, h->name)) {
            err = T5_ERR_BUSY;
            break;
        }
    }
    if (err != T5_ERR_BUSY) {
        for (int i = 0; i < T5_SUSPEND_MAX_HOOKS; i++) {
            if (!s_slots[i].used) {
                s_slots[i] = (slot_t){ .h = *h, .used = true };
                s_slots[i].st.name = h->name;
                s_slots[i].st.prio = h->prio;
                s_slots[i].st.budget_ms = h->budget_ms;
                err = T5_OK;
                break;
            }
        }
    }
    t5_mutex_unlock(s_mtx);
    return err;
}

t5_err_t t5_power_unregister_hook(const char *name)
{
    t5_err_t err = T5_ERR_NOT_FOUND;
    lock();
    for (int i = 0; i < T5_SUSPEND_MAX_HOOKS; i++) {
        if (s_slots[i].used && !strcmp(s_slots[i].h.name, name)) {
            s_slots[i].used = false;
            err = T5_OK;
        }
    }
    t5_mutex_unlock(s_mtx);
    return err;
}

/* Indices of used slots sorted by priority (stable: registration order). */
static int sorted(int idx[T5_SUSPEND_MAX_HOOKS])
{
    int n = 0;
    for (int i = 0; i < T5_SUSPEND_MAX_HOOKS; i++) {
        if (s_slots[i].used) idx[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && s_slots[idx[j]].h.prio > s_slots[v].h.prio) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
    return n;
}

static bool call(slot_t *s, bool suspend)
{
    t5_suspend_fn_t fn = suspend ? s->h.suspend : s->h.resume;
    if (!fn) {
        return true;
    }
    int64_t t0 = t5_time_us();
    t5_err_t err = fn(s->h.arg);
    uint32_t ms = (uint32_t)((t5_time_us() - t0) / 1000);
    if (suspend) s->st.last_suspend_ms = ms;
    else s->st.last_resume_ms = ms;
    if (ms > s->st.max_ms) s->st.max_ms = ms;
    if (s->h.budget_ms && ms > s->h.budget_ms) {
        s->st.overruns++;
        T5_LOGW(TAG, "%s %s took %u ms (budget %u)", s->h.name, suspend ? "suspend" : "resume", (unsigned)ms,
                (unsigned)s->h.budget_ms);
        t5_event_post(T5_EV_POWER_SUSPEND_OVERRUN, NULL, 0);
    }
    if (err != T5_OK) {
        s->st.failures++;
        T5_LOGE(TAG, "%s %s failed: %s", s->h.name, suspend ? "suspend" : "resume", t5_err_str(err));
        return false;
    }
    return true;
}

int t5_suspend_run(int min_prio, uint32_t *ms_out)
{
    int idx[T5_SUSPEND_MAX_HOOKS];
    int failed = 0;
    int64_t t0 = t5_time_us();
    lock();
    int n = sorted(idx);
    for (int i = 0; i < n; i++) {
        slot_t *s = &s_slots[idx[i]];
        if (s->h.prio < min_prio || s->suspended) continue;
        /* A failed suspend still gets its resume: the hook may have half-done
         * its work, and resume must put things back either way. */
        if (!call(s, true)) failed++;
        s->suspended = true;
    }
    t5_mutex_unlock(s_mtx);
    if (ms_out) *ms_out = (uint32_t)((t5_time_us() - t0) / 1000);
    return failed;
}

int t5_resume_run(int min_prio, uint32_t *ms_out)
{
    int idx[T5_SUSPEND_MAX_HOOKS];
    int failed = 0;
    int64_t t0 = t5_time_us();
    lock();
    int n = sorted(idx);
    for (int i = n - 1; i >= 0; i--) {
        slot_t *s = &s_slots[idx[i]];
        if (s->h.prio < min_prio || !s->suspended) continue;
        if (!call(s, false)) failed++;
        s->suspended = false;
    }
    t5_mutex_unlock(s_mtx);
    if (ms_out) *ms_out = (uint32_t)((t5_time_us() - t0) / 1000);
    return failed;
}

int t5_power_hook_stats(t5_suspend_stats_t *out, int max)
{
    int idx[T5_SUSPEND_MAX_HOOKS];
    lock();
    int n = sorted(idx);
    for (int i = 0; i < n && i < max; i++) {
        out[i] = s_slots[idx[i]].st;
    }
    t5_mutex_unlock(s_mtx);
    return n;
}

void t5_suspend_reset_for_test(void)
{
    memset(s_slots, 0, sizeof s_slots);
}
