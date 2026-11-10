/* SPDX-License-Identifier: Apache-2.0
 * Standby refreshes ([background] standby_refresh_minutes in the manifest):
 * while the standby screen shows, an app that asks for it has its background
 * entry run headless that often, so its card stays current even with another
 * app open under standby.
 *
 * One system task, started when standby shows and ended soon after it goes.
 * It reads the installed apps' manifests once per standby, then looks for a
 * due app once a minute and runs one at a time (t5_apps_run_alarm, bounded
 * by the app's max_seconds). An app is due when neither a run nor its own
 * card update happened within its interval; so one that set its card just
 * before standby isn't run straight away. Skipped: the open app (it runs, and
 * keeps its own card), and a Python app while a Python app is open (one VM).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t5_app/apps.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_pkg/pkg.h"
#include "shell_priv.h"

static const char *TAG = "t5.standby";

#define MAX_APPS 8
#define CHECK_MS 60000
#define STACK_BYTES 8192            /* the console's `app alarm` runs in as much */

typedef struct {
    char id[T5_APP_ID_LEN];
    uint32_t minutes;
    t5_runtime_t runtime;
} cand_t;

/* When each app was last run, kept across standby sessions this boot. */
static struct {
    char id[T5_APP_ID_LEN];
    int64_t at;                     /* t5_time_us */
} s_last[MAX_APPS];

static bool s_on, s_alive;

static int64_t last_run(const char *id)
{
    for (int i = 0; i < MAX_APPS; i++) {
        if (!strcmp(s_last[i].id, id)) return s_last[i].at;
    }
    return 0;
}

static void note_run(const char *id, int64_t at)
{
    int k = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!strcmp(s_last[i].id, id)) {
            k = i;
            break;
        }
        if (s_last[i].at < s_last[k].at) k = i;     /* else the oldest goes */
    }
    snprintf(s_last[k].id, sizeof s_last[k].id, "%s", id);
    s_last[k].at = at;
}

static int candidates(cand_t *out, int max)
{
    t5_pkg_info_t *infos = calloc(T5_APPS_MAX, sizeof *infos);
    if (!infos) return 0;
    int n = 0, k = t5_pkg_list(infos, T5_APPS_MAX);
    for (int i = 0; i < k && n < max; i++) {
        const t5_manifest_t *m = &infos[i].manifest;
        if (!m->background || !m->standby_refresh_min || !(m->perms & T5_PERM_AMBIENT)) continue;
        snprintf(out[n].id, sizeof out[n].id, "%s", m->id);
        out[n].minutes = m->standby_refresh_min;
        out[n].runtime = m->runtime;
        n++;
    }
    free(infos);
    return n;
}

static bool due(const cand_t *c, int64_t now)
{
    int64_t ref = last_run(c->id), card = ambient_set_at(c->id);
    if (card > ref) ref = card;
    return !ref || now - ref >= (int64_t)c->minutes * 60 * 1000000;
}

/* Runs the first due app, if any; true if it ran one. */
static bool run_due(const cand_t *c, int n)
{
    if (t5_apps_safe_mode()) return false;
    t5_manifest_t *fg = malloc(sizeof *fg);
    if (!fg) return false;
    bool open = t5_apps_fg_manifest(fg);
    int64_t now = t5_time_us();
    bool ran = false;
    for (int i = 0; i < n && !ran && __atomic_load_n(&s_on, __ATOMIC_ACQUIRE); i++) {
        if (open && !strcmp(fg->id, c[i].id)) continue;
        if (open && c[i].runtime == T5_RUNTIME_MICROPYTHON && fg->runtime == T5_RUNTIME_MICROPYTHON) continue;
        if (!due(&c[i], now)) continue;
        T5_LOGI(TAG, "refreshing %s", c[i].id);
        t5_err_t e = t5_apps_run_alarm(c[i].id);
        /* Busy: another headless run (the console's) or it just opened. Try
         * again at the next check rather than waiting a whole interval. */
        if (e != T5_ERR_BUSY) note_run(c[i].id, now);
        if (e != T5_OK) T5_LOGW(TAG, "%s: %s", c[i].id, t5_err_str(e));
        ran = e != T5_ERR_BUSY;
    }
    free(fg);
    return ran;
}

static void task(void *arg)
{
    (void)arg;
    cand_t c[MAX_APPS];
    for (;;) {
        int n = candidates(c, MAX_APPS);
        if (n) T5_LOGI(TAG, "%d app(s) refresh their cards in standby", n);
        while (n && __atomic_load_n(&s_on, __ATOMIC_ACQUIRE)) {
            if (run_due(c, n)) continue;            /* another may be due too */
            for (uint32_t t = 0; t < CHECK_MS && __atomic_load_n(&s_on, __ATOMIC_ACQUIRE); t += 1000) {
                t5_delay_ms(1000);
            }
        }
        __atomic_store_n(&s_alive, false, __ATOMIC_RELEASE);
        /* Standby came back while this was ending: carry on, unless a new
         * task was started for it meanwhile. */
        if (!n || !__atomic_load_n(&s_on, __ATOMIC_ACQUIRE) || __atomic_exchange_n(&s_alive, true, __ATOMIC_ACQ_REL)) {
            break;
        }
    }
}

void standby_refresh_set(bool on)
{
    if (!on) {
        __atomic_store_n(&s_on, false, __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_exchange_n(&s_on, true, __ATOMIC_ACQ_REL)) return;      /* already this standby */
    if (__atomic_exchange_n(&s_alive, true, __ATOMIC_ACQ_REL)) return;   /* the last one is still ending */
    /* A system task: the UI task may be working for the open app, whose
     * context a new task would otherwise inherit (and be ended with). */
    void *saved = t5_sys_enter();
    t5_err_t e = t5_task_create("t5_standby", task, NULL, STACK_BYTES, 3, T5_CORE_ANY, NULL);
    t5_sys_leave(saved);
    if (e != T5_OK) {
        T5_LOGW(TAG, "can't start the refresh task: %s", t5_err_str(e));
        __atomic_store_n(&s_alive, false, __ATOMIC_RELEASE);
    }
}
