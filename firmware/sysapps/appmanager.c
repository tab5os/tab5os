/* SPDX-License-Identifier: Apache-2.0
 * App Manager (plan 6.7): installed and built-in apps; per app its
 * version, storage, permissions (the ones asked at first use can be changed),
 * crash record and core dump, average draw, and removal. A crash report
 * list covers every app. The launch argument opens one app's page.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "t5/system.h"
#include "t5_core/res.h"     /* T5_CORE_CAPS */
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_icons.h"
#include "t5_pkg/manifest.h"
#include "t5_shell/app_icon.h"
#include "t5_storage/paths.h"
#include "sysapps_priv.h"


static sa_nav_t s_nav;
static void open_app(const char *id);

/* A list row, as md made them: grows when its second line wraps. */
static t5ui_obj_t *row_create(t5ui_obj_t *parent, const char *icon, const char *primary, const char *secondary)
{
    t5ui_obj_t *r = t5ui_create(parent, T5UI_MD_ROW);
    if (icon) t5ui_set_str(r, T5UI_P_ICON, icon);
    t5ui_set_str(r, T5UI_P_TEXT, primary ? primary : "");
    if (secondary) {
        t5ui_set_str(r, T5UI_P_SECONDARY, secondary);
        t5ui_set_int(r, T5UI_P_HEIGHT, T5UI_CONTENT);
        t5ui_set_int(r, T5UI_P_MIN_HEIGHT, t5ui_dp(64));
    }
    return r;
}

static void section(t5ui_obj_t *parent, const char *title)
{
    t5ui_set_str(t5ui_create(parent, T5UI_MD_SECTION), T5UI_P_TEXT, title);
}

static t5ui_obj_t *button(t5ui_obj_t *parent, t5ui_kind_t kind, const char *text, t5ui_event_cb_t cb, void *user)
{
    t5ui_obj_t *b = t5ui_create(parent, kind);
    t5ui_set_str(b, T5UI_P_TEXT, text);
    t5ui_on(b, T5UI_EV_CLICK, cb, user);
    return b;
}

/* A switch at the end of a row, changing permission bit. */
static void grant_changed(t5ui_event_t *e);
static void grant_switch(t5ui_obj_t *row, bool on, uint32_t bit)
{
    t5ui_obj_t *sw = t5ui_create(row, T5UI_MD_ROW_SWITCH);
    t5ui_set_int(sw, T5UI_P_CHECKED, on);
    t5ui_on(sw, T5UI_EV_VALUE, grant_changed, (void *)(uintptr_t)bit);
}

/* ---- one app ------------------------------------------------------------------------- */
typedef struct {
    char id[64];
    t5_app_info_t info;
    t5ui_obj_t *crash_row;
} app_page_t;

static void grant_changed(t5ui_event_t *e)
{
    app_page_t *p = s_nav.lv[s_nav.depth - 1].arg;
    uint32_t perm = (uint32_t)(uintptr_t)t5ui_event_user(e);
    bool on = t5ui_get_int(t5ui_event_target(e), T5UI_P_CHECKED, 0);
    sa_report(t5_apps_set_grant(p->id, perm, on), "change the permission");
}

/* An installed app's own icon in place of a row's glyph: 40 dp, the
 * width of the glyph's slot, so the names still line up. */
static void app_row_icon(t5ui_obj_t *row, const t5_app_entry_t *a)
{
    t5ui_obj_t *img = row && !a->builtin ? t5_shell_app_icon_create(row, a->id, t5ui_dp(40)) : NULL;
    if (!img) return;
    t5ui_set_int(t5ui_child(row, 0), T5UI_P_HIDDEN, true);   /* the glyph */
    t5ui_move_to(img, 0);
}

static void open_clicked(t5ui_event_t *e)
{
    app_page_t *p = t5ui_event_user(e);
    sa_report(t5_apps_launch(p->id), "open the app");
}

static void crash_reset_clicked(t5ui_event_t *e)
{
    app_page_t *p = t5ui_event_user(e);
    t5_err_t err = t5_apps_crash_reset(p->id);
    if (err != T5_OK) {
        sa_report(err, "clear the crash record");
        return;
    }
    t5ui_snackbar("Crash record cleared", NULL, NULL, NULL);
    if (p->crash_row) t5ui_set_str(p->crash_row, T5UI_P_SECONDARY, "No crashes since it was cleared");
}

typedef struct {
    char id[64];
    char name[T5_APP_NAME_LEN];
    bool keep;
} remove_t;

static void remove_answer(int b, void *ud)
{
    remove_t *r = ud;
    if (b == 0) {
        t5_err_t e = t5_apps_remove(r->id, r->keep);
        char msg[96];
        if (e == T5_OK) snprintf(msg, sizeof msg, "%s removed", r->name);
        else if (e == T5_ERR_BUSY) snprintf(msg, sizeof msg, "Couldn't remove %s: close it first", r->name);
        else snprintf(msg, sizeof msg, "Couldn't remove %s (%s)", r->name, t5_err_str(e));
        t5ui_snackbar(msg, NULL, NULL, NULL);
        if (e == T5_OK) sa_pop(&s_nav);
    }
    free(r);
}

static void remove_clicked(t5ui_event_t *e)
{
    app_page_t *p = t5ui_event_user(e);
    remove_t *r = calloc(1, sizeof *r);
    if (!r) return;
    snprintf(r->id, sizeof r->id, "%s", p->id);
    snprintf(r->name, sizeof r->name, "%s", p->info.entry.name[0] ? p->info.entry.name : p->id);
    r->keep = true;
    char title[96];
    snprintf(title, sizeof title, "Remove %s?", p->info.entry.name);
    t5ui_dialog(title, "Its saved data stays on the Tab5, so installing it again picks up where you left off.",
                   "Remove", "Cancel", remove_answer, r);
}

static const char *crash_kind(uint8_t k)
{
    return t5_crash_kind_name((t5_crash_kind_t)k);
}

static void open_app(const char *id)
{
    app_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    snprintf(p->id, sizeof p->id, "%s", id);
    if (t5_apps_info(id, &p->info) != T5_OK) {
        free(p);
        t5ui_snackbar("That app isn't installed any more", NULL, NULL, NULL);
        return;
    }
    const t5_app_info_t *in = &p->info;
    t5ui_obj_t *body = sa_push(&s_nav, in->entry.name[0] ? in->entry.name : id);
    char buf[256], a[24], b[24];
    if (in->entry.builtin) snprintf(buf, sizeof buf, "Part of Tab5 OS");
    else snprintf(buf, sizeof buf, "Version %s", in->entry.version);
    app_row_icon(row_create(body, in->entry.icon[0] ? in->entry.icon : T5UI_ICON_EXTENSION, in->entry.name, buf),
                 &in->entry);
    t5ui_obj_t *row = sa_button_row(body);
    button(row, T5UI_MD_BUTTON_CONTAINED, "Open", open_clicked, p);
    if (!in->entry.builtin) button(row, T5UI_MD_BUTTON_OUTLINED, "Remove", remove_clicked, p);

    section(body, "Storage");
    if (!in->entry.builtin) {
        sa_fmt_bytes(a, sizeof a, in->install_bytes);
        snprintf(buf, sizeof buf, "%s on %s", a, in->on_sd ? "the SD card" : "internal storage");
        row_create(body, in->on_sd ? T5UI_ICON_SD_STORAGE : T5UI_ICON_STORAGE, "App", buf);
    }
    sa_fmt_bytes(b, sizeof b, in->data_bytes);
    row_create(body, T5UI_ICON_FOLDER, "Saved data", in->data_bytes ? b : "Nothing saved yet");

    section(body, "Permissions");
    uint32_t perms = in->entry.perms;
    if (!perms) sa_note(body, "This app doesn't ask for any permissions.");
    for (uint32_t bit = 1; bit & T5_PERM_ALL; bit <<= 1) {
        if (!(perms & bit)) continue;
        const char *name = t5_perm_label(bit);
        if ((bit & t5_perm_prompted()) && !in->entry.builtin) {
            const char *state = (in->granted & bit) ? "Allowed" : (in->denied & bit) ? "Not allowed" : "Asks the first time";
            t5ui_obj_t *r = row_create(body, bit == T5_PERM_CAMERA ? T5UI_ICON_CAMERA_ALT : bit == T5_PERM_MIC ? T5UI_ICON_MIC : T5UI_ICON_FOLDER,
                                       name, state);
            grant_switch(r, (in->granted & bit) != 0, bit);
        } else {
            row_create(body, T5UI_ICON_CHECK, name, in->entry.builtin ? NULL : "Allowed when installed");
        }
    }
    /* Capabilities (USB device mode, Wi-Fi monitor...): only those the app
     * has asked for, each a switch like camera and mic. */
    for (uint32_t bit = T5_CORE_CAPS & -T5_CORE_CAPS; bit & T5_CORE_CAPS; bit <<= 1) {
        if (in->entry.builtin || !((in->granted | in->denied) & bit)) continue;
        t5ui_obj_t *r = row_create(body, T5UI_ICON_SETTINGS, t5_perm_label(bit),
                                   (in->granted & bit) ? "Allowed" : "Not allowed");
        grant_switch(r, (in->granted & bit) != 0, bit);
    }
    if (!in->entry.builtin) sa_note(body, T5_NATIVE_APP_WARNING);

    section(body, "Power");
    t5_energy_rec_t rec[T5_ENERGY_MAX];
    int n = t5_apps_energy(rec, T5_ENERGY_MAX);
    snprintf(buf, sizeof buf, "Not measured yet. Use it on battery to get a reading.");
    for (int i = 0; i < n; i++) {
        if (!strcmp(rec[i].id, id) && t5_energy_avg_mw(&rec[i]) >= 0) {
            sa_fmt_duration(a, sizeof a, rec[i].ms / 1000);
            snprintf(buf, sizeof buf, "%d mW on average, over %s of use", (int)t5_energy_avg_mw(&rec[i]), a);
        }
    }
    row_create(body, T5UI_ICON_BOLT, "Average draw", buf);

    section(body, "Crashes");
    if (in->crashes_total) {
        char when[32] = "";
        if (in->last_crash_time > 0) {
            time_t t = (time_t)in->last_crash_time;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(when, sizeof when, ", %d %b at %H:%M", &tm);
        }
        snprintf(buf, sizeof buf, "Last: %s%s", crash_kind(in->last_crash_kind), when);
        char title[48];
        if (in->crashes_total == 1) snprintf(title, sizeof title, "Crashed once");
        else snprintf(title, sizeof title, "Crashed %u times", in->crashes_total);
        p->crash_row = row_create(body, T5UI_ICON_WARNING, title, buf);
        if (in->entry.autolaunch_blocked)
            sa_note(body, "It crashed several times in a row, so it won't start on its own (alarms, "
                          "background work) until you clear the record.");
        if (in->has_dump) {
            struct stat st;
            sa_fmt_bytes(a, sizeof a, stat(in->dump_path, &st) == 0 ? (uint64_t)st.st_size : 0);
            snprintf(buf, sizeof buf, "%s, %s", a, in->dump_path);
            row_create(body, T5UI_ICON_DOWNLOAD, "Core dump", buf);
            sa_note(body, "Read it on a computer with idf.py coredump-info.");
        }
        row = sa_button_row(body);
        button(row, T5UI_MD_BUTTON_TEXT, "Clear crash record", crash_reset_clicked, p);
    } else {
        sa_note(body, "No crashes.");
    }

    /* Developer details last: useful for logs and the t5 tool, noise for most. */
    section(body, "Details");
    row_create(body, T5UI_ICON_INFO, "App ID", id);
    if (!in->entry.builtin) {
        bool py = !strcmp(in->runtime, "micropython");
        row_create(body, T5UI_ICON_CODE, "Runtime", py ? "MicroPython" : "Native (C)");
        row_create(body, T5UI_ICON_FOLDER_OPEN, "Installed in", in->install_dir);
    }
    row_create(body, T5UI_ICON_FOLDER_OPEN, "Data folder", in->data_dir);
    sa_set_refresh(&s_nav, NULL, p, free);
}

/* ---- crash reports ----------------------------------------------------------------------- */
static void crash_clicked(t5ui_event_t *e)
{
    open_app(t5ui_event_user(e));
}

typedef struct {
    t5_crash_rec_t rec[16];
    t5_app_entry_t ents[T5_APPS_MAX];
} crashes_t;

static void open_crashes(t5ui_event_t *e)
{
    (void)e;
    crashes_t *c = calloc(1, sizeof *c);
    if (!c) return;
    t5ui_obj_t *body = sa_push(&s_nav, "Crash reports");
    int n = t5_apps_crashes(c->rec, 16);
    int ne = t5_apps_list(c->ents, T5_APPS_MAX);
    char buf[128];
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (!c->rec[i].total) continue;
        const char *name = c->rec[i].id;
        for (int k = 0; k < ne; k++)
            if (!strcmp(c->ents[k].id, c->rec[i].id) && c->ents[k].name[0]) name = c->ents[k].name;
        const char *dump = c->rec[i].has_dump ? ", core dump saved" : "";
        if (c->rec[i].total == 1) snprintf(buf, sizeof buf, "Once, %s%s", crash_kind(c->rec[i].last_kind), dump);
        else snprintf(buf, sizeof buf, "%u times, last %s%s", c->rec[i].total, crash_kind(c->rec[i].last_kind), dump);
        t5ui_obj_t *r = row_create(body, T5UI_ICON_WARNING, name, buf);
        t5ui_on(r, T5UI_EV_CLICK, crash_clicked, c->rec[i].id);
        shown++;
    }
    if (!shown) sa_note(body, "No app has crashed. Crashes and core dumps show up here.");
    sa_set_refresh(&s_nav, NULL, c, free);
}

/* ---- list ------------------------------------------------------------------------------------ */
typedef struct {
    t5_app_entry_t e[T5_APPS_MAX];
    int n;
    t5ui_obj_t *list;
    int shown;
} list_t;

static void app_clicked(t5ui_event_t *e)
{
    open_app(t5ui_event_user(e));
}

static void list_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    list_t *l = arg;
    l->n = t5_apps_list(l->e, T5_APPS_MAX);
    int h = l->n;
    for (int i = 0; i < l->n; i++) h = h * 31 + l->e[i].crashes + (l->e[i].id[0] << 3);
    if (h == l->shown) return;
    l->shown = h;
    t5ui_clean(l->list);
    char buf[96];
    section(l->list, "Installed");
    int inst = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) section(l->list, "Built in");
        for (int i = 0; i < l->n; i++) {
            t5_app_entry_t *a = &l->e[i];
            if (a->builtin != (pass == 1)) continue;
            inst += pass == 0;
            /* System apps only get a second line when it says something. */
            const char *state = a->autolaunch_blocked ? "Stopped after crashing" : a->running ? "Running" : NULL;
            if (a->builtin) snprintf(buf, sizeof buf, "%s", state ? state : "");
            else snprintf(buf, sizeof buf, "Version %s%s%s", a->version, state ? ", " : "",
                          state ? (a->autolaunch_blocked ? "stopped after crashing" : "running") : "");
            t5ui_obj_t *r = row_create(l->list, a->icon[0] ? a->icon : T5UI_ICON_EXTENSION, a->name,
                                       buf[0] ? buf : NULL);
            app_row_icon(r, a);
            t5ui_on(r, T5UI_EV_CLICK, app_clicked, a->id);
        }
        if (pass == 0 && !inst) sa_note(l->list, "No apps installed yet. Open a .t5app file in Files to install one.");
    }
}

static void on_create(t5_app_t *app)
{
    sa_nav_init(&s_nav, app);
    t5ui_obj_t *body = sa_push(&s_nav, "App Manager");
    t5_crash_rec_t *cr = calloc(16, sizeof *cr);
    int crashed = 0;
    if (cr) {
        int n = t5_apps_crashes(cr, 16);
        for (int i = 0; i < n; i++) crashed += cr[i].total > 0;
        free(cr);
    }
    char sub[48];
    if (crashed == 1) snprintf(sub, sizeof sub, "1 app has crashed");
    else if (crashed) snprintf(sub, sizeof sub, "%d apps have crashed", crashed);
    t5ui_obj_t *r = row_create(body, crashed ? T5UI_ICON_WARNING : T5UI_ICON_CHECK, "Crash reports",
                               crashed ? sub : "No crashes");
    t5ui_on(r, T5UI_EV_CLICK, open_crashes, NULL);
    list_t *l = calloc(1, sizeof *l);
    if (!l) return;
    l->shown = -1;
    l->list = t5ui_create(body, T5UI_BOX);
    t5ui_set_int(l->list, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(l->list, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(l->list, T5UI_P_FLOW, T5UI_COLUMN);
    sa_set_refresh(&s_nav, list_refresh, l, free);
    const char *arg = t5_app_launch_arg(app);
    if (arg[0]) open_app(arg);
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_nav_destroy(&s_nav);
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_appmanager = {
    .id = T5_APP_ID_MANAGER,
    .name = "App Manager",
    .desc = &s_desc,
    .perms = T5_PERM_STORAGE | T5_PERM_SYSTEM,
    .orientation = T5_ORIENT_ANY,
    .icon = T5UI_ICON_APPS,
};
