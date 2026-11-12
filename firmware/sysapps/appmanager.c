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
#include "t5_pkg/manifest.h"
#include "t5_shell/app_icon.h"
#include "t5_storage/paths.h"
#include "sysapps_priv.h"


static sa_nav_t s_nav;
static void open_app(const char *id);

/* ---- one app ------------------------------------------------------------------------- */
typedef struct {
    char id[64];
    t5_app_info_t info;
    lv_obj_t *crash_row;
} app_page_t;

static void grant_changed(lv_event_t *e)
{
    app_page_t *p = s_nav.lv[s_nav.depth - 1].arg;
    uint32_t perm = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    sa_report(t5_apps_set_grant(p->id, perm, on), "change the permission");
}

/* An installed app's own icon in place of a row's glyph: 40 dp, the
 * width of the glyph's slot, so the names still line up. */
static void app_row_icon(lv_obj_t *row, const t5_app_entry_t *a)
{
    lv_obj_t *img = row && !a->builtin ? t5_shell_app_icon_create(row, a->id, T5_DP(40)) : NULL;
    if (!img) return;
    lv_obj_set_hidden(lv_obj_get_child(row, 0), true);
    lv_obj_move_to_index(img, 0);
}

static void open_clicked(lv_event_t *e)
{
    app_page_t *p = lv_event_get_user_data(e);
    sa_report(t5_apps_launch(p->id), "open the app");
}

static void crash_reset_clicked(lv_event_t *e)
{
    app_page_t *p = lv_event_get_user_data(e);
    t5_err_t err = t5_apps_crash_reset(p->id);
    if (err != T5_OK) {
        sa_report(err, "clear the crash record");
        return;
    }
    md_snackbar_show("Crash record cleared", NULL, NULL, NULL);
    if (p->crash_row) md_row_set_secondary(p->crash_row, "No crashes since it was cleared");
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
        md_snackbar_show(msg, NULL, NULL, NULL);
        if (e == T5_OK) sa_pop(&s_nav);
    }
    free(r);
}

static void remove_clicked(lv_event_t *e)
{
    app_page_t *p = lv_event_get_user_data(e);
    remove_t *r = calloc(1, sizeof *r);
    if (!r) return;
    snprintf(r->id, sizeof r->id, "%s", p->id);
    snprintf(r->name, sizeof r->name, "%s", p->info.entry.name[0] ? p->info.entry.name : p->id);
    r->keep = true;
    char title[96];
    snprintf(title, sizeof title, "Remove %s?", p->info.entry.name);
    md_dialog_show(title, "Its saved data stays on the Tab5, so installing it again picks up where you left off.",
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
        md_snackbar_show("That app isn't installed any more", NULL, NULL, NULL);
        return;
    }
    const t5_app_info_t *in = &p->info;
    lv_obj_t *body = sa_push(&s_nav, in->entry.name[0] ? in->entry.name : id);
    char buf[256], a[24], b[24];
    if (in->entry.builtin) snprintf(buf, sizeof buf, "Part of Tab5 OS");
    else snprintf(buf, sizeof buf, "Version %s", in->entry.version);
    app_row_icon(md_row_create(body, in->entry.icon[0] ? in->entry.icon : MD_ICON_EXTENSION, in->entry.name, buf),
                 &in->entry);
    lv_obj_t *row = sa_button_row(body);
    lv_obj_t *bt = md_button_create(row, "Open", MD_BUTTON_CONTAINED);
    lv_obj_add_event_cb(bt, open_clicked, LV_EVENT_CLICKED, p);
    if (!in->entry.builtin) {
        bt = md_button_create(row, "Remove", MD_BUTTON_OUTLINED);
        lv_obj_add_event_cb(bt, remove_clicked, LV_EVENT_CLICKED, p);
    }

    md_section_create(body, "Storage");
    if (!in->entry.builtin) {
        sa_fmt_bytes(a, sizeof a, in->install_bytes);
        snprintf(buf, sizeof buf, "%s on %s", a, in->on_sd ? "the SD card" : "internal storage");
        md_row_create(body, in->on_sd ? MD_ICON_SD_STORAGE : MD_ICON_STORAGE, "App", buf);
    }
    sa_fmt_bytes(b, sizeof b, in->data_bytes);
    md_row_create(body, MD_ICON_FOLDER, "Saved data", in->data_bytes ? b : "Nothing saved yet");

    md_section_create(body, "Permissions");
    uint32_t perms = in->entry.perms;
    if (!perms) sa_note(body, "This app doesn't ask for any permissions.");
    for (uint32_t bit = 1; bit & T5_PERM_ALL; bit <<= 1) {
        if (!(perms & bit)) continue;
        const char *name = t5_perm_label(bit);
        if ((bit & t5_perm_prompted()) && !in->entry.builtin) {
            const char *state = (in->granted & bit) ? "Allowed" : (in->denied & bit) ? "Not allowed" : "Asks the first time";
            lv_obj_t *r = md_row_create(body, bit == T5_PERM_CAMERA ? MD_ICON_CAMERA_ALT : bit == T5_PERM_MIC ? MD_ICON_MIC : MD_ICON_FOLDER,
                                       name, state);
            lv_obj_t *sw = md_row_switch(r, (in->granted & bit) != 0);
            lv_obj_add_event_cb(sw, grant_changed, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)bit);
        } else {
            md_row_create(body, MD_ICON_CHECK, name, in->entry.builtin ? NULL : "Allowed when installed");
        }
    }
    /* Capabilities (USB device mode, Wi-Fi monitor...): only those the app
     * has asked for, each a switch like camera and mic. */
    for (uint32_t bit = T5_CORE_CAPS & -T5_CORE_CAPS; bit & T5_CORE_CAPS; bit <<= 1) {
        if (in->entry.builtin || !((in->granted | in->denied) & bit)) continue;
        lv_obj_t *r = md_row_create(body, MD_ICON_SETTINGS, t5_perm_label(bit),
                                    (in->granted & bit) ? "Allowed" : "Not allowed");
        lv_obj_t *sw = md_row_switch(r, (in->granted & bit) != 0);
        lv_obj_add_event_cb(sw, grant_changed, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)bit);
    }
    if (!in->entry.builtin) sa_note(body, T5_NATIVE_APP_WARNING);

    md_section_create(body, "Power");
    t5_energy_rec_t rec[T5_ENERGY_MAX];
    int n = t5_apps_energy(rec, T5_ENERGY_MAX);
    snprintf(buf, sizeof buf, "Not measured yet. Use it on battery to get a reading.");
    for (int i = 0; i < n; i++) {
        if (!strcmp(rec[i].id, id) && t5_energy_avg_mw(&rec[i]) >= 0) {
            sa_fmt_duration(a, sizeof a, rec[i].ms / 1000);
            snprintf(buf, sizeof buf, "%d mW on average, over %s of use", (int)t5_energy_avg_mw(&rec[i]), a);
        }
    }
    md_row_create(body, MD_ICON_BOLT, "Average draw", buf);

    md_section_create(body, "Crashes");
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
        p->crash_row = md_row_create(body, MD_ICON_WARNING, title, buf);
        if (in->entry.autolaunch_blocked)
            sa_note(body, "It crashed several times in a row, so it won't start on its own (alarms, "
                          "background work) until you clear the record.");
        if (in->has_dump) {
            struct stat st;
            sa_fmt_bytes(a, sizeof a, stat(in->dump_path, &st) == 0 ? (uint64_t)st.st_size : 0);
            snprintf(buf, sizeof buf, "%s, %s", a, in->dump_path);
            md_row_create(body, MD_ICON_DOWNLOAD, "Core dump", buf);
            sa_note(body, "Read it on a computer with idf.py coredump-info.");
        }
        row = sa_button_row(body);
        bt = md_button_create(row, "Clear crash record", MD_BUTTON_TEXT);
        lv_obj_add_event_cb(bt, crash_reset_clicked, LV_EVENT_CLICKED, p);
    } else {
        sa_note(body, "No crashes.");
    }

    /* Developer details last: useful for logs and the t5 tool, noise for most. */
    md_section_create(body, "Details");
    md_row_create(body, MD_ICON_INFO, "App ID", id);
    if (!in->entry.builtin) {
        bool py = !strcmp(in->runtime, "micropython");
        md_row_create(body, MD_ICON_CODE, "Runtime", py ? "MicroPython" : "Native (C)");
        md_row_create(body, MD_ICON_FOLDER_OPEN, "Installed in", in->install_dir);
    }
    md_row_create(body, MD_ICON_FOLDER_OPEN, "Data folder", in->data_dir);
    sa_set_refresh(&s_nav, NULL, p, free);
}

/* ---- crash reports ----------------------------------------------------------------------- */
static void crash_clicked(lv_event_t *e)
{
    open_app(lv_event_get_user_data(e));
}

typedef struct {
    t5_crash_rec_t rec[16];
    t5_app_entry_t ents[T5_APPS_MAX];
} crashes_t;

static void open_crashes(lv_event_t *e)
{
    (void)e;
    crashes_t *c = calloc(1, sizeof *c);
    if (!c) return;
    lv_obj_t *body = sa_push(&s_nav, "Crash reports");
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
        lv_obj_t *r = md_row_create(body, MD_ICON_WARNING, name, buf);
        md_row_set_clickable(r, crash_clicked, c->rec[i].id);
        shown++;
    }
    if (!shown) sa_note(body, "No app has crashed. Crashes and core dumps show up here.");
    sa_set_refresh(&s_nav, NULL, c, free);
}

/* ---- list ------------------------------------------------------------------------------------ */
typedef struct {
    t5_app_entry_t e[T5_APPS_MAX];
    int n;
    lv_obj_t *list;
    int shown;
} list_t;

static void app_clicked(lv_event_t *e)
{
    open_app(lv_event_get_user_data(e));
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
    lv_obj_clean(l->list);
    char buf[96];
    md_section_create(l->list, "Installed");
    int inst = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) md_section_create(l->list, "Built in");
        for (int i = 0; i < l->n; i++) {
            t5_app_entry_t *a = &l->e[i];
            if (a->builtin != (pass == 1)) continue;
            inst += pass == 0;
            /* System apps only get a second line when it says something. */
            const char *state = a->autolaunch_blocked ? "Stopped after crashing" : a->running ? "Running" : NULL;
            if (a->builtin) snprintf(buf, sizeof buf, "%s", state ? state : "");
            else snprintf(buf, sizeof buf, "Version %s%s%s", a->version, state ? ", " : "",
                          state ? (a->autolaunch_blocked ? "stopped after crashing" : "running") : "");
            lv_obj_t *r = md_row_create(l->list, a->icon[0] ? a->icon : MD_ICON_EXTENSION, a->name,
                                        buf[0] ? buf : NULL);
            app_row_icon(r, a);
            md_row_set_clickable(r, app_clicked, a->id);
        }
        if (pass == 0 && !inst) sa_note(l->list, "No apps installed yet. Open a .t5app file in Files to install one.");
    }
}

static void on_create(t5_app_t *app)
{
    sa_nav_init(&s_nav, app);
    lv_obj_t *body = sa_push(&s_nav, "App Manager");
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
    lv_obj_t *r = md_row_create(body, crashed ? MD_ICON_WARNING : MD_ICON_CHECK, "Crash reports",
                                crashed ? sub : "No crashes");
    md_row_set_clickable(r, open_crashes, NULL);
    list_t *l = calloc(1, sizeof *l);
    if (!l) return;
    l->shown = -1;
    l->list = lv_obj_create(body);
    lv_obj_remove_style_all(l->list);
    lv_obj_set_size(l->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(l->list, LV_FLEX_FLOW_COLUMN);
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
    .icon = MD_ICON_APPS,
};
