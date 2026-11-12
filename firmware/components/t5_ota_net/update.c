/* SPDX-License-Identifier: Apache-2.0
 * Updates over Wi-Fi: see update.h.
 *
 * A small scheduler task in LP RAM wakes once a minute and, when a check is
 * due and Wi-Fi is connected, starts a worker. The worker (12 KB of internal
 * stack: TLS needs it, and the install writes flash) does one check or one
 * install and ends, so the RAM is only taken while one runs.
 *
 * The check reads the GitHub releases list into PSRAM and picks the newest
 * update newer than this system (t5_ota_pick_release). Pre-releases count.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_net/download.h"
#include "t5_net/net.h"
#include "t5_ota/ota.h"
#include "t5_ota_net/ota_url.h"
#include "t5_ota_net/update.h"
#include "ota_rule.h"

static const char *TAG = "t5.update";

#define TICK_MS 60000
#define FIRST_DELAY_US (120LL * 1000000)        /* after Wi-Fi first connects */
#define EVERY_US (24LL * 3600 * 1000000)
#define RETRY_US (3600LL * 1000000)             /* after a failed check */
#define MAX_JSON (256 * 1024)
#define WORKER_STACK 12288

typedef enum { JOB_CHECK, JOB_INSTALL } job_t;

static t5_mutex_t s_mx;
static t5_update_status_t s_st;
static bool s_busy;
static int64_t s_next_us;                       /* when the next check is due, 0 not yet scheduled */
static char s_notified[32];                     /* the last version found() was told about */
static void (*s_found)(const char *version);

static void set_state(t5_update_state_t st, const char *error)
{
    t5_mutex_lock(s_mx);
    s_st.state = st;
    snprintf(s_st.error, sizeof s_st.error, "%s", error ? error : "");
    t5_mutex_unlock(s_mx);
}

/* The releases list, NUL-terminated, or NULL. */
static char *fetch_list(char *err, size_t errlen)
{
    t5_download_t *dl;
    if (t5_download_open(T5_OTA_RELEASES_API, &dl, NULL, err, errlen) != T5_OK) return NULL;
    size_t cap = 32 * 1024, n = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (n + 1 >= cap) {
            if (cap >= MAX_JSON) {
                snprintf(err, errlen, "the release list is too long");
                free(buf);
                buf = NULL;
                break;
            }
            char *more = realloc(buf, cap * 2);
            if (!more) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = more;
            cap *= 2;
        }
        int r = t5_download_read(dl, buf + n, cap - 1 - n);
        if (r < 0) {
            snprintf(err, errlen, "%s", t5_err_str((t5_err_t)r));
            free(buf);
            buf = NULL;
            break;
        }
        if (r == 0) {
            buf[n] = 0;
            break;
        }
        n += (size_t)r;
    }
    if (!buf && !err[0]) snprintf(err, errlen, "out of memory");
    t5_download_close(dl);
    return buf;
}

static void do_check(void)
{
    char err[64] = "";
    char *json = fetch_list(err, sizeof err);
    int64_t now = t5_time_us();
    if (!json) {
        T5_LOGW(TAG, "check failed: %s", err);
        t5_mutex_lock(s_mx);
        s_st.state = T5_UPDATE_FAILED;
        snprintf(s_st.error, sizeof s_st.error, "Couldn't check: %s", err);
        s_next_us = now + RETRY_US;
        t5_mutex_unlock(s_mx);
        return;
    }
    char url[sizeof s_st.url], ver[sizeof s_st.version];
    bool found = t5_ota_pick_release(json, esp_app_get_description()->version, url, sizeof url, ver, sizeof ver);
    free(json);
    bool tell = false;
    t5_mutex_lock(s_mx);
    s_st.checked_us = now;
    s_st.error[0] = 0;
    s_st.state = found ? T5_UPDATE_AVAILABLE : T5_UPDATE_CURRENT;
    if (found) {
        snprintf(s_st.version, sizeof s_st.version, "%s", ver);
        snprintf(s_st.url, sizeof s_st.url, "%s", url);
        tell = strcmp(s_notified, ver) != 0;
        if (tell) snprintf(s_notified, sizeof s_notified, "%s", ver);
    }
    s_next_us = now + EVERY_US;
    t5_mutex_unlock(s_mx);
    if (found) T5_LOGI(TAG, "Tab5 OS %s is available", ver);
    else T5_LOGI(TAG, "up to date (%s)", esp_app_get_description()->version);
    if (tell && s_found) s_found(ver);
}

static void do_install(void)
{
    char url[sizeof s_st.url];
    t5_mutex_lock(s_mx);
    snprintf(url, sizeof url, "%s", s_st.url);
    t5_mutex_unlock(s_mx);
    char err[64] = "";
    /* No consent: only an image a built-in key signed, not older than this. */
    t5_err_t e = t5_ota_install_url(url, T5_OTA_FROM_SETTINGS, false, err, sizeof err);
    if (e == T5_OK) {
        set_state(T5_UPDATE_CURRENT, NULL);     /* t5_ota_status() now says "ready (restart)" */
    } else {
        char why[96];
        snprintf(why, sizeof why, "Couldn't install: %s", err[0] ? err : t5_err_str(e));
        T5_LOGW(TAG, "%s", why);
        t5_mutex_lock(s_mx);
        s_st.state = T5_UPDATE_AVAILABLE;       /* still there to try again */
        snprintf(s_st.error, sizeof s_st.error, "%s", why);
        t5_mutex_unlock(s_mx);
    }
}

static void worker(void *arg)
{
    if ((job_t)(intptr_t)arg == JOB_INSTALL) do_install();
    else do_check();
    t5_mutex_lock(s_mx);
    s_busy = false;
    t5_mutex_unlock(s_mx);
}

static t5_err_t run(job_t job)
{
    t5_mutex_t m = t5_mutex_once(&s_mx);
    if (!m) return T5_ERR_NO_MEM;
    t5_mutex_lock(m);
    if (s_busy || (job == JOB_INSTALL && !s_st.url[0])) {
        t5_mutex_unlock(m);
        return T5_ERR_STATE;
    }
    s_busy = true;
    s_st.state = job == JOB_INSTALL ? T5_UPDATE_INSTALLING : T5_UPDATE_CHECKING;
    if (job == JOB_INSTALL) s_st.error[0] = 0;
    t5_mutex_unlock(m);
    t5_err_t e = t5_task_create("t5_update", worker, (void *)(intptr_t)job, WORKER_STACK, 3, T5_CORE_ANY, NULL);
    if (e != T5_OK) {
        t5_mutex_lock(m);
        s_busy = false;
        s_st.state = T5_UPDATE_FAILED;
        snprintf(s_st.error, sizeof s_st.error, "Couldn't start: %s", t5_err_str(e));
        t5_mutex_unlock(m);
    }
    return e;
}

static void scheduler(void *arg)
{
    (void)arg;
    for (;;) {
        t5_delay_ms(TICK_MS);
        t5_net_status_t n;
        t5_net_status(&n);
        if (n.state != T5_NET_CONNECTED) continue;
        int64_t now = t5_time_us();
        t5_mutex_lock(s_mx);
        if (!s_next_us) s_next_us = now + FIRST_DELAY_US;
        bool due = !s_busy && now >= s_next_us;
        t5_mutex_unlock(s_mx);
        if (due) T5_IGNORE(run(JOB_CHECK));      /* not now: tried again next tick */
    }
}

t5_err_t t5_update_start(void (*found)(const char *version))
{
    if (!t5_mutex_once(&s_mx)) return T5_ERR_NO_MEM;
    s_found = found;
    return t5_task_create_lp("t5_upd_sched", scheduler, NULL, 3072, 2, T5_CORE_ANY, NULL);
}

void t5_update_status(t5_update_status_t *out)
{
    if (!t5_mutex_once(&s_mx)) {
        memset(out, 0, sizeof *out);
        return;
    }
    t5_mutex_lock(s_mx);
    *out = s_st;
    t5_mutex_unlock(s_mx);
}

t5_err_t t5_update_check_now(void)
{
    return run(JOB_CHECK);
}

t5_err_t t5_update_install(void)
{
    return run(JOB_INSTALL);
}
