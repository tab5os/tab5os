/* SPDX-License-Identifier: Apache-2.0
 * On disk: a storage blob holding "app:<id>" and its NUL, or nothing.
 */
#include "app_session.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "t5_core/os.h"
#include "t5_pkg/manifest.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"

#define SESSION_FILE "session.bin"
#define SESSION_VERSION 1
#define SESSION_PREFIX "app:"
#define SESSION_MAX (sizeof SESSION_PREFIX + T5_APP_ID_LEN)

static t5_mutex_t s_mx;
static char s_restored[T5_APP_ID_LEN];
static char s_now[T5_APP_ID_LEN];
static bool s_dirty;

void app_session_load(void)
{
    if (!s_mx) s_mx = t5_mutex_create();
    s_restored[0] = 0;
    s_now[0] = 0;
    s_dirty = false;
    char path[96], blob[SESSION_MAX];
    size_t len = 0;
    if (t5_sys_path(path, sizeof path, SESSION_FILE) != T5_OK ||
        t5_storage_blob_read(path, SESSION_VERSION, blob, sizeof blob, &len) != T5_OK) {
        return;   /* none saved yet (first boot), or torn: no session */
    }
    if (len <= strlen(SESSION_PREFIX) || strncmp(blob, SESSION_PREFIX, strlen(SESSION_PREFIX))) return;
    const char *id = blob + strlen(SESSION_PREFIX);
    size_t n = strnlen(id, len - strlen(SESSION_PREFIX));
    if (n >= sizeof s_restored) return;
    memcpy(s_restored, id, n);
    s_restored[n] = 0;
    snprintf(s_now, sizeof s_now, "%s", s_restored);   /* what the file holds */
}

const char *app_session_restored(void)
{
    return s_restored;
}

void app_session_set(const char *id)
{
    if (!s_mx) return;
    if (!id) id = "";
    t5_mutex_lock(s_mx);
    if (strcmp(s_now, id)) {
        snprintf(s_now, sizeof s_now, "%s", id);
        s_dirty = true;
    }
    t5_mutex_unlock(s_mx);
}

t5_err_t app_session_save(void *arg)
{
    (void)arg;
    if (!s_mx) return T5_ERR_STATE;
    char path[96], blob[SESSION_MAX];
    T5_RETURN_ON_ERR(t5_sys_path(path, sizeof path, SESSION_FILE));
    t5_mutex_lock(s_mx);
    bool dirty = s_dirty;
    size_t len = 0;
    if (s_now[0]) len = (size_t)snprintf(blob, sizeof blob, SESSION_PREFIX "%s", s_now) + 1;
    s_dirty = false;
    t5_mutex_unlock(s_mx);
    if (!dirty) return T5_OK;
    t5_err_t e = t5_storage_blob_write(path, SESSION_VERSION, blob, len);
    if (e != T5_OK) {
        t5_mutex_lock(s_mx);
        s_dirty = true;   /* try again next time */
        t5_mutex_unlock(s_mx);
    }
    return e;
}
