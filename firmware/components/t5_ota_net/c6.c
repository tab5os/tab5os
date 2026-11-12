/* SPDX-License-Identifier: Apache-2.0
 * Keeping the C6 radio's firmware matched to the OS: see c6.h.
 *
 * Like update.c: a scheduler task in LP RAM looks every 30 s and starts a
 * worker (12 KB of internal stack, for TLS and the flash write) only when
 * there is something to do. The C6 update itself runs on t5_net's task;
 * T5_EV_NET_C6_UPDATE says when it's over.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_app_desc.h"
#include "t5_core/event.h"
#include "t5_core/excl_sys.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_net/download.h"
#include "t5_net/net.h"
#include "t5_ota/ota.h"
#include "t5_ota_net/c6.h"
#include "t5_storage/paths.h"
#include "ota_rule.h"

static const char *TAG = "t5.c6sync";

#define TICK_MS 30000
#define MAX_TRIES 3
#define TRIES_FILE T5_SYS_DIR "/c6sync"
#define WORKER_STACK 12288

static t5_mutex_t s_mx;
static char s_want[40];          /* the C6 version this OS expects */
static char s_msg[96];
static bool s_done;              /* nothing more to do this boot */
static bool s_busy;              /* a worker runs */
static bool s_updating;          /* t5_net is updating the C6 */
static bool s_fetched;           /* this boot already fetched (or tried to) */

__attribute__((format(printf, 1, 2))) static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    t5_mutex_lock(s_mx);
    vsnprintf(s_msg, sizeof s_msg, fmt, ap);
    t5_mutex_unlock(s_mx);
    va_end(ap);
    T5_LOGI(TAG, "%s", s_msg);
}

static int tries_get(void)
{
    char ver[40] = "";
    int n = 0;
    FILE *f = fopen(TRIES_FILE, "r");
    if (!f) return 0;
    if (fscanf(f, "%39s %d", ver, &n) != 2) n = 0;
    fclose(f);
    return strcmp(ver, s_want) ? 0 : n;
}

static void tries_set(int n)
{
    FILE *f = fopen(TRIES_FILE, "w");
    if (!f) return;
    fprintf(f, "%s %d\n", s_want, n);
    fclose(f);
}

/* The release's .t5c6 into c6fw: from the SD card, else from GitHub.
 * T5_ERR_STATE: neither is reachable yet (no card file, no Wi-Fi). */
static t5_err_t fetch(const t5_net_status_t *n, char *err, size_t errlen)
{
    char name[64], path[128];
    t5_ota_c6_file(T5_C6_VERSION, name, sizeof name);
    static const char *const dirs[] = { T5_SD_PATH, T5_SD_PATH "/update" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dirs[i], name);
        struct stat st;
        if (stat(path, &st) == 0) {
            say("installing %s from the SD card", name);
            return t5_ota_c6_install_file(path, T5_OTA_FROM_SETTINGS, false, s_want, err, errlen);
        }
    }
    if (n->state != T5_NET_CONNECTED) return T5_ERR_STATE;
    char url[192];
    t5_ota_c6_url(esp_app_get_description()->version, T5_C6_VERSION, url, sizeof url);
    say("downloading %s", name);
    t5_download_t *dl;
    T5_RETURN_ON_ERR(t5_download_open(url, &dl, NULL, err, errlen));
    t5_err_t e = t5_ota_c6_install_stream(t5_download_read, dl, T5_OTA_FROM_SETTINGS, false, s_want, err, errlen);
    t5_download_close(dl);
    return e;
}

static void worker(void *arg)
{
    (void)arg;
    char running[32];
    t5_err_t re = t5_net_c6_running(running, sizeof running);   /* also reads c6fw again */
    t5_net_status_t n;
    t5_net_status(&n);
    bool stop = false;
    if (re == T5_ERR_STATE) goto out;                       /* the C6 went off: later */
    if (re == T5_OK && !strcmp(running, s_want)) {
        say("the C6 runs %s", s_want);
        stop = true;
        goto out;
    }
    int tries = tries_get();
    if (tries >= MAX_TRIES) {
        say("gave up updating the C6 to %s after 3 tries (ota c6 retry)", s_want);
        stop = true;
    } else if (strcmp(n.c6_image, s_want)) {
        if (s_fetched) {
            say("c6fw doesn't hold %s; nothing to install it from", s_want);
            stop = true;
        } else {
            char err[96] = "";
            t5_err_t e = fetch(&n, err, sizeof err);
            if (e == T5_ERR_STATE) {
                say("waiting for Wi-Fi or an SD card to get %s", s_want);
                goto out;                                   /* not a try: look again next tick */
            }
            s_fetched = true;
            if (e != T5_OK) {
                say("couldn't get %s: %s", s_want, err[0] ? err : t5_err_str(e));
                stop = true;
            }
        }
    }
    if (!stop) {
        tries_set(tries + 1);
        t5_mutex_lock(s_mx);
        s_updating = true;
        t5_mutex_unlock(s_mx);
        say("updating the C6 from %s to %s", running[0] ? running : "?", s_want);
        if (t5_net_c6_update(false) != T5_OK) {
            t5_mutex_lock(s_mx);
            s_updating = false;
            t5_mutex_unlock(s_mx);
        }
    }
out:
    t5_mutex_lock(s_mx);
    if (stop) s_done = true;
    s_busy = false;
    t5_mutex_unlock(s_mx);
}

static void on_update(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id;
    (void)arg;
    int32_t e = data && len >= sizeof(t5_ev_net_t) ? ((const t5_ev_net_t *)data)->err : T5_OK;
    t5_mutex_lock(s_mx);
    bool ours = s_updating;
    s_updating = false;
    t5_mutex_unlock(s_mx);
    if (ours && e != T5_OK) say("the C6 update failed: %s", t5_err_str((t5_err_t)e));
}

static void scheduler(void *arg)
{
    (void)arg;
    for (;;) {
        t5_delay_ms(TICK_MS);
        t5_mutex_lock(s_mx);
        bool idle = !s_done && !s_busy && !s_updating;
        t5_mutex_unlock(s_mx);
        if (!idle || t5_ota_on_trial() || t5_safe_mode()) continue;
        t5_net_status_t n;
        t5_net_status(&n);
        if (!n.c6_version[0]) continue;                     /* the C6 hasn't been up yet */
        if (t5_excl_claimed(T5_EXCL_WIFI) || t5_excl_claimed(T5_EXCL_BT)) continue;
        t5_mutex_lock(s_mx);
        s_busy = true;
        t5_mutex_unlock(s_mx);
        if (t5_task_create("t5_c6sync", worker, NULL, WORKER_STACK, 3, T5_CORE_ANY, NULL) != T5_OK) {
            t5_mutex_lock(s_mx);
            s_busy = false;
            t5_mutex_unlock(s_mx);
        }
    }
}

t5_err_t t5_c6_sync_start(void)
{
    if (!t5_mutex_once(&s_mx)) return T5_ERR_NO_MEM;
    t5_ota_c6_version(T5_C6_VERSION, s_want, sizeof s_want);
    snprintf(s_msg, sizeof s_msg, "not checked yet");
    T5_RETURN_ON_ERR(t5_event_subscribe(T5_EV_NET_C6_UPDATE, on_update, NULL));
    return t5_task_create_lp("t5_c6_sched", scheduler, NULL, 4096, 2, T5_CORE_ANY, NULL);
}

void t5_c6_sync_status(char *out, size_t n)
{
    if (!t5_mutex_once(&s_mx)) {
        snprintf(out, n, "?");
        return;
    }
    t5_mutex_lock(s_mx);
    snprintf(out, n, "%s%s", s_msg, s_updating ? " (in progress)" : "");
    t5_mutex_unlock(s_mx);
}

t5_err_t t5_c6_sync_retry(void)
{
    if (!t5_mutex_once(&s_mx)) return T5_ERR_NO_MEM;
    remove(TRIES_FILE);
    t5_mutex_lock(s_mx);
    s_done = false;
    s_fetched = false;
    snprintf(s_msg, sizeof s_msg, "checking again within 30 s");
    t5_mutex_unlock(s_mx);
    return T5_OK;
}
