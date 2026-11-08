/* SPDX-License-Identifier: Apache-2.0
 * Camera (plan 7.6): full-screen preview, photos (JPEG) and video (H.264)
 * to /sd/DCIM, and a gallery. A launch argument naming a .jpg opens it in
 * the shared viewer (photo.c).
 *
 * The camera is opened in on_resume and closed in on_suspend, as the
 * lifecycle asks, so the sensor is in reset whenever the app isn't in front.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "t5/audio.h"
#include "t5/camera.h"
#include "t5/shell.h"
#include "t5/storage.h"
#include "t5/sys.h"
#include "sysapps_priv.h"

#define DCIM T5_SD_PATH "/DCIM"

#define MAX_FILES 200

typedef struct {
    t5_app_t *app;
    t5_camera_t *cam;
    lv_obj_t *scr, *view, *frame, *bar, *preview, *shutter, *rec, *status, *gallery_btn;
    lv_timer_t *timer;
    bool granted, mirror, destroying;
    bool video_mode;              /* opened for recording (1280x720), not photos (1600x900) */
    volatile bool busy;           /* a photo is being taken, or recording starting/stopping */
    char rec_path[96];
} cam_app_t;

static cam_app_t s;
static sa_nav_t s_nav;            /* gallery pages */

static void make_dcim(void)
{
    mkdir(DCIM, 0755);
}

static void stamp(char *out, size_t n, const char *prefix, const char *ext)
{
    struct tm tm;
    t5_time_local(&tm);
    snprintf(out, n, DCIM "/%s_%04d%02d%02d_%02d%02d%02d.%s", prefix, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ext);
}

/* ---- photo ------------------------------------------------------------------------- */
typedef struct {
    char path[96];
    t5_err_t err;
} shot_t;

static void shot_done(void *p)
{
    shot_t *sh = p;
    s.busy = false;
    char msg[128];
    if (sh->err == T5_OK) snprintf(msg, sizeof msg, "Saved %s", sh->path + strlen(T5_SD_PATH) + 1);
    else snprintf(msg, sizeof msg, "Photo failed: %s", t5_err_str(sh->err));
    if (t5_app_running(s.app)) md_snackbar_show(msg, NULL, NULL, NULL);
    free(sh);
}

static void shot_task(void *arg)
{
    shot_t *sh = arg;
    sh->err = s.cam ? t5_camera_photo(s.cam, sh->path, 90) : T5_ERR_STATE;
    t5_ui_async(shot_done, sh);
}

static void shutter_clicked(lv_event_t *e)
{
    (void)e;
    if (!s.cam || s.busy) return;
    if (t5_storage_sd_state() != T5_SD_MOUNTED) {
        md_snackbar_show("Insert an SD card to take photos", NULL, NULL, NULL);
        return;
    }
    make_dcim();
    shot_t *sh = calloc(1, sizeof *sh);
    if (!sh) return;
    stamp(sh->path, sizeof sh->path, "IMG", "jpg");
    s.busy = true;
    t5_audio_play_sound(T5_SOUND_SHUTTER);
    if (t5_task_create("cam.shot", shot_task, sh, 6144, 5, T5_CORE_ANY, NULL) != T5_OK) {
        s.busy = false;
        free(sh);
    }
}

/* ---- video ------------------------------------------------------------------------------ */
static void rec_paint(void)
{
    bool on = s.cam && t5_camera_recording(s.cam);
    lv_obj_t *icon = lv_obj_get_child(s.rec, 0);
    lv_label_set_text(icon, on ? MD_ICON_STOP : MD_ICON_FIBER_MANUAL_RECORD);
    lv_obj_set_style_text_color(icon, on ? lv_color_white() : md_color(md_accent(MD_ACCENT_RED)->dark), 0);
}

/* Starting and stopping run on a worker, like photos: the camera service
 * needs the UI task meanwhile (it lends the encoder the UI's draw buffer),
 * so the UI task must not wait for it (t5_camera_record_start refuses). */
typedef struct {
    bool start;
    char path[96];
    t5_err_t err;
} rec_job_t;

static void cam_start(void);
static void cam_stop(void);

/* Recording needs the camera's video mode, photos use the full-width mode:
 * reopen it in the other one (about a second; the preview pauses). */
static void cam_reopen(bool video)
{
    s.video_mode = video;
    cam_stop();
    cam_start();
}

static void rec_done(void *p)
{
    rec_job_t *j = p;
    s.busy = false;
    if (t5_app_running(s.app) && s.video_mode && (!j->start || j->err != T5_OK)) cam_reopen(false);
    if (t5_app_running(s.app)) {
        if (!j->start) md_snackbar_show(j->err == T5_OK ? "Video saved" : "Couldn't stop recording", NULL, NULL, NULL);
        else if (j->err == T5_OK) t5_audio_play_sound(T5_SOUND_START);
        else md_snackbar_show("Couldn't start recording", NULL, NULL, NULL);
        rec_paint();
    }
    free(j);
}

static void rec_task(void *arg)
{
    rec_job_t *j = arg;
    if (!s.cam) j->err = T5_ERR_STATE;
    else j->err = j->start ? t5_camera_record_start(s.cam, j->path, 6000) : t5_camera_record_stop(s.cam);
    t5_ui_async(rec_done, j);
}

static void rec_clicked(lv_event_t *e)
{
    (void)e;
    if (!s.cam || s.busy) return;
    rec_job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->start = !t5_camera_recording(s.cam);
    if (j->start) {
        if (t5_storage_sd_state() != T5_SD_MOUNTED) {
            md_snackbar_show("Insert an SD card to record", NULL, NULL, NULL);
            free(j);
            return;
        }
        make_dcim();
        stamp(s.rec_path, sizeof s.rec_path, "VID", "h264");
        snprintf(j->path, sizeof j->path, "%s", s.rec_path);
        cam_reopen(true);
        if (!s.cam) {
            free(j);
            cam_reopen(false);
            return;
        }
    }
    s.busy = true;
    if (t5_task_create("cam.rec", rec_task, j, 6144, 5, T5_CORE_ANY, NULL) != T5_OK) {
        s.busy = false;
        free(j);
    }
}

static void mirror_clicked(lv_event_t *e)
{
    (void)e;
    s.mirror = !s.mirror;
    if (s.cam) t5_camera_set_mirror(s.cam, s.mirror);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    t5_camera_stats_t st;
    t5_camera_stats(&st);
    char buf[96];
    if (st.recording) {
        char d[24];
        sa_fmt_duration(d, sizeof d, st.rec_ms / 1000);
        snprintf(buf, sizeof buf, "REC %s · %u frames%s", d, (unsigned)st.rec_frames,
                 st.rec_dropped ? " · dropping" : "");
    } else if (st.open) {
        snprintf(buf, sizeof buf, "%u fps", (unsigned)st.preview_fps);
    } else {
        snprintf(buf, sizeof buf, "%s", s.granted ? "Camera off" : "Needs the camera permission");
    }
    sa_set_text(s.status, buf);
}

static void cam_stop(void);

/* ---- gallery ---------------------------------------------------------------------------------- */
typedef struct {
    char names[MAX_FILES][40];
    uint32_t sizes[MAX_FILES];
    int n;
    lv_obj_t *list;
} gallery_t;

static int cmp_desc(const void *a, const void *b)
{
    return -strcmp(a, b);
}

static void gal_bind(md_list_row_t *r, uint32_t i, void *ud)
{
    gallery_t *g = ud;
    if ((int)i >= g->n) return;
    lv_label_set_text(r->primary, g->names[i]);
    bool video = strstr(g->names[i], ".h264") != NULL;
    lv_label_set_text(r->icon, video ? MD_ICON_VIDEOCAM : MD_ICON_IMAGE);
    if (r->secondary) {
        char b[24];
        sa_fmt_bytes(b, sizeof b, g->sizes[i]);
        lv_label_set_text_fmt(r->secondary, "%s%s", b, video ? " · H.264 (play it on a computer)" : "");
    }
}

static void gal_click(uint32_t i, void *ud)
{
    gallery_t *g = ud;
    if ((int)i >= g->n || !sa_photo_can_open(g->names[i])) return;
    char path[96];
    snprintf(path, sizeof path, DCIM "/%s", g->names[i]);
    sa_photo_open(s.scr, path, NULL);
}

static void cam_start(void);

static void gallery_closed(void)
{
    lv_obj_set_hidden(s.view, false);
    t5_shell_set_fullscreen(true);
    cam_start();
}

static void open_gallery(lv_event_t *e)
{
    (void)e;
    gallery_t *g = calloc(1, sizeof *g);
    if (!g) return;
    DIR *d = opendir(DCIM);
    struct dirent *de;
    while (d && g->n < MAX_FILES && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(g->names[g->n], sizeof g->names[0], "%s", de->d_name);
        g->n++;
    }
    if (d) closedir(d);
    qsort(g->names, (size_t)g->n, sizeof g->names[0], cmp_desc);
    for (int i = 0; i < g->n; i++) {
        char path[96];
        struct stat st;
        snprintf(path, sizeof path, DCIM "/%s", g->names[i]);
        g->sizes[i] = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
    }
    cam_stop();
    t5_shell_set_fullscreen(false);
    lv_obj_set_hidden(s.view, true);
    lv_obj_t *body = sa_push(&s_nav, "Gallery");
    lv_obj_set_scrollable(body, false);
    g->list = md_list_create(body, (uint32_t)g->n, MD_LIST_ROW_2_DP, gal_bind, gal_click, g);
    lv_obj_set_width(g->list, LV_PCT(100));
    lv_obj_set_flex_grow(g->list, 1);
    if (!g->n) sa_note(body, "No photos or videos yet.");
    sa_set_refresh(&s_nav, NULL, g, free);
}

/* ---- camera on/off ------------------------------------------------------------------------------- */
static void cam_start(void)
{
    if (s.cam || !s.granted) return;
    t5_err_t e = s.video_mode ? t5_camera_open(&s.cam) : t5_camera_open_photo(&s.cam);
    if (e != T5_OK) {
        char msg[64];
        snprintf(msg, sizeof msg, "Camera: %s", t5_err_str(e));
        md_snackbar_show(msg, NULL, NULL, NULL);
        return;
    }
    t5_camera_set_mirror(s.cam, s.mirror);
    s.preview = t5_camera_preview_create(s.cam, s.frame);
    if (s.preview) lv_obj_move_background(s.preview);   /* under the back and mirror buttons */
    rec_paint();
}

static void cam_stop(void)
{
    if (!s.cam) return;
    if (t5_camera_recording(s.cam)) t5_camera_record_stop(s.cam);
    t5_camera_close(s.cam);
    s.cam = NULL;
    if (s.preview) lv_obj_delete(s.preview);
    s.preview = NULL;
}

static void perm_answer(t5_app_t *app, uint32_t perm, bool granted, void *user)
{
    (void)app; (void)perm; (void)user;
    s.granted = granted;
    if (granted) cam_start();
}

/* A dark translucent circle with a white icon (back, gallery, record, mirror). */
static lv_obj_t *round_button(lv_obj_t *parent, const char *icon, int size_dp, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, T5_DP(size_dp), T5_DP(size_dp));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, 38, 0);                    /* 15% */
    lv_obj_set_style_bg_opa(b, 82, LV_STATE_PRESSED);     /* 32% */
    lv_obj_t *l = md_icon_create(b, icon, MD_ICON_DP);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* The shutter: a white disc inside a white ring. */
static lv_obj_t *shutter_button(lv_obj_t *parent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, T5_DP(64), T5_DP(64));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(b, lv_color_white(), 0);
    lv_obj_set_style_outline_width(b, T5_DP(3), 0);
    lv_obj_set_style_outline_pad(b, T5_DP(5), 0);
    lv_obj_add_event_cb(b, shutter_clicked, LV_EVENT_CLICKED, NULL);
    return b;
}

static void home_clicked(lv_event_t *e)
{
    (void)e;
    t5_app_exit(s.app);
}

/* Landscape: the preview, then the controls in a column on the right.
 * Portrait: the preview, then the controls in a row underneath. */
static void apply_layout(void)
{
    lv_display_t *d = lv_obj_get_display(s.scr);
    bool land = lv_display_get_horizontal_resolution(d) >= lv_display_get_vertical_resolution(d);
    lv_obj_set_flex_flow(s.view, land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_flow(s.bar, land ? LV_FLEX_FLOW_COLUMN_REVERSE : LV_FLEX_FLOW_ROW);
    if (land) {
        lv_obj_set_size(s.frame, 0, LV_PCT(100));
        lv_obj_set_size(s.bar, T5_DP(112), LV_PCT(100));
    } else {
        lv_obj_set_size(s.frame, LV_PCT(100), 0);
        lv_obj_set_size(s.bar, LV_PCT(100), T5_DP(144));
    }
    lv_obj_set_flex_grow(s.frame, 1);
}

/* The preview's buffer is sized for the screen it was made on: make it again. */
static void on_rotate(t5_app_t *app, int degrees)
{
    (void)app; (void)degrees;
    apply_layout();
    if (s.cam && !s.busy && !t5_camera_recording(s.cam)) {
        cam_stop();
        cam_start();
    }
}

/* A photo opened by another app (a launch argument) was closed. */
static void photo_closed(void)
{
    if (!s.destroying && s_nav.depth == 0) cam_start();
}

static void on_create(t5_app_t *app)
{
    memset(&s, 0, sizeof s);
    s.app = app;
    s.scr = t5_app_screen(app);
    sa_nav_init(&s_nav, app);
    s_nav.root_back = gallery_closed;
    lv_obj_set_style_bg_color(s.scr, lv_color_black(), 0);
    t5_shell_set_fullscreen(true);
    s.view = lv_obj_create(s.scr);
    lv_obj_remove_style_all(s.view);
    lv_obj_set_size(s.view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(s.view, false);

    /* The preview never sits under the controls: every pixel of it is
     * redrawn each frame, and the display pipeline's copy cost grows with
     * its area (see bench-results, Phase 7). Only the small back and mirror
     * buttons and the status line float over it. */
    s.frame = lv_obj_create(s.view);
    lv_obj_remove_style_all(s.frame);
    lv_obj_set_scrollable(s.frame, false);
    lv_obj_t *back = round_button(s.frame, MD_ICON_ARROW_BACK, 44, home_clicked);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, T5_DP(12), T5_DP(12));
    lv_obj_t *mirror = round_button(s.frame, MD_ICON_SWAP_HORIZ, 44, mirror_clicked);
    lv_obj_align(mirror, LV_ALIGN_TOP_RIGHT, -T5_DP(12), T5_DP(12));
    s.status = md_label_create(s.frame, "", MD_TYPE_CAPTION);
    lv_obj_set_style_text_color(s.status, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s.status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.status, LV_OPA_50, 0);
    lv_obj_set_style_radius(s.status, T5_DP(10), 0);
    lv_obj_set_style_pad_hor(s.status, T5_DP(8), 0);
    lv_obj_set_style_pad_ver(s.status, T5_DP(2), 0);
    lv_obj_align(s.status, LV_ALIGN_BOTTOM_MID, 0, -T5_DP(12));

    s.bar = lv_obj_create(s.view);
    lv_obj_remove_style_all(s.bar);
    lv_obj_set_scrollable(s.bar, false);
    lv_obj_set_flex_align(s.bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s.gallery_btn = round_button(s.bar, MD_ICON_PHOTO_LIBRARY, 52, open_gallery);
    s.shutter = shutter_button(s.bar);
    s.rec = round_button(s.bar, MD_ICON_FIBER_MANUAL_RECORD, 52, rec_clicked);
    apply_layout();
    rec_paint();
    s.timer = lv_timer_create(tick, 500, NULL);

    s.granted = t5_app_has_permission(app, T5_PERM_CAMERA);
    if (!s.granted) sa_report(t5_app_request_permission(app, T5_PERM_CAMERA, perm_answer, NULL), "ask for the camera");
    const char *arg = t5_app_launch_arg(app);
    if (arg[0] == '/') sa_photo_open(s.scr, arg, photo_closed);
}

static void on_resume(t5_app_t *app)
{
    (void)app;
    if (!sa_photo_is_open() && s_nav.depth == 0) cam_start();
}

static void on_suspend(t5_app_t *app)
{
    (void)app;
    cam_stop();                   /* stops a recording too */
    s.video_mode = false;
}

static void on_destroy(t5_app_t *app)
{
    (void)app;
    s.destroying = true;
    sa_photo_close();
    cam_stop();
    if (s.timer) lv_timer_delete(s.timer);
    s.timer = NULL;
    sa_nav_destroy(&s_nav);
}

static const t5_app_desc_t s_desc = {
    .api = T5_API_LEVEL, .on_create = on_create, .on_resume = on_resume, .on_suspend = on_suspend,
    .on_destroy = on_destroy, .on_rotate = on_rotate,
};

const t5_builtin_app_t sysapp_camera = {
    .id = "com.t5.camera",
    .name = "Camera",
    .desc = &s_desc,
    .perms = T5_PERM_CAMERA | T5_PERM_MIC | T5_PERM_STORAGE,
    .orientation = T5_ORIENT_ANY,
    .icon = MD_ICON_CAMERA_ALT,
};
