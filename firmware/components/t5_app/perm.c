/* SPDX-License-Identifier: Apache-2.0
 * Permissions (plan 5.9). The manifest's groups are the app's consent list;
 * camera and mic are asked again on first use and the answer is stored with
 * the app's data (t5_pkg grants). For native apps this is consent and
 * correctness, not a boundary (T5_NATIVE_APP_WARNING).
 */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "md/md.h"
#include "t5_core/log.h"
#include "t5_pkg/pkg.h"
#include "t5_ui/t5_ui.h"
#include "app_priv.h"

static const char *TAG = "t5.perm";

#define WAIT_MS 60000

_Static_assert(T5_CORE_PERM_CAMERA == T5_PERM_CAMERA && T5_CORE_PERM_MIC == T5_PERM_MIC &&
               T5_CORE_PERM_USB == T5_PERM_USB, "t5_core's permission bits are the manifest's");

typedef struct {
    t5_app_t *app;           /* NULL once the app is gone */
    uint32_t perm;
    t5_perm_cb_t cb;
    void *user;
    int answer;              /* -1 pending, 0 denied, 1 granted */
    int refs;                /* the dialog, plus a waiting task if any */
    lv_obj_t *dlg;
    SemaphoreHandle_t wake;   /* a waiting task's, given with the answer */
    char name[T5_APP_NAME_LEN];
} prompt_t;

/* Open prompts, from creation until answered, so an app that exits takes its
 * prompts with it: the dialog is the system's and would otherwise outlive
 * the app it names. Guarded by the apps lock. */
#define MAX_PROMPTS 8
static prompt_t *s_open[MAX_PROMPTS];

static void untrack_locked(prompt_t *p)
{
    for (int i = 0; i < MAX_PROMPTS; i++) {
        if (s_open[i] == p) s_open[i] = NULL;
    }
}

static prompt_t *prompt_new(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user, bool waiter)
{
    prompt_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    *p = (prompt_t){ .app = app, .perm = perm, .cb = cb, .user = user, .answer = -1, .refs = waiter ? 2 : 1 };
    snprintf(p->name, sizeof p->name, "%s", app->man.name);
    if (waiter && !(p->wake = xSemaphoreCreateBinary())) {
        free(p);
        return NULL;
    }
    apps_lock();
    int i = 0;
    while (i < MAX_PROMPTS && s_open[i]) i++;
    if (i < MAX_PROMPTS) s_open[i] = p;
    apps_unlock();
    if (i == MAX_PROMPTS) {
        T5_LOGW(TAG, "%s: too many open permission prompts", app->id);
        if (p->wake) vSemaphoreDelete(p->wake);
        free(p);
        return NULL;
    }
    return p;
}

static void unref(prompt_t *p)
{
    apps_lock();
    int n = --p->refs;
    apps_unlock();
    if (n) return;
    if (p->wake) vSemaphoreDelete(p->wake);
    free(p);
}

static void set_answer(prompt_t *p, int answer)
{
    p->answer = answer;
    if (p->wake) xSemaphoreGive(p->wake);
}

/* Runs as the app starts exiting, so a task waiting on a prompt hears
 * "denied" while it still has time to finish. A prompt not shown yet is
 * dropped by show(). */
void perm_app_gone(t5_app_t *app)
{
    for (;;) {
        prompt_t *p = NULL;
        lv_obj_t *dlg = NULL;
        apps_lock();
        for (int i = 0; i < MAX_PROMPTS && !p; i++) {
            if (s_open[i] && s_open[i]->app == app) p = s_open[i];
        }
        if (p) {
            untrack_locked(p);
            p->app = NULL;
            p->cb = NULL;
            if (p->answer < 0) set_answer(p, 0);
            dlg = p->dlg;
            p->dlg = NULL;
        }
        apps_unlock();
        if (!p) return;
        if (dlg) {
            md_dialog_close(dlg);   /* closes without calling answered() */
            unref(p);               /* the dialog's reference */
        }
    }
}

static void answered(int button, void *ud)
{
    prompt_t *p = ud;
    bool granted = button == 0;
    apps_lock();
    t5_app_t *a = p->app;
    untrack_locked(p);
    p->dlg = NULL;
    apps_unlock();
    if (a) {
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
    set_answer(p, granted);
    unref(p);
}

/* How the person sees a group: App Manager's label, and the question.
 * The rest are their manifest name ("use the camera"). */
static const struct {
    uint32_t bit;
    const char *label;
    const char *ask;
} s_words[] = {
    { T5_PERM_SHARED_READ, "See shared files", "see your photos, music and documents" },
    { T5_PERM_SHARED_WRITE, "Change shared files", "change your photos, music and documents" },
    { T5_PERM_ALL_FILES, "Manage all files", "manage all files on your SD card and USB drives" },
    { T5_PERM_MIC, NULL, "use the microphone" },
};

const char *t5_perm_label(uint32_t perm_bit)
{
    for (size_t i = 0; i < sizeof s_words / sizeof s_words[0]; i++) {
        if (s_words[i].bit == perm_bit && s_words[i].label) return s_words[i].label;
    }
    return t5_perm_name(perm_bit);
}

/* UI task. Reads only the prompt: the app may be gone by now. */
static void show(void *arg)
{
    prompt_t *p = arg;
    apps_lock();
    bool gone = !p->app;
    apps_unlock();
    if (gone) {
        unref(p);   /* the dialog's reference; it never opened */
        return;
    }
    char title[112];
    const char *ask = NULL;
    for (size_t i = 0; i < sizeof s_words / sizeof s_words[0]; i++) {
        if (s_words[i].bit == p->perm) ask = s_words[i].ask;
    }
    if (ask) snprintf(title, sizeof title, "Allow %s to %s?", p->name, ask);
    else snprintf(title, sizeof title, "Allow %s to use the %s?", p->name, t5_perm_name(p->perm));
    /* The dialog belongs to the system, not the app. */
    void *saved = t5_sys_enter();
    lv_obj_t *dlg = md_dialog_show(title, "You can change this later in App Manager.", "Allow", "Don't allow", answered, p);
    t5_sys_leave(saved);
    apps_lock();
    gone = !p->app;
    if (!gone) p->dlg = dlg;
    apps_unlock();
    if (gone) {
        if (dlg) md_dialog_close(dlg);
        unref(p);
    }
}

static void perm_prompt(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user)
{
    prompt_t *p = prompt_new(app, perm, cb, user, false);
    if (!p) {
        if (cb) cb(app, perm, false, user);
        return;
    }
    show(p);
}

t5_err_t t5_app_request_permission(t5_app_t *app, uint32_t perm, t5_perm_cb_t cb, void *user)
{
    if (!app || app->hdr.magic != T5_APP_CTX_MAGIC) return T5_ERR_INVALID_ARG;
    if (!(app->man.perms & perm)) return T5_ERR_DENIED;          /* not declared */
    if (!(perm & t5_perm_prompted()) || (app->granted & perm) == perm) {
        if (cb) cb(app, perm, true, user);
        return T5_OK;
    }
    if (!t5_ui_on_ui_task()) return T5_ERR_STATE;
    perm_prompt(app, perm, cb, user);
    return T5_OK;
}

static bool prompt_open(const t5_app_t *a, uint32_t perm)
{
    bool open = false;
    apps_lock();
    for (int i = 0; i < MAX_PROMPTS; i++) {
        if (s_open[i] && s_open[i]->app == a && s_open[i]->perm == perm) open = true;
    }
    apps_unlock();
    return open;
}

static t5_err_t check_for(t5_app_t *a, uint32_t perm);

int perm_check_self(uint32_t perm)
{
    t5_app_t *a = t5_app_self();
    return a ? (int)check_for(a, perm) : T5_OK;
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
    if (!(perm & t5_perm_prompted()) || (a->granted & perm) == perm) return T5_OK;
    if (a->denied & perm) return T5_ERR_DENIED;
    if (t5_ui_on_ui_task()) {
        /* Can't block the UI task on a dialog: ask, and deny this call. The
         * app should use t5_app_request_permission() first. */
        if (!prompt_open(a, perm)) perm_prompt(a, perm, NULL, NULL);
        return T5_ERR_DENIED;
    }
    prompt_t *p = prompt_new(a, perm, NULL, NULL, true);
    if (!p) return T5_ERR_NO_MEM;
    if (t5_ui_async(show, p) != T5_OK) {
        apps_lock();
        untrack_locked(p);
        apps_unlock();
        vSemaphoreDelete(p->wake);
        free(p);
        return T5_ERR_STATE;
    }
    xSemaphoreTake(p->wake, pdMS_TO_TICKS(WAIT_MS));
    int answer = p->answer;
    unref(p);     /* the dialog keeps its own reference if still open */
    if (answer < 0) return T5_ERR_TIMEOUT;
    return answer ? T5_OK : T5_ERR_DENIED;
}
