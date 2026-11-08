/* SPDX-License-Identifier: Apache-2.0
 * The system file picker (t5_file_pick). A large modal over the app: the
 * person browses the SD card, USB drive and internal shared folder, and the
 * file they choose is opened up to the app for this run. Choosing is the
 * consent, so no permission is needed.
 *
 * Every way out goes through one of two paths:
 *   finish(): Cancel, the close button, Back at the top level, a tap outside
 *             the box, the home gesture. The app's callback runs with the
 *             file or with NULL, and the app keeps running.
 *   picker_app_gone(): the app exited or crashed. The picker just goes; the
 *             app's code is no longer there to call.
 *
 * Folders are read on a task of their own (a big directory on a slow card
 * must not stall the UI); a result that arrives after the picker moved on
 * or closed is dropped by its generation number.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "md/md.h"
#include "md/md_icons.h"
#include "t5_app/fspolicy.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_ui/t5_ui.h"
#include "app_priv.h"

static const char *TAG = "t5.pick";

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MAX_ENTRIES 1000
#define POOL_BYTES (64u * 1024u)
#define ROW_DP 48

typedef struct {
    const char *name;           /* in the listing's pool; a label for top-level rows */
    const char *path;           /* top-level rows only */
    bool dir;
} entry_t;

typedef struct {
    uint32_t gen;
    char path[T5_FS_PATH_MAX];  /* "": the top level */
    char exts[48];
    entry_t *ents;
    char *pool;
    uint32_t n;
    bool failed;
    bool truncated;
} listing_t;

typedef struct {
    t5_app_t *app;
    t5_file_pick_cb_t cb;
    void *user;
    uint32_t flags;
    char title[64];
    char exts[48];
    char path[T5_FS_PATH_MAX];
    lv_obj_t *scrim, *bar, *list, *status, *crumb;
    listing_t *shown;
    uint32_t gen;
} picker_t;

static picker_t *s_p;           /* UI task only */

static const struct {
    const char *path;
    const char *label;
} s_roots[] = {
    { "/sd", "SD card" },
    { "/usb", "USB drive" },
    { "/data/user", "Internal storage" },
};
#define NROOTS (sizeof s_roots / sizeof s_roots[0])

static void listing_free(listing_t *l)
{
    if (!l) return;
    heap_caps_free(l->ents);
    heap_caps_free(l->pool);
    heap_caps_free(l);
}

static void destroy(picker_t *p)
{
    s_p = NULL;
    p->gen++;                   /* any listing on its way is dropped */
    if (p->scrim) lv_obj_delete(p->scrim);
    listing_free(p->shown);
    heap_caps_free(p);
}

/* The person is done: close, then tell the app. It may open another picker
 * or exit from its callback, so nothing here runs after the call. */
static void finish(picker_t *p, const char *path)
{
    t5_app_t *a = p->app;
    t5_file_pick_cb_t cb = p->cb;
    void *user = p->user;
    uint32_t flags = p->flags;
    char picked[T5_FS_PATH_MAX];
    if (path) snprintf(picked, sizeof picked, "%s", path);
    void *saved = t5_sys_enter();
    destroy(p);
    t5_sys_leave(saved);
    if (a->exiting || !cb) return;
    if (path) apps_fs_add_pick(a, picked, flags & T5_FILE_PICK_WRITE);
    void *ctx = t5_app_ctx_get();
    t5_app_ctx_set(a);
    cb(a, path ? picked : NULL, user);
    t5_app_ctx_set(ctx);
}

void picker_app_gone(t5_app_t *app)
{
    if (!s_p || s_p->app != app) return;
    void *saved = t5_sys_enter();
    destroy(s_p);
    t5_sys_leave(saved);
}

bool picker_dismiss(void)
{
    if (!s_p) return false;
    finish(s_p, NULL);
    return true;
}

/* ---- reading a folder ------------------------------------------------------------- */
static int entry_cmp(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool dir_exists(const char *path)
{
    DIR *d = opendir(path);
    if (d) closedir(d);
    return d != NULL;
}

static void read_top_level(listing_t *l)
{
    for (size_t i = 0; i < NROOTS; i++) {
        if (!dir_exists(s_roots[i].path)) continue;
        l->ents[l->n++] = (entry_t){ s_roots[i].label, s_roots[i].path, true };
    }
}

static void read_folder(listing_t *l)
{
    DIR *d = opendir(l->path);
    if (!d) {
        l->failed = true;
        return;
    }
    size_t used = 0;
    char full[T5_FS_PATH_MAX];
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        bool is_dir = e->d_type == DT_DIR;
        if (!is_dir && !t5_fs_ext_matches(e->d_name, l->exts)) continue;
        int w = snprintf(full, sizeof full, "%s/%s", l->path, e->d_name);
        if (w < 0 || (size_t)w >= sizeof full || t5_fs_protected(full)) continue;
        size_t len = strlen(e->d_name) + 1;
        if (l->n >= MAX_ENTRIES || used + len > POOL_BYTES) {
            l->truncated = true;
            break;
        }
        memcpy(l->pool + used, e->d_name, len);
        l->ents[l->n++] = (entry_t){ l->pool + used, NULL, is_dir };
        used += len;
    }
    closedir(d);
    qsort(l->ents, l->n, sizeof l->ents[0], entry_cmp);
}

static void show_listing(void *arg);

static void list_task(void *arg)
{
    listing_t *l = arg;
    if (l->path[0]) read_folder(l);
    else read_top_level(l);
    if (t5_ui_async(show_listing, l) != T5_OK) listing_free(l);
    vTaskDeleteWithCaps(NULL);
}

static void set_status(picker_t *p, const char *text)
{
    if (text) {
        lv_label_set_text(p->status, text);
        lv_obj_set_hidden(p->status, false);
        lv_obj_set_hidden(p->list, true);
    } else {
        lv_obj_set_hidden(p->status, true);
        lv_obj_set_hidden(p->list, false);
    }
}

static const char *leaf_label(const picker_t *p)
{
    for (size_t i = 0; i < NROOTS; i++) {
        if (!strcmp(p->path, s_roots[i].path)) return s_roots[i].label;
    }
    const char *s = strrchr(p->path, '/');
    return s ? s + 1 : p->path;
}

static void go(picker_t *p, const char *path)
{
    snprintf(p->path, sizeof p->path, "%s", path);
    uint32_t gen = ++p->gen;
    md_appbar_set_title(p->bar, p->path[0] ? leaf_label(p) : p->title);
    lv_label_set_text(p->crumb, p->path[0] ? p->path : "Choose a location");
    md_list_set_count(p->list, 0);
    set_status(p, "Loading...");
    listing_t *l = heap_caps_calloc(1, sizeof *l, CAPS);
    if (l) {
        l->ents = heap_caps_malloc(MAX_ENTRIES * sizeof(entry_t), CAPS);
        l->pool = heap_caps_malloc(POOL_BYTES, CAPS);
    }
    if (!l || !l->ents || !l->pool) {
        listing_free(l);
        set_status(p, "Not enough memory to read this folder.");
        return;
    }
    l->gen = gen;
    snprintf(l->path, sizeof l->path, "%s", p->path);
    snprintf(l->exts, sizeof l->exts, "%s", p->exts);
    if (xTaskCreatePinnedToCoreWithCaps(list_task, "t5_pick", 6144, l, 3, NULL, tskNO_AFFINITY, CAPS) != pdPASS) {
        listing_free(l);
        set_status(p, "Couldn't read this folder.");
    }
}

static void show_listing(void *arg)
{
    listing_t *l = arg;
    picker_t *p = s_p;
    if (!p || l->gen != p->gen) {
        listing_free(l);
        return;
    }
    listing_t *old = p->shown;
    p->shown = l;
    md_list_set_count(p->list, l->n);
    listing_free(old);
    if (l->failed) set_status(p, "Couldn't read this folder.");
    else if (!l->n) set_status(p, l->path[0] ? "Nothing to choose from here." : "No storage available.");
    else set_status(p, NULL);
    if (l->truncated) T5_LOGW(TAG, "%s: showing the first %u entries", l->path, (unsigned)l->n);
}

/* ---- the modal ------------------------------------------------------------------- */
static void bind_row(md_list_row_t *r, uint32_t i, void *ud)
{
    picker_t *p = ud;
    if (!p->shown || i >= p->shown->n) return;
    const entry_t *e = &p->shown->ents[i];
    lv_label_set_text(r->primary, e->name);
    lv_label_set_text(r->icon, e->dir ? MD_ICON_FOLDER : MD_ICON_INSERT_DRIVE_FILE);
}

static void row_clicked(uint32_t i, void *ud)
{
    picker_t *p = ud;
    if (!p->shown || i >= p->shown->n) return;
    const entry_t *e = &p->shown->ents[i];
    if (e->path) {
        go(p, e->path);
        return;
    }
    char full[T5_FS_PATH_MAX];
    int w = snprintf(full, sizeof full, "%s/%s", p->path, e->name);
    if (w < 0 || (size_t)w >= sizeof full) return;
    if (e->dir) go(p, full);
    else finish(p, full);
}

static void up_clicked(lv_event_t *e)
{
    picker_t *p = lv_event_get_user_data(e);
    if (!p->path[0]) {
        finish(p, NULL);
        return;
    }
    for (size_t i = 0; i < NROOTS; i++) {
        if (!strcmp(p->path, s_roots[i].path)) {
            go(p, "");
            return;
        }
    }
    char *s = strrchr(p->path, '/');
    if (s) *s = 0;
    char parent[T5_FS_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", p->path);
    go(p, parent);
}

static void cancel_clicked(lv_event_t *e)
{
    finish(lv_event_get_user_data(e), NULL);
}

static void scrim_clicked(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) return;   /* inside the box */
    finish(lv_event_get_user_data(e), NULL);
}

static void build(picker_t *p)
{
    const md_scheme_t *sc = md_scheme();
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    p->scrim = scrim;
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, md_color(sc->c.scrim), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_set_clickable(scrim, true);
    lv_obj_set_scrollable(scrim, false);
    lv_obj_add_event_cb(scrim, scrim_clicked, LV_EVENT_CLICKED, p);

    /* Percentages, so a rotation needs nothing from us. */
    lv_obj_t *box = lv_obj_create(scrim);
    md_set_elevation(box, MD_ELEV_DIALOG, true);
    lv_obj_set_size(box, LV_PCT(94), LV_PCT(90));
    lv_obj_center(box);
    lv_obj_set_style_radius(box, T5_DP(MD_SHAPE_MEDIUM_DP), 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(box, false);

    p->bar = md_appbar_create(box, p->title, MD_ICON_ARROW_BACK, up_clicked, p);
    md_appbar_add_action(p->bar, MD_ICON_CLOSE, cancel_clicked, p);

    lv_obj_t *body = lv_obj_create(box);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_scrollable(body, false);
    p->list = md_list_create(body, 0, ROW_DP, bind_row, row_clicked, p);
    lv_obj_set_size(p->list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(p->list, LV_OPA_TRANSP, 0);      /* the box's colour, as the footer */
    p->status = md_label_create(body, "", MD_TYPE_BODY1);
    lv_obj_center(p->status);
    lv_obj_set_hidden(p->status, true);

    md_divider_create(box);
    lv_obj_t *foot = lv_obj_create(box);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_PCT(100), T5_DP(56));
    lv_obj_set_style_pad_hor(foot, T5_DP(16), 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(foot, false);
    p->crumb = md_label_create(foot, "", MD_TYPE_CAPTION);
    lv_obj_set_flex_grow(p->crumb, 1);
    lv_label_set_long_mode(p->crumb, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_opa(p->crumb, sc->on_surface.medium, 0);
    lv_obj_t *cancel = md_button_create(foot, "Cancel", MD_BUTTON_TEXT);
    lv_obj_add_event_cb(cancel, cancel_clicked, LV_EVENT_CLICKED, p);
}

/* The card can go while a folder on it is open. */
static void storage_changed_ui(void *arg)
{
    (void)arg;
    picker_t *p = s_p;
    if (!p) return;
    if (!p->path[0] || t5_fs_under(p->path, "/sd")) {
        void *saved = t5_sys_enter();
        go(p, "");
        t5_sys_leave(saved);
    }
}

static void storage_event(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id; (void)data; (void)len; (void)arg;
    if (t5_ui_async(storage_changed_ui, NULL) != T5_OK) T5_LOGW(TAG, "can't refresh after a storage change");
}

void picker_init(void)
{
    T5_LOG_ON_ERR(TAG, t5_event_subscribe(T5_EV_SD_INSERTED, storage_event, NULL));
    T5_LOG_ON_ERR(TAG, t5_event_subscribe(T5_EV_SD_REMOVED, storage_event, NULL));
}

t5_err_t t5_file_pick(t5_app_t *app, const char *title, const char *exts, uint32_t flags, t5_file_pick_cb_t cb, void *user)
{
    if (!app || app->hdr.magic != T5_APP_CTX_MAGIC || !cb) return T5_ERR_INVALID_ARG;
    if (!t5_ui_on_ui_task()) return T5_ERR_STATE;
    if (s_p) return T5_ERR_BUSY;
    if (app->exiting) return T5_ERR_STATE;
    picker_t *p = heap_caps_calloc(1, sizeof *p, CAPS);
    if (!p) return T5_ERR_NO_MEM;
    p->app = app;
    p->cb = cb;
    p->user = user;
    p->flags = flags;
    snprintf(p->title, sizeof p->title, "%s", title && title[0] ? title : "Choose a file");
    snprintf(p->exts, sizeof p->exts, "%s", exts ? exts : "");
    void *saved = t5_sys_enter();
    s_p = p;
    build(p);
    go(p, "");
    t5_sys_leave(saved);
    return T5_OK;
}
