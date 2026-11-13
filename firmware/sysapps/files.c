/* SPDX-License-Identifier: Apache-2.0
 * Files (plan 6.6): browse /sd, /data and a USB drive at /usb; install a
 * .t5app by tapping it; view photos and play videos (.avi); delete;
 * safe eject of the SD card and the USB drive. The launch argument opens a
 * directory.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "t5/storage.h"
#include "t5/system.h"
#include "t5/usb.h"
#include "t5_gui/t5ui_icons.h"
#include "t5_gui/t5ui_sys.h"
#include "sysapps_priv.h"

#define MAX_ENTRIES 512

typedef struct {
    char name[64];
    bool dir;
    uint64_t size;
} entry_t;

typedef struct {
    char path[192];
    entry_t *e;
    int n;
    t5ui_obj_t *list, *empty;
} dir_page_t;

static sa_nav_t s_nav;

static int by_name(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool ends_with(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && !strcasecmp(s + n - m, suf);
}

static const char *icon_for(const entry_t *e)
{
    if (e->dir) return T5UI_ICON_FOLDER;
    if (ends_with(e->name, ".t5app")) return T5UI_ICON_EXTENSION;
    if (ends_with(e->name, ".jpg") || ends_with(e->name, ".jpeg") || ends_with(e->name, ".png")) return T5UI_ICON_IMAGE;
    if (ends_with(e->name, ".h264") || ends_with(e->name, ".mp4") || ends_with(e->name, ".avi")) return T5UI_ICON_VIDEOCAM;
    if (ends_with(e->name, ".wav") || ends_with(e->name, ".mp3")) return T5UI_ICON_MUSIC_NOTE;
    return T5UI_ICON_INSERT_DRIVE_FILE;
}

static void load(dir_page_t *p)
{
    free(p->e);
    p->e = NULL;
    p->n = 0;
    DIR *d = opendir(p->path);
    if (!d) return;
    p->e = calloc(MAX_ENTRIES, sizeof *p->e);
    struct dirent *de;
    char full[256];
    while (p->e && p->n < MAX_ENTRIES && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        entry_t *e = &p->e[p->n];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        snprintf(full, sizeof full, "%s/%s", p->path, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            e->dir = S_ISDIR(st.st_mode);
            e->size = (uint64_t)st.st_size;
        } else {
            e->dir = de->d_type == DT_DIR;
        }
        p->n++;
    }
    closedir(d);
    if (p->n) qsort(p->e, (size_t)p->n, sizeof *p->e, by_name);
}

static void bind(t5ui_event_t *ev)
{
    dir_page_t *p = t5ui_event_user(ev);
    t5ui_obj_t *r = t5ui_event_target(ev);
    int i = (int)t5ui_event_index(ev);
    if (i < 0 || i >= p->n) return;
    entry_t *e = &p->e[i];
    t5ui_set_str(r, T5UI_P_TEXT, e->name);
    t5ui_set_str(r, T5UI_P_ICON, icon_for(e));
    /* Two-line rows: a one-line row has no second line, and this does nothing. */
    char b[32];
    if (e->dir) snprintf(b, sizeof b, "Folder");
    else sa_fmt_bytes(b, sizeof b, e->size);
    t5ui_set_str(r, T5UI_P_SECONDARY, b);
}

static void open_dir(const char *path);

/* ---- install --------------------------------------------------------------------- */
typedef struct {
    char path[256];
} install_t;

static void install_answer(int b, void *ud)
{
    install_t *in = ud;
    if (b == 0) {
        char err[128] = "";
        t5_manifest_t *man = calloc(1, sizeof *man);
        t5_err_t e = t5_apps_install(in->path, T5_PKG_DEST_AUTO, man, err, sizeof err);
        char msg[192];
        if (e == T5_OK && man && man->name[0])
            snprintf(msg, sizeof msg, "%s installed. Find it in your apps.", man->name);
        else if (e == T5_OK) snprintf(msg, sizeof msg, "Installed. Find it in your apps.");
        else snprintf(msg, sizeof msg, "Couldn't install: %s", err[0] ? err : t5_err_str(e));
        free(man);
        t5ui_snackbar(msg, NULL, NULL, NULL);
    }
    free(in);
}

static void ask_install(const char *path)
{
    install_t *in = calloc(1, sizeof *in);
    if (!in) return;
    snprintf(in->path, sizeof in->path, "%s", path);
    const char *base = strrchr(path, '/');
    char title[96];
    snprintf(title, sizeof title, "Install %s?", base ? base + 1 : path);
    t5ui_dialog(title, T5_NATIVE_APP_WARNING, "Install", "Cancel", install_answer, in);
}

/* ---- background jobs: delete and format ------------------------------------------
 * Deleting a big folder or formatting a card takes seconds, longer than the
 * UI watchdog allows, so both run on a task. Formats run as system tasks:
 * leaving the app mustn't stop one halfway. */
typedef enum { JOB_DELETE, JOB_FORMAT_SD, JOB_FORMAT_USB } job_kind_t;

typedef struct {
    job_kind_t kind;
    char path[256];
    t5_err_t err;
} job_t;

static t5_app_t *s_app;
static volatile bool s_busy;

static void dir_refresh(sa_nav_t *nav, void *arg);

static void reload_top(void)
{
    if (!s_nav.depth) return;
    sa_level_t *l = &s_nav.lv[s_nav.depth - 1];
    if (l->refresh == dir_refresh && l->arg) {
        dir_page_t *p = l->arg;
        load(p);
        t5ui_set_int(p->list, T5UI_P_COUNT, p->n);
        t5ui_cmd(p->list, T5UI_CMD_LIST_REFRESH, 0, 0, 0, NULL);
        t5ui_set_int(p->empty, T5UI_P_HIDDEN, p->n > 0);
    }
}

static int rm_tree(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode) && depth < 12) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            char sub[256];
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                if (snprintf(sub, sizeof sub, "%s/%s", path, de->d_name) >= (int)sizeof sub) continue;
                rm_tree(sub, depth + 1);
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return remove(path);
}

static void job_done(void *arg)
{
    job_t *j = arg;
    s_busy = false;
    if (s_app && t5_app_running(s_app)) {
        static const char *ok[] = { "Deleted", "SD card formatted", "USB drive formatted" };
        static const char *bad[] = { "Couldn't delete everything: some files are still there",
                                     "Couldn't format the SD card", "Couldn't format the USB drive" };
        t5ui_snackbar(j->err == T5_OK ? ok[j->kind] : bad[j->kind], NULL, NULL, NULL);
        if (j->kind == JOB_DELETE) reload_top();
        else sa_pop_to_root(&s_nav);
    }
    free(j);
}

static void job_task(void *arg)
{
    job_t *j = arg;
    switch (j->kind) {
    case JOB_DELETE: j->err = rm_tree(j->path, 0) == 0 ? T5_OK : T5_ERR_IO; break;
    case JOB_FORMAT_SD: j->err = t5_storage_sd_format(); break;
    case JOB_FORMAT_USB: j->err = t5_usb_storage_format(); break;
    }
    if (t5ui_async(job_done, j) != T5_OK) {
        s_busy = false;
        free(j);
    }
}

static void start_job(job_kind_t kind, const char *path)
{
    if (s_busy) {
        t5ui_snackbar("Wait for the current delete or format to finish", NULL, NULL, NULL);
        return;
    }
    job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = kind;
    snprintf(j->path, sizeof j->path, "%s", path ? path : "");
    s_busy = true;
    static const char *msg[] = { "Deleting...", "Formatting the SD card...", "Formatting the USB drive..." };
    t5ui_snackbar(msg[kind], NULL, NULL, NULL);
    void *ctx = kind == JOB_DELETE ? NULL : t5_sys_enter();
    t5_err_t e = t5_task_create("files.job", job_task, j, 6144, 4, T5_CORE_ANY, NULL);
    if (kind != JOB_DELETE) t5_sys_leave(ctx);
    if (e != T5_OK) {
        s_busy = false;
        free(j);
    }
}

typedef struct {
    char path[256];
    bool dir;
} menu_ctx_t;

static void delete_answer(int b, void *ud)
{
    menu_ctx_t *m = ud;
    if (b == 0) start_job(JOB_DELETE, m->path);
    free(m);
}

static void ask_delete(const char *path, bool dir)
{
    menu_ctx_t *m = calloc(1, sizeof *m);
    if (!m) return;
    snprintf(m->path, sizeof m->path, "%s", path);
    char title[96];
    const char *base = strrchr(path, '/');
    snprintf(title, sizeof title, "Delete %s?", base ? base + 1 : path);
    t5ui_dialog(title, dir ? "The folder and everything in it are deleted. This can't be undone."
                              : "The file is deleted. This can't be undone.",
                "Delete", "Cancel", delete_answer, m);
}

static void format_answer(int b, void *ud)
{
    if (b == 0) start_job((job_kind_t)(intptr_t)ud, NULL);
}

static void ask_format(bool usb)
{
    if (usb ? !t5_usb_storage_mounted() : t5_storage_sd_state() != T5_SD_MOUNTED) {
        t5ui_snackbar(usb ? "No USB drive to format" : "No SD card to format", NULL, NULL, NULL);
        return;
    }
    t5ui_dialog(usb ? "Format the USB drive?" : "Format the SD card?",
                usb ? "Everything on the drive is erased, and it gets one FAT partition. This can't be undone."
                    : "Everything on the card is erased, including apps installed on it and photos. "
                      "This can't be undone.",
                "Format", "Cancel", format_answer, (void *)(intptr_t)(usb ? JOB_FORMAT_USB : JOB_FORMAT_SD));
}

static bool is_drive_root(const char *path, bool *usb)
{
    *usb = !strcmp(path, T5_USB_PATH);
    return *usb || !strcmp(path, T5_SD_PATH);
}

static void entry_menu_picked(int item, void *ud)
{
    menu_ctx_t *m = ud;
    if (item == 0) {
        if (m->dir) open_dir(m->path);
        else if (ends_with(m->path, ".t5app")) ask_install(m->path);
        else if (sa_photo_can_open(m->path)) sa_photo_open(t5ui_root(s_app), m->path, NULL);
        else if (sa_video_can_open(m->path)) sa_video_open(t5ui_root(s_app), m->path, NULL);
        else t5ui_snackbar("No app on the Tab5 opens this kind of file", NULL, NULL, NULL);
    } else if (item == 1) {
        ask_delete(m->path, m->dir);
    }
    free(m);
}

static void entry_menu(dir_page_t *p, uint32_t i)
{
    menu_ctx_t *m = calloc(1, sizeof *m);
    if (!m) return;
    snprintf(m->path, sizeof m->path, "%s/%s", p->path, p->e[i].name);
    m->dir = p->e[i].dir;
    static const char *items[] = { "Open", "Delete" };
    /* Anchored to the whole screen: the top right corner, as a menu with no anchor. */
    t5ui_menu(t5ui_sys_layer(T5UI_LAYER_TOP), items, 2, entry_menu_picked, m);
}

static void clicked(t5ui_event_t *ev)
{
    dir_page_t *p = t5ui_event_user(ev);
    int i = (int)t5ui_event_index(ev);
    if (i < 0 || i >= p->n) return;
    entry_t *e = &p->e[i];
    char full[256];
    snprintf(full, sizeof full, "%s/%s", p->path, e->name);
    if (e->dir) open_dir(full);
    else if (ends_with(e->name, ".t5app")) ask_install(full);
    else if (sa_photo_can_open(full)) sa_photo_open(t5ui_root(s_app), full, NULL);
    else if (sa_video_can_open(full)) sa_video_open(t5ui_root(s_app), full, NULL);
    else entry_menu(p, (uint32_t)i);
}

static void long_pressed(t5ui_event_t *ev)
{
    dir_page_t *p = t5ui_event_user(ev);
    int i = (int)t5ui_event_index(ev);
    if (i >= 0 && i < p->n) entry_menu(p, (uint32_t)i);
}

static void folder_menu_picked(int item, void *ud)
{
    (void)ud;
    dir_page_t *p = s_nav.depth ? s_nav.lv[s_nav.depth - 1].arg : NULL;
    if (!p) return;
    bool usb;
    if (item == 0) reload_top();
    else if (item == 1 && is_drive_root(p->path, &usb)) ask_format(usb);
    else if (item == 1) ask_delete(p->path, true);
}

static void more_clicked(t5ui_event_t *e)
{
    dir_page_t *p = s_nav.lv[s_nav.depth - 1].arg;
    bool usb;
    static const char *folder[] = { "Refresh", "Delete this folder" };
    static const char *drive[] = { "Refresh", "Format this drive" };
    t5ui_menu(t5ui_event_obj(e), is_drive_root(p->path, &usb) ? drive : folder, 2, folder_menu_picked, NULL);
}

static void eject(bool usb)
{
    t5_err_t err = usb ? t5_usb_storage_eject() : t5_storage_sd_eject();
    char msg[96];
    if (err == T5_OK) snprintf(msg, sizeof msg, "%s ejected. You can remove it now.", usb ? "USB drive" : "SD card");
    else snprintf(msg, sizeof msg, "Couldn't eject the %s (%s)", usb ? "USB drive" : "SD card", t5_err_str(err));
    t5ui_snackbar(msg, NULL, NULL, NULL);
}

static void eject_clicked(t5ui_event_t *e)
{
    (void)e;
    dir_page_t *p = s_nav.lv[s_nav.depth - 1].arg;
    eject(!strncmp(p->path, T5_USB_PATH, strlen(T5_USB_PATH)));
    sa_pop_to_root(&s_nav);
}

static void dir_free(void *arg)
{
    dir_page_t *p = arg;
    free(p->e);
    free(p);
}

static void dir_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav; (void)arg;          /* marks a directory page; reload_top() finds it */
}

/* A drive's name at its root, else the folder's own name: full paths
 * don't fit a portrait app bar and mean little to most people. */
static const char *title_for(const char *path)
{
    if (!strcmp(path, T5_SD_PATH)) return "SD card";
    if (!strcmp(path, T5_USB_PATH)) return "USB drive";
    if (!strcmp(path, T5_DATA_PATH)) return "Internal storage";
    const char *base = strrchr(path, '/');
    return base && base[1] ? base + 1 : path;
}

static void open_dir(const char *path)
{
    dir_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    snprintf(p->path, sizeof p->path, "%s", path);
    load(p);
    t5ui_obj_t *body = sa_push(&s_nav, title_for(path));
    t5ui_set_int(body, T5UI_P_SCROLLABLE, false);
    t5ui_obj_t *bar = sa_bar(&s_nav);
    bool removable = !strncmp(path, T5_SD_PATH, strlen(T5_SD_PATH)) || !strncmp(path, T5_USB_PATH, strlen(T5_USB_PATH));
    if (removable) {
        t5ui_obj_t *a = t5ui_create(bar, T5UI_MD_APPBAR_ACTION);
        t5ui_set_str(a, T5UI_P_ICON, T5UI_ICON_EJECT);
        t5ui_on(a, T5UI_EV_CLICK, eject_clicked, NULL);
    }
    t5ui_obj_t *more = t5ui_create(bar, T5UI_MD_APPBAR_ACTION);
    t5ui_set_str(more, T5UI_P_ICON, T5UI_ICON_MORE_VERT);
    t5ui_on(more, T5UI_EV_CLICK, more_clicked, NULL);
    p->list = t5ui_create(body, T5UI_MD_LIST);
    t5ui_on(p->list, T5UI_EV_BIND, bind, p);
    t5ui_on(p->list, T5UI_EV_ITEM, clicked, p);
    t5ui_on(p->list, T5UI_EV_ITEM_LONG, long_pressed, p);
    t5ui_set_int(p->list, T5UI_P_ROW_DP, 64);           /* two-line rows */
    t5ui_set_int(p->list, T5UI_P_COUNT, p->n);
    t5ui_set_int(p->list, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(p->list, T5UI_P_GROW, 1);
    p->empty = sa_note(body, "This folder is empty.");
    t5ui_set_int(p->empty, T5UI_P_HIDDEN, p->n > 0);
    sa_set_refresh(&s_nav, dir_refresh, p, dir_free);
}

/* ---- roots --------------------------------------------------------------------------- */
typedef struct {
    t5ui_obj_t *sd, *data, *usb;
    int shown_sd, shown_usb;
} root_t;

static void root_clicked(t5ui_event_t *e)
{
    const char *path = t5ui_event_user(e);
    if (!strcmp(path, T5_SD_PATH) && t5_storage_sd_state() != T5_SD_MOUNTED) {
        if (t5_storage_sd_rescan() != T5_OK) {
            bool bad = t5_storage_sd_state() == T5_SD_FAILED;
            t5ui_snackbar(bad ? "Couldn't read the SD card. Long press to format it."
                              : "No SD card. Insert one, then tap again.",
                          NULL, NULL, NULL);
            return;
        }
    }
    if (!strcmp(path, T5_USB_PATH) && !t5_usb_storage_mounted()) {
        t5ui_snackbar("No USB drive. Plug one into the USB-A port.", NULL, NULL, NULL);
        return;
    }
    open_dir(path);
}

static void drive_menu_picked(int item, void *ud)
{
    bool usb = ud != NULL;
    if (item == 0) {
        open_dir(usb ? T5_USB_PATH : T5_SD_PATH);
    } else if (item == 1) {
        eject(usb);
    } else if (item == 2) {
        ask_format(usb);
    }
}

static void drive_long(t5ui_event_t *e)
{
    t5ui_cmd(t5ui_event_obj(e), T5UI_CMD_IGNORE_PRESS, 0, 0, 0, NULL);
    static const char *items[] = { "Open", "Eject", "Format" };
    t5ui_menu(t5ui_event_obj(e), items, 3, drive_menu_picked, t5ui_event_user(e));
}

static void root_refresh(sa_nav_t *nav, void *arg)
{
    (void)nav;
    root_t *r = arg;
    char buf[96], a[24], b[24];
    t5_sd_state_t sd = t5_storage_sd_state();
    t5_sd_info_t info;
    if (sd == T5_SD_MOUNTED && t5_storage_sd_info(&info) == T5_OK) {
        sa_fmt_bytes(a, sizeof a, info.free_bytes);
        sa_fmt_bytes(b, sizeof b, info.total_bytes);
        snprintf(buf, sizeof buf, "%s free of %s", a, b);
    } else {
        static const char *st[] = { [T5_SD_ABSENT] = "No card inserted", [T5_SD_MOUNTED] = "Reading the card",
                                    [T5_SD_FAILED] = "Can't read this card. Long press to format it.",
                                    [T5_SD_EJECTED] = "Ejected. Tap to use it again." };
        bool known = (unsigned)sd < sizeof st / sizeof st[0] && st[sd];
        snprintf(buf, sizeof buf, "%s", known ? st[sd] : t5_storage_sd_state_str(sd));
    }
    t5ui_set_str(r->sd, T5UI_P_SECONDARY, buf);
    uint64_t total = 0, used = 0;
    if (t5_storage_data_usage(&total, &used) == T5_OK) {
        sa_fmt_bytes(a, sizeof a, total - used);
        sa_fmt_bytes(b, sizeof b, total);
        snprintf(buf, sizeof buf, "%s free of %s", a, b);
        t5ui_set_str(r->data, T5UI_P_SECONDARY, buf);
    }
    t5ui_set_str(r->usb, T5UI_P_SECONDARY,
                 t5_usb_storage_mounted() ? "Connected" : "Plug a drive into the USB-A port");
}

/* A drive's row: its icon and name, a line for its state, a tap opens it. */
static t5ui_obj_t *drive_row(t5ui_obj_t *body, const char *icon, const char *name, const char *path)
{
    t5ui_obj_t *row = t5ui_create(body, T5UI_MD_ROW);
    t5ui_set_str(row, T5UI_P_ICON, icon);
    t5ui_set_str(row, T5UI_P_TEXT, name);
    t5ui_set_str(row, T5UI_P_SECONDARY, " ");
    t5ui_on(row, T5UI_EV_CLICK, root_clicked, (void *)path);
    return row;
}

static void on_create(t5_app_t *app)
{
    s_app = app;
    sa_nav_init(&s_nav, app);
    t5ui_obj_t *body = sa_push(&s_nav, "Files");
    root_t *r = calloc(1, sizeof *r);
    if (!r) return;
    r->sd = drive_row(body, T5UI_ICON_SD_STORAGE, "SD card", T5_SD_PATH);
    t5ui_on(r->sd, T5UI_EV_LONG_PRESS, drive_long, NULL);
    r->usb = drive_row(body, T5UI_ICON_USB, "USB drive", T5_USB_PATH);
    t5ui_on(r->usb, T5UI_EV_LONG_PRESS, drive_long, (void *)1);
    r->data = drive_row(body, T5UI_ICON_STORAGE, "Internal storage", T5_DATA_PATH);
    sa_note(body, "Open a .t5app file to install it. Long press a drive, file or folder for more options.");
    sa_set_refresh(&s_nav, root_refresh, r, free);
    const char *arg = t5_app_launch_arg(app);
    if (arg[0] == '/') open_dir(arg);
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_photo_close();
    sa_video_close();
    s_app = NULL;                  /* a running format reports to nobody */
    sa_nav_destroy(&s_nav);
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_files = {
    .id = T5_APP_ID_FILES,
    .name = "Files",
    .desc = &s_desc,
    .perms = T5_PERM_STORAGE | T5_PERM_USB | T5_PERM_SYSTEM,
    .orientation = T5_ORIENT_ANY,
    .icon = T5UI_ICON_FOLDER,
};
