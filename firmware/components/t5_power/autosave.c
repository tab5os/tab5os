/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_power/autosave.h"
#include "t5_storage/t5_storage.h"
#include "priv.h"

static const char *TAG = "t5.autosave";

#define SESSION_VERSION 1

static struct { const char *name; t5_autosave_fn_t fn; void *arg; } s_savers[T5_AUTOSAVE_MAX];
static t5_mutex_t s_mtx;
static uint32_t s_last_ms;
static bool s_started;
static uint32_t s_runs, s_failures;
static char s_dir[64] = T5_SYS_DIR;

static uint8_t *s_session;          /* pending, to be written */
static size_t s_session_len;
static bool s_session_dirty;
static uint8_t *s_restored;         /* what boot found */
static size_t s_restored_len;

static void lock(void)
{
    if (!s_mtx) s_mtx = t5_mutex_create();
    t5_mutex_lock(s_mtx);
}

void t5_autosave_set_dir(const char *dir)
{
    strncpy(s_dir, dir, sizeof s_dir - 1);
}

const char *t5_autosave_dir(void)
{
    return s_dir;
}

static void ensure_dir(void)
{
    mkdir(s_dir, 0755);   /* EEXIST is fine */
}

static t5_err_t session_path(char *out, size_t cap)
{
    return snprintf(out, cap, "%s/session.bin", s_dir) < (int)cap ? T5_OK : T5_ERR_INVALID_ARG;
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

static t5_err_t save_session(void)
{
    if (!s_session_dirty) return T5_OK;
    char path[96];
    T5_RETURN_ON_ERR(session_path(path, sizeof path));
    ensure_dir();
    t5_err_t err = t5_storage_blob_write(path, SESSION_VERSION, s_session, s_session_len);
    if (err == T5_OK) s_session_dirty = false;
    return err;
}

t5_err_t t5_autosave_now(void)
{
    t5_err_t first = T5_OK;
    lock();
    ensure_dir();
    t5_err_t e = save_session();
    if (e != T5_OK) {
        first = e;
        T5_LOGW(TAG, "session: %s", t5_err_str(e));
    }
    for (int i = 0; i < T5_AUTOSAVE_MAX; i++) {
        if (!s_savers[i].fn) continue;
        e = s_savers[i].fn(s_savers[i].arg);
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
        t5_autosave_now();
    }
}

uint32_t t5_autosave_runs(void) { return s_runs; }
uint32_t t5_autosave_failures(void) { return s_failures; }

t5_err_t t5_session_set(const void *data, size_t len)
{
    if (len > T5_SESSION_MAX || (len && !data)) return T5_ERR_INVALID_ARG;
    lock();
    if (!s_session) s_session = malloc(T5_SESSION_MAX);
    t5_err_t err = T5_ERR_NO_MEM;
    if (s_session) {
        if (len != s_session_len || memcmp(s_session, data, len)) {
            memcpy(s_session, data, len);
            s_session_len = len;
            s_session_dirty = true;
        }
        err = T5_OK;
    }
    t5_mutex_unlock(s_mtx);
    return err;
}

t5_err_t t5_session_get(void *buf, size_t cap, size_t *len)
{
    if (!s_restored) return T5_ERR_NOT_FOUND;
    if (cap < s_restored_len) return T5_ERR_NO_MEM;
    memcpy(buf, s_restored, s_restored_len);
    if (len) *len = s_restored_len;
    return T5_OK;
}

t5_err_t t5_session_load(void)
{
    char path[96];
    T5_RETURN_ON_ERR(session_path(path, sizeof path));
    uint8_t *buf = malloc(T5_SESSION_MAX);
    if (!buf) return T5_ERR_NO_MEM;
    size_t len = 0;
    t5_err_t err = t5_storage_blob_read(path, SESSION_VERSION, buf, T5_SESSION_MAX, &len);
    if (err != T5_OK) {
        free(buf);
        return err;
    }
    lock();
    free(s_restored);
    s_restored = buf;
    s_restored_len = len;
    t5_mutex_unlock(s_mtx);
    return T5_OK;
}

void t5_autosave_reset_for_test(void)
{
    memset(s_savers, 0, sizeof s_savers);
    s_started = false;
    s_last_ms = 0;
    s_runs = s_failures = 0;
    s_session_dirty = false;
    s_session_len = 0;
    free(s_restored);
    s_restored = NULL;
    s_restored_len = 0;
}
