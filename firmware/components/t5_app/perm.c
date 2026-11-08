/* SPDX-License-Identifier: Apache-2.0
 * Permissions (plan 5.9). The manifest's groups are the app's consent list;
 * camera and mic are asked again on first use and the answer is stored with
 * the app's data (t5_pkg grants). For native apps this is consent and
 * correctness, not a boundary (T5_NATIVE_APP_WARNING).
 */
#include <stdio.h>
#include <stdlib.h>
#include "md/md.h"
#include "t5_core/log.h"
#include "t5_pkg/pkg.h"
#include "t5_ui/t5_ui.h"
#include "app_priv.h"

static const char *TAG = "t5.perm";

#define PROMPTED (T5_PERM_CAMERA | T5_PERM_MIC | T5_PERM_SHARED_READ | T5_PERM_SHARED_WRITE | T5_PERM_ALL_FILES)
#define WAIT_MS 60000

typedef struct {
    t5_app_t *app;
    uint32_t perm;
    t5_perm_cb_t cb;
    void *user;
    volatile int answer;     /* -1 pending, 0 denied, 1 granted */
    int refs;                /* the dialog, plus a waiting task if any */
    lv_obj_t *dlg;
} prompt_t;

/* Open prompts, so an app that exits takes its prompts with it: the dialog
 * is the system's and would otherwise outlive the app it names. */
#define MAX_PROMPTS 8
static prompt_t *s_open[MAX_PROMPTS];

static void track(prompt_t *p, bool on)
{
    for (int i = 0; i < MAX_PROMPTS; i++) {
        if (on && !s_open[i]) {
            s_open[i] = p;
            return;
        }
        if (!on && s_open[i] == p) {
            s_open[i] = NULL;
            return;
        }
    }
}

static void unref(prompt_t *p)
{
    apps_lock();
    int n = --p->refs;
    apps_unlock();
    if (n == 0) free(p);
}

void perm_app_gone(t5_app_t *app)
{
    for (int i = 0; i < MAX_PROMPTS; i++) {
        prompt_t *p = s_open[i];
        if (!p || p->app != app) continue;
        p->app = NULL;
        p->cb = NULL;
        if (p->answer < 0) p->answer = 0;      /* a waiting task sees "denied" */
        s_open[i] = NULL;
        if (p->dlg) md_dialog_close(p->dlg);   /* closes without calling answered() */
        unref(p);                              /* the dialog's reference */
    }
}


static void answered(int button, void *ud)
{
    prompt_t *p = ud;
    bool granted = button == 0;
    t5_app_t *a = p->app;
    track(p, false);
    if (a && a->hdr.magic == T5_APP_CTX_MAGIC) {
        if (granted) {
            a->granted |= p->perm;
            a->denied &= ~p->perm;
        } else {
            a->denied |= p->perm;
            a->granted &= ~p->perm;
        }
        if (!a->builtin) T5_LOG_ON_ERR(TAG, t5_pkg_grants_save(a->id, a->granted, a->denied));
        if (p->cb) p->cb(a, p->perm, granted, p->user);
    }
    p->answer = granted;
    unref(p);
}

static void show(prompt_t *p)
{
    char title[112];
    const char *n = p->app->man.name;
    switch (p->perm) {
    case T5_PERM_SHARED_READ: snprintf(title, sizeof title, "Allow %s to see your photos, music and documents?", n); break;
    case T5_PERM_SHARED_WRITE: snprintf(title, sizeof title, "Allow %s to change your photos, music and documents?", n); break;
    case T5_PERM_ALL_FILES: snprintf(title, sizeof title, "Allow %s to manage all files on your SD card and USB drives?", n); break;
    case T5_PERM_CAMERA: snprintf(title, sizeof title, "Allow %s to use the camera?", n); break;
    case T5_PERM_MIC: snprintf(title, sizeof title, "Allow %s to use the microphone?", n); break;
    default: snprintf(title, sizeof title, "Allow %s to use the %s?", n, t5_perm_name(p->perm)); break;
    }
    /* The dialog belongs to the system, not the app. */
    void *saved = t5_sys_enter();
    p->dlg = md_dialog_show(title, "You can change this later in App Manager.", "Allow", "Don't allow", answered, p);
    t5_sys_leave(saved);
    track(p, true);
}

void perm_prompt(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user)
{
    prompt_t *p = calloc(1, sizeof *p);
    if (!p) {
        if (cb) cb(app, perm, false, user);
        return;
    }
    *p = (prompt_t){ app, perm, cb, user, -1, 1, NULL };
    show(p);
}

t5_err_t t5_app_request_permission(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user)
{
    if (!app || app->hdr.magic != T5_APP_CTX_MAGIC) return T5_ERR_INVALID_ARG;
    if (!(app->man.perms & perm)) return T5_ERR_DENIED;          /* not declared */
    if (!(perm & PROMPTED) || (app->granted & perm) == perm) {
        if (cb) cb(app, perm, true, user);
        return T5_OK;
    }
    if (!t5_ui_on_ui_task()) return T5_ERR_STATE;
    perm_prompt(app, perm, cb, user);
    return T5_OK;
}

static void show_async(void *p)
{
    show(p);
}

static bool prompt_open(const t5_app_t *a, uint32_t perm)
{
    for (int i = 0; i < MAX_PROMPTS; i++) {
        if (s_open[i] && s_open[i]->app == a && s_open[i]->perm == perm) return true;
    }
    return false;
}

static t5_err_t check_for(t5_app_t *a, uint32_t perm);

t5_err_t t5_perm_check(uint32_t perm)
{
    t5_app_t *a = t5_app_self();
    if (!a) return T5_OK;                                         /* system */
    return check_for(a, perm);
}

t5_err_t perm_check_any(t5_app_t *a, uint32_t mask)
{
    if (a->granted & mask & a->man.perms) return T5_OK;
    uint32_t declared = mask & a->man.perms;
    if (!declared) return T5_ERR_DENIED;
    /* The narrowest one the app declared. */
    return check_for(a, declared & -declared);
}

static t5_err_t check_for(t5_app_t *a, uint32_t perm)
{
    if (!(a->man.perms & perm)) return T5_ERR_DENIED;
    if (!(perm & PROMPTED) || (a->granted & perm) == perm) return T5_OK;
    if (a->denied & perm) return T5_ERR_DENIED;
    if (t5_ui_on_ui_task()) {
        /* Can't block the UI task on a dialog: ask, and deny this call. The
         * app should use t5_app_request_permission() first. */
        if (!prompt_open(a, perm)) perm_prompt(a, perm, NULL, NULL);
        return T5_ERR_DENIED;
    }
    prompt_t *p = calloc(1, sizeof *p);
    if (!p) return T5_ERR_NO_MEM;
    *p = (prompt_t){ a, perm, NULL, NULL, -1, 2, NULL };
    if (t5_ui_async(show_async, p) != T5_OK) {
        free(p);
        return T5_ERR_STATE;
    }
    for (uint32_t t = 0; p->answer < 0 && t < WAIT_MS; t += 50) t5_delay_ms(50);
    int answer = p->answer;
    unref(p);     /* the dialog keeps its own reference if still open */
    if (answer < 0) return T5_ERR_TIMEOUT;
    return answer ? T5_OK : T5_ERR_DENIED;
}
