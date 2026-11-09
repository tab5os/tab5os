/* SPDX-License-Identifier: Apache-2.0
 * Ongoing activities apps post (t5_shell_activity_*): a fixed table the
 * status bar and quick settings read through shell_sys_activities(). Each
 * one is recorded as the app's resource, so it ends when the app exits or
 * crashes. Any task.
 */
#include <stdio.h>
#include <string.h>
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_shell/shell.h"
#include "shell_priv.h"

#define ACT_TABLE 16          /* every app's together */

struct t5_shell_activity {
    bool used;
    void *app;                /* t5_app_ctx_get() of the poster */
    char icon[T5_APP_ICON_LEN];
    char text[SHELL_ACTIVITY_TEXT_LEN];
    int8_t percent;
};

static struct t5_shell_activity s_act[ACT_TABLE];
static t5_mutex_t s_mx;

static t5_mutex_t mx(void)
{
    return t5_mutex_once(&s_mx);
}

/* In the table, in use, and the caller's own (an app can't end another's). */
static bool valid(t5_shell_activity_t *a)
{
    return a >= s_act && a < s_act + ACT_TABLE && a->used && a->app == t5_app_ctx_get();
}

static void copy_icon(char *dst, const char *icon)
{
    /* One glyph: an MD_ICON_* string is 3 bytes of UTF-8. */
    snprintf(dst, T5_APP_ICON_LEN, "%s", icon && *icon ? icon : MD_ICON_SCHEDULE);
}

static void release(void *h)
{
    t5_mutex_t m = mx();
    if (!m) return;
    t5_mutex_lock(m);
    struct t5_shell_activity *a = h;
    if (a >= s_act && a < s_act + ACT_TABLE) memset(a, 0, sizeof *a);
    t5_mutex_unlock(m);
}

t5_err_t t5_shell_activity_start(const char *icon, const char *text, t5_shell_activity_t **out)
{
    if (!out || !text) return T5_ERR_INVALID_ARG;
    *out = NULL;
    void *app = t5_app_ctx_get();
    t5_mutex_t m = mx();
    if (!m) return T5_ERR_NO_MEM;
    t5_mutex_lock(m);
    int mine = 0, free_at = -1;
    for (int i = 0; i < ACT_TABLE; i++) {
        if (s_act[i].used && s_act[i].app == app) mine++;
        else if (!s_act[i].used && free_at < 0) free_at = i;
    }
    t5_err_t e = T5_OK;
    if (app && mine >= T5_SHELL_ACTIVITY_MAX) e = T5_ERR_BUSY;
    else if (free_at < 0) e = T5_ERR_NO_MEM;
    else {
        struct t5_shell_activity *a = &s_act[free_at];
        memset(a, 0, sizeof *a);
        a->used = true;
        a->app = app;
        a->percent = -1;
        copy_icon(a->icon, icon);
        snprintf(a->text, sizeof a->text, "%s", text);
        *out = a;
    }
    t5_mutex_unlock(m);
    if (e == T5_OK && app) t5_res_add(T5_RES_OTHER, *out, release);
    return e;
}

t5_err_t t5_shell_activity_update(t5_shell_activity_t *a, const char *text, int percent)
{
    if (percent > 100) percent = 100;
    if (percent < -1) percent = -1;
    t5_mutex_t m = mx();
    if (!m) return T5_ERR_STATE;
    t5_mutex_lock(m);
    t5_err_t e = valid(a) ? T5_OK : T5_ERR_INVALID_ARG;
    if (e == T5_OK) {
        if (text) snprintf(a->text, sizeof a->text, "%s", text);
        a->percent = (int8_t)percent;
    }
    t5_mutex_unlock(m);
    return e;
}

t5_err_t t5_shell_activity_end(t5_shell_activity_t *a)
{
    t5_mutex_t m = mx();
    if (!m) return T5_ERR_STATE;
    t5_mutex_lock(m);
    bool ok = valid(a);
    t5_mutex_unlock(m);
    if (!ok) return T5_ERR_INVALID_ARG;
    t5_res_remove(a);
    release(a);
    return T5_OK;
}

int shell_activity_list(shell_activity_t *out, int max, const char *(*app_name)(void *app, char *buf, size_t n))
{
    t5_mutex_t m = mx();
    if (!m) return 0;
    struct t5_shell_activity copy[ACT_TABLE];
    t5_mutex_lock(m);
    memcpy(copy, s_act, sizeof copy);
    t5_mutex_unlock(m);
    int n = 0;
    for (int i = 0; i < ACT_TABLE && n < max; i++) {
        if (!copy[i].used) continue;
        shell_activity_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        snprintf(o->icon, sizeof o->icon, "%s", copy[i].icon);
        snprintf(o->text, sizeof o->text, "%s", copy[i].text);
        o->percent = copy[i].percent;
        o->app = true;
        app_name(copy[i].app, o->who, sizeof o->who);
    }
    return n;
}
