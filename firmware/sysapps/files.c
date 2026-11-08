/* SPDX-License-Identifier: Apache-2.0
 * Files (plan 6.6): browse /sd, /data and a USB drive at /usb; install a
 * .t5app by tapping it; open photos in the Camera app's gallery; delete;
 * safe eject of the SD card and the USB drive. The launch argument opens a
 * directory.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "t5/shell.h"
#include "t5/storage.h"
#include "t5/system.h"
#include "sysapp_system.h"
#include "t5/usb.h"
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
    lv_obj_t *list, *empty;
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
    if (e->dir) return MD_ICON_FOLDER;
    if (ends_with(e->name, ".t5app")) return MD_ICON_EXTENSION;
    if (ends_with(e->name, ".jpg") || ends_with(e->name, ".jpeg") || ends_with(e->name, ".png")) return MD_ICON_IMAGE;
    if (ends_with(e->name, ".h264") || ends_with(e->name, ".mp4")) return MD_ICON_VIDEOCAM;
    if (ends_with(e->name, ".wav") || ends_with(e->name, ".mp3")) return MD_ICON_MUSIC_NOTE;
    return MD_ICON_INSERT_DRIVE_FILE;
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

static void bind(md_list_row_t *r, uint32_t i, void *ud)
{
    dir_page_t *p = ud;
    if ((int)i >= p->n) return;
    entry_t *e = &p->e[i];
    lv_label_set_text(r->primary, e->name);
    lv_label_set_text(r->icon, icon_for(e));
    if (r->secondary) {
        char b[32];
        if (e->dir) snprintf(b, sizeof b, "Folder");
        else sa_fmt_bytes(b, sizeof b, e->size);
        lv_label_set_text(r->secondary, b);
    }
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
        t5_err_t e = t5_apps_install(in->path, 0, err, sizeof err);
        char msg[192];
        if (e == T5_OK) snprintf(msg, sizeof msg, "Installed. It's in the launcher.");
        else snprintf(msg, sizeof msg, "Couldn't install: %s", err[0] ? err : t5_err_str(e));
        md_snackbar_show(msg, NULL, NULL, NULL);
    }
    free(in);
}

static void ask_install(const char *path)
{
    install_t *in = calloc(1, sizeof *in);
    if (!in) return;
    snprintf(in->path, sizeof in->path, "%s", path);
    md_dialog_show("Install this app?", T5_NATIVE_APP_WARNING, "Install", "Cancel", install_answer, in);
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
        md_list_set_count(p->list, (uint32_t)p->n);
        md_list_refresh(p->list);
        lv_obj_set_hidden(p->empty, p->n > 0);
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
        static const char *bad[] = { "Couldn't delete all of it", "Formatting the SD card failed",
                                     "Formatting the USB drive failed" };
        md_snackbar_show(j->err == T5_OK ? ok[j->kind] : bad[j->kind], NULL, NULL, NULL);
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
    if (t5_ui_async(job_done, j) != T5_OK) {
        s_busy = false;
        free(j);
    }
}

static void start_job(job_kind_t kind, const char *path)
{
    if (s_busy) {
        md_snackbar_show("Still working on the last one", NULL, NULL, NULL);
        return;
    }
    job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->kind = kind;
    snprintf(j->path, sizeof j->path, "%s", path ? path : "");
    s_busy = true;
    static const char *msg[] = { "Deleting...", "Formatting the SD card...", "Formatting the USB drive..." };
    md_snackbar_show(msg[kind], NULL, NULL, NULL);
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
    md_dialog_show(title, dir ? "The folder and everything in it are deleted. This can't be undone."
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
        md_snackbar_show(usb ? "No USB drive" : "No SD card", NULL, NULL, NULL);
        return;
    }
    md_dialog_show(usb ? "Format the USB drive?" : "Format the SD card?",
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
        else if (sa_photo_can_open(m->path)) sa_photo_open(t5_app_screen(s_app), m->path, NULL);
        else md_snackbar_show("No app opens this kind of file", NULL, NULL, NULL);
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
    md_menu_show(NULL, items, 2, entry_menu_picked, m);
}

static void clicked(uint32_t i, void *ud)
{
    dir_page_t *p = ud;
    if ((int)i >= p->n) return;
    entry_t *e = &p->e[i];
    char full[256];
    snprintf(full, sizeof full, "%s/%s", p->path, e->name);
    if (e->dir) open_dir(full);
    else if (ends_with(e->name, ".t5app")) ask_install(full);
    else if (sa_photo_can_open(full)) sa_photo_open(t5_app_screen(s_app), full, NULL);
    else entry_menu(p, i);
}

static void long_pressed(uint32_t i, void *ud)
{
    dir_page_t *p = ud;
    if ((int)i < p->n) entry_menu(p, i);
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

static void more_clicked(lv_event_t *e)
{
    dir_page_t *p = s_nav.lv[s_nav.depth - 1].arg;
    bool usb;
    static const char *folder[] = { "Refresh", "Delete this folder" };
    static const char *drive[] = { "Refresh", "Format this drive" };
    md_menu_show(lv_event_get_current_target_obj(e), is_drive_root(p->path, &usb) ? drive : folder, 2,
                 folder_menu_picked, NULL);
}

static void eject_clicked(lv_event_t *e)
{
    (void)e;
    dir_page_t *p = s_nav.lv[s_nav.depth - 1].arg;
    t5_err_t err;
    if (!strncmp(p->path, T5_USB_PATH, strlen(T5_USB_PATH))) err = t5_usb_storage_eject();
    else err = t5_storage_sd_eject();
    md_snackbar_show(err == T5_OK ? "Ejected. It's safe to remove." : "Nothing to eject", NULL, NULL, NULL);
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

static void open_dir(const char *path)
{
    dir_page_t *p = calloc(1, sizeof *p);
    if (!p) return;
    snprintf(p->path, sizeof p->path, "%s", path);
    load(p);
    lv_obj_t *body = sa_push(&s_nav, path);
    lv_obj_set_scrollable(body, false);
    lv_obj_t *bar = sa_bar(&s_nav);
    bool removable = !strncmp(path, T5_SD_PATH, strlen(T5_SD_PATH)) || !strncmp(path, T5_USB_PATH, strlen(T5_USB_PATH));
    if (removable) md_appbar_add_action(bar, MD_ICON_EJECT, eject_clicked, NULL);
    md_appbar_add_action(bar, MD_ICON_MORE_VERT, more_clicked, NULL);
    p->list = md_list_create(body, (uint32_t)p->n, MD_LIST_ROW_2_DP, bind, clicked, p);
    md_list_set_long_press(p->list, long_pressed);
    lv_obj_set_width(p->list, LV_PCT(100));
    lv_obj_set_flex_grow(p->list, 1);
    p->empty = sa_note(body, "This folder is empty.");
    lv_obj_set_hidden(p->empty, p->n > 0);
    sa_set_refresh(&s_nav, dir_refresh, p, dir_free);
}

/* ---- roots --------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *sd, *data, *usb;
    int shown_sd, shown_usb;
} root_t;

static void root_clicked(lv_event_t *e)
{
    const char *path = lv_event_get_user_data(e);
    if (!strcmp(path, T5_SD_PATH) && t5_storage_sd_state() != T5_SD_MOUNTED) {
        if (t5_storage_sd_rescan() != T5_OK) {
            md_snackbar_show("No SD card", NULL, NULL, NULL);
            return;
        }
    }
    if (!strcmp(path, T5_USB_PATH) && !t5_usb_storage_mounted()) {
        md_snackbar_show("No USB drive", NULL, NULL, NULL);
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
        t5_err_t err = usb ? t5_usb_storage_eject() : t5_storage_sd_eject();
        md_snackbar_show(err == T5_OK ? "Ejected. It's safe to remove." : "Nothing to eject", NULL, NULL, NULL);
    } else if (item == 2) {
        ask_format(usb);
    }
}

static void drive_long(lv_event_t *e)
{
    lv_indev_wait_release(lv_indev_active());
    static const char *items[] = { "Open", "Eject", "Format..." };
    md_menu_show(lv_event_get_current_target_obj(e), items, 3, drive_menu_picked, lv_event_get_user_data(e));
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
        snprintf(buf, sizeof buf, "%s%s", t5_storage_sd_state_str(sd), sd == T5_SD_EJECTED ? " (tap to mount)" : "");
    }
    md_row_set_secondary(r->sd, buf);
    uint64_t total = 0, used = 0;
    if (t5_storage_data_usage(&total, &used) == T5_OK) {
        sa_fmt_bytes(a, sizeof a, total - used);
        sa_fmt_bytes(b, sizeof b, total);
        snprintf(buf, sizeof buf, "%s free of %s (internal flash)", a, b);
        md_row_set_secondary(r->data, buf);
    }
    md_row_set_secondary(r->usb, t5_usb_storage_mounted() ? "USB drive" : "Nothing plugged in");
}

static void on_create(t5_app_t *app)
{
    s_app = app;
    sa_nav_init(&s_nav, app);
    lv_obj_t *body = sa_push(&s_nav, "Files");
    root_t *r = calloc(1, sizeof *r);
    if (!r) return;
    r->sd = md_row_create(body, MD_ICON_SD_STORAGE, "SD card", " ");
    md_row_set_clickable(r->sd, root_clicked, (void *)T5_SD_PATH);
    lv_obj_add_event_cb(r->sd, drive_long, LV_EVENT_LONG_PRESSED, NULL);
    r->usb = md_row_create(body, MD_ICON_USB, "USB drive", " ");
    md_row_set_clickable(r->usb, root_clicked, (void *)T5_USB_PATH);
    lv_obj_add_event_cb(r->usb, drive_long, LV_EVENT_LONG_PRESSED, (void *)1);
    r->data = md_row_create(body, MD_ICON_STORAGE, "Internal (/data)", " ");
    md_row_set_clickable(r->data, root_clicked, (void *)T5_DATA_PATH);
    sa_note(body, "Tap a .t5app file to install it. Apps install to the SD card when there is one. Long press a "
                  "file or folder to delete it, or a drive to eject or format it.");
    sa_set_refresh(&s_nav, root_refresh, r, free);
    const char *arg = t5_app_launch_arg(app);
    if (arg[0] == '/') open_dir(arg);
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    sa_photo_close();
    s_app = NULL;                  /* a running format reports to nobody */
    sa_nav_destroy(&s_nav);
}

static const t5_app_desc_t s_desc = { .api = T5_API_LEVEL, .on_create = on_create, .on_destroy = on_destroy };

const t5_builtin_app_t sysapp_files = {
    .id = "com.t5.files",
    .name = "Files",
    .desc = &s_desc,
    .perms = T5_PERM_STORAGE | T5_PERM_USB | T5_PERM_SYSTEM,
    .orientation = T5_ORIENT_ANY,
    .icon = MD_ICON_FOLDER,
};
