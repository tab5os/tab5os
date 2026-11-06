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

#define PROMPTED (T5_PERM_CAMERA | T5_PERM_MIC)
#define WAIT_MS 60000

typedef struct {
    t5_app_t *app;
    uint32_t perm;
    t5_perm_cb_t cb;
    void *user;
    volatile int answer;     /* -1 pending, 0 denied, 1 granted */
    int refs;                /* the dialog, plus a waiting task if any */
} prompt_t;

static void unref(prompt_t *p)
{
    apps_lock();
    int n = --p->refs;
    apps_unlock();
    if (n == 0) free(p);
}

static void answered(int button, void *ud)
{
    prompt_t *p = ud;
    bool granted = button == 0;
    t5_app_t *a = p->app;
    if (a->hdr.magic == T5_APP_CTX_MAGIC) {
        if (granted) {
            a->granted |= p->perm;
            a->denied &= ~p->perm;
        } else {
            a->denied |= p->perm;
            a->granted &= ~p->perm;
        }
        if (!a->builtin) t5_pkg_grants_save(a->id, a->granted, a->denied);
        if (p->cb) p->cb(a, p->perm, granted, p->user);
    }
    p->answer = granted;
    unref(p);
}

static void show(prompt_t *p)
{
    char title[96];
    snprintf(title, sizeof title, "Allow %s to use the %s?", p->app->man.name,
             p->perm == T5_PERM_CAMERA ? "camera" : p->perm == T5_PERM_MIC ? "microphone" : t5_perm_name(p->perm));
    /* The dialog belongs to the system, not the app. */
    void *saved = t5_sys_enter();
    md_dialog_show(title, "You can change this later in App Manager.", "Allow", "Don't allow", answered, p);
    t5_sys_leave(saved);
}

void perm_prompt(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user)
{
    prompt_t *p = calloc(1, sizeof *p);
    if (!p) {
        if (cb) cb(app, perm, false, user);
        return;
    }
    *p = (prompt_t){ app, perm, cb, user, -1, 1 };
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

t5_err_t t5_perm_check(uint32_t perm)
{
    t5_app_t *a = t5_app_self();
    if (!a) return T5_OK;                                         /* system */
    if (!(a->man.perms & perm)) return T5_ERR_DENIED;
    if (!(perm & PROMPTED) || (a->granted & perm) == perm) return T5_OK;
    if (a->denied & perm) return T5_ERR_DENIED;
    if (t5_ui_on_ui_task()) {
        /* Can't block the UI task on a dialog: ask, and deny this call. The
         * app should use t5_app_request_permission() first. */
        perm_prompt(a, perm, NULL, NULL);
        return T5_ERR_DENIED;
    }
    prompt_t *p = calloc(1, sizeof *p);
    if (!p) return T5_ERR_NO_MEM;
    *p = (prompt_t){ a, perm, NULL, NULL, -1, 2 };
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
