/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "priv.h"

static const char *TAG = "t5.lock";

struct t5_wakelock {
    bool used;
    uint8_t type;
    uint32_t t0_ms;
    uint32_t timeout_ms;     /* 0 = none */
    void *app;
    char owner[T5_WAKELOCK_OWNER_LEN];
};

static struct t5_wakelock s_locks[T5_WAKELOCK_MAX];
static t5_mutex_t s_mtx;

static void lock(void)
{
    if (!s_mtx) s_mtx = t5_mutex_create();
    t5_mutex_lock(s_mtx);
}

const char *t5_lock_type_name(t5_lock_type_t t)
{
    switch (t) {
    case T5_LOCK_SCREEN: return "screen";
    case T5_LOCK_CPU: return "cpu";
    case T5_LOCK_AUDIO: return "audio";
    case T5_LOCK_NET: return "net";
    case T5_LOCK_RAIL: return "rail";
    default: return "?";
    }
}

static void release_lock(void *l)
{
    t5_wakelock_release(l);
}

t5_wakelock_t *t5_wakelock_acquire_timeout(t5_lock_type_t type, const char *owner, uint32_t timeout_ms)
{
    if (type >= T5_LOCK_TYPE_COUNT) {
        return NULL;
    }
    t5_wakelock_t *l = NULL;
    lock();
    for (int i = 0; i < T5_WAKELOCK_MAX; i++) {
        if (!s_locks[i].used) {
            l = &s_locks[i];
            *l = (struct t5_wakelock){ .used = true, .type = (uint8_t)type, .t0_ms = t5_time_ms(),
                                       .timeout_ms = timeout_ms, .app = t5_app_ctx_get() };
            strncpy(l->owner, owner ? owner : "?", sizeof l->owner - 1);
            break;
        }
    }
    /* Tracked before the table lock is released: an exiting app's task can't
     * be stopped while it holds a lock, so it can't be left with an untracked
     * slot (t5_app quiesce). */
    if (l) t5_res_add(T5_RES_LOCK, l, release_lock);
    t5_mutex_unlock(s_mtx);
    if (!l) {
        T5_LOGE(TAG, "table full, %s lock for %s refused", t5_lock_type_name(type), owner ? owner : "?");
        return NULL;
    }
    /* A new lock can only make the state shallower: re-arbitrate now. */
    t5_power_hw_notify(false);
    return l;
}

t5_wakelock_t *t5_wakelock_acquire(t5_lock_type_t type, const char *owner)
{
    return t5_wakelock_acquire_timeout(type, owner, 0);
}

void t5_wakelock_release(t5_wakelock_t *l)
{
    if (!l || l < s_locks || l >= s_locks + T5_WAKELOCK_MAX) {
        return;
    }
    lock();
    if (!l->used) {
        /* Already released (its timeout ran out): a second release must not
         * free a slot someone else now holds. */
        t5_mutex_unlock(s_mtx);
        return;
    }
    /* Untracked before the slot is free: once free it may be someone else's. */
    t5_res_remove_app(l->app, l);
    l->used = false;
    t5_mutex_unlock(s_mtx);
    t5_power_hw_notify(false);
}

int t5_wakelock_count(t5_lock_type_t type)
{
    int n = 0;
    lock();
    for (int i = 0; i < T5_WAKELOCK_MAX; i++) {
        n += s_locks[i].used && s_locks[i].type == type;
    }
    t5_mutex_unlock(s_mtx);
    return n;
}

void t5_wakelock_counts(uint16_t out[T5_LOCK_TYPE_COUNT])
{
    memset(out, 0, sizeof(uint16_t) * T5_LOCK_TYPE_COUNT);
    lock();
    for (int i = 0; i < T5_WAKELOCK_MAX; i++) {
        if (!s_locks[i].used) continue;
        T5_ASSERT(s_locks[i].type < T5_LOCK_TYPE_COUNT);   /* checked at acquire */
        out[s_locks[i].type]++;
    }
    t5_mutex_unlock(s_mtx);
}

int t5_wakelock_expire(uint32_t now_ms)
{
    int n = 0;
    lock();
    for (int i = 0; i < T5_WAKELOCK_MAX; i++) {
        struct t5_wakelock *l = &s_locks[i];
        if (l->used && l->timeout_ms && now_ms - l->t0_ms >= l->timeout_ms) {
            T5_LOGD(TAG, "%s lock for %s timed out", t5_lock_type_name((t5_lock_type_t)l->type), l->owner);
            /* Untracked before the slot is free (lock order: ours, then the tracker's). */
            t5_res_remove_app(l->app, l);
            l->used = false;
            n++;
        }
    }
    t5_mutex_unlock(s_mtx);
    return n;
}

int t5_wakelock_list(t5_wakelock_info_t *out, int max)
{
    int n = 0;
    uint32_t now = t5_time_ms();
    lock();
    for (int i = 0; i < T5_WAKELOCK_MAX; i++) {
        const struct t5_wakelock *l = &s_locks[i];
        if (!l->used) continue;
        if (n < max) {
            uint32_t age = now - l->t0_ms;
            out[n] = (t5_wakelock_info_t){ .type = (t5_lock_type_t)l->type, .age_ms = age, .app = l->app,
                                           .left_ms = l->timeout_ms ? (age < l->timeout_ms ? l->timeout_ms - age : 0) : 0 };
            memcpy(out[n].owner, l->owner, sizeof l->owner);
        }
        n++;
    }
    t5_mutex_unlock(s_mtx);
    return n;
}

void t5_wakelock_reset_for_test(void)
{
    memset(s_locks, 0, sizeof s_locks);
}
