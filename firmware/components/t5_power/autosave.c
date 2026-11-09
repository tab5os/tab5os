/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_power/autosave.h"
#include "priv.h"

static const char *TAG = "t5.autosave";

static struct { const char *name; t5_autosave_fn_t fn; void *arg; } s_savers[T5_AUTOSAVE_MAX];
static t5_mutex_t s_mtx;
static uint32_t s_last_ms;
static bool s_started;
static uint32_t s_runs, s_failures;

static void lock(void)
{
    t5_mutex_lock(t5_mutex_once(&s_mtx));
}

t5_err_t t5_autosave_register(const char *name, t5_autosave_fn_t fn, void *arg)
{
    if (!fn) return T5_ERR_INVALID_ARG;
    t5_err_t err = T5_ERR_NO_MEM;
    lock();
    for (int i = 0; i < T5_AUTOSAVE_MAX; i++) {
        if (!s_savers[i].fn) {
            s_savers[i].name = name ? name : "?";
            s_savers[i].fn = fn;
            s_savers[i].arg = arg;
            err = T5_OK;
            break;
        }
    }
    t5_mutex_unlock(s_mtx);
    return err;
}

t5_err_t t5_autosave_now(void)
{
    t5_err_t first = T5_OK;
    lock();
    for (int i = 0; i < T5_AUTOSAVE_MAX; i++) {
        if (!s_savers[i].fn) continue;
        t5_err_t e = s_savers[i].fn(s_savers[i].arg);
        if (e != T5_OK) {
            T5_LOGW(TAG, "%s: %s", s_savers[i].name, t5_err_str(e));
            if (first == T5_OK) first = e;
        }
    }
    s_runs++;
    if (first != T5_OK) s_failures++;
    s_last_ms = t5_time_ms();
    s_started = true;
    t5_mutex_unlock(s_mtx);
    return first;
}

void t5_autosave_poll(uint32_t now_ms)
{
    if (!s_started) {
        s_started = true;
        s_last_ms = now_ms;
        return;
    }
    if (now_ms - s_last_ms >= T5_AUTOSAVE_PERIOD_MS) {
        T5_LOG_ON_ERR(TAG, t5_autosave_now());
    }
}

uint32_t t5_autosave_runs(void) { return s_runs; }
uint32_t t5_autosave_failures(void) { return s_failures; }

void t5_autosave_reset_for_test(void)
{
    memset(s_savers, 0, sizeof s_savers);
    s_started = false;
    s_last_ms = 0;
    s_runs = s_failures = 0;
}
