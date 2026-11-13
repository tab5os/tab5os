/* SPDX-License-Identifier: Apache-2.0
 * Camera (plan 7.6): full-screen preview, photos (JPEG) and video (Motion
 * JPEG in AVI) to /sd/DCIM, and a gallery. A launch argument naming a .jpg
 * opens it in the shared viewer (photo.c), an .avi in the player (video.c).
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
#include "t5/audio.h"
#include "t5/camera.h"
#include "t5/shell.h"
#include "t5/storage.h"
#include "t5/sys.h"
#include "t5_gui/t5ui_icons.h"
#include "t5_gui/t5ui_sys.h"
#include "sysapps_priv.h"

#define MAX_FILES 200

typedef struct {
    t5_app_t *app;
    t5_camera_t *cam;
    t5ui_obj_t *scr, *view, *frame, *bar, *preview, *shutter, *rec, *status, *gallery_btn;
    t5ui_timer_t *timer;
    bool granted, mirror, destroying;
    bool video_mode;              /* opened for recording (1280x720), not photos (1600x900) */
    volatile bool busy;           /* a photo is being taken, or recording starting/stopping */
    char rec_path[96];
} cam_app_t;

static cam_app_t s;
static sa_nav_t s_nav;            /* gallery pages */
static char s_last_photo[96];     /* for the snackbar's View */
static char s_last_video[96];     /* for the snackbar's Play */

static void viewer_closed(void);

static void make_dcim(void)
{
    mkdir(T5_DCIM_DIR, 0755);
}

static void stamp(char *out, size_t n, const char *prefix, const char *ext)
{
    struct tm tm;
    t5_time_local(&tm);
    snprintf(out, n, T5_DCIM_DIR "/%s_%04d%02d%02d_%02d%02d%02d.%s", prefix, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ext);
}

/* ---- photo ------------------------------------------------------------------------- */
typedef struct {
    char path[96];
    t5_err_t err;
} shot_t;

static void cam_stop(void);

/* The snackbar's View: the viewer replaces the preview until it's closed
 * (viewer_closed starts the camera again). */
static void view_last(void *arg)
{
    (void)arg;
    if (!t5_app_running(s.app) || !s_last_photo[0] || sa_photo_is_open() || sa_video_is_open() || s.busy) return;
    if (s.cam && t5_camera_recording(s.cam)) return;
    cam_stop();
    if (!sa_photo_open(s.scr, s_last_photo, viewer_closed)) viewer_closed();
}

/* The snackbar's Play, the same way. */
static void play_last(void *arg)
{
    (void)arg;
    if (!t5_app_running(s.app) || !s_last_video[0] || sa_photo_is_open() || sa_video_is_open() || s.busy) return;
    if (s.cam && t5_camera_recording(s.cam)) return;
    cam_stop();
    if (!sa_video_open(s.scr, s_last_video, viewer_closed)) viewer_closed();
}

static void shot_done(void *p)
{
    shot_t *sh = p;
    s.busy = false;
    if (t5_app_running(s.app)) {
        if (sh->err == T5_OK) {
            snprintf(s_last_photo, sizeof s_last_photo, "%s", sh->path);
            t5ui_snackbar("Photo saved", "View", view_last, NULL);
        } else {
            char msg[96];
            snprintf(msg, sizeof msg, "Couldn't take the photo (%s)", t5_err_str(sh->err));
            t5ui_snackbar(msg, NULL, NULL, NULL);
        }
    }
    free(sh);
}

static void shot_task(void *arg)
{
    shot_t *sh = arg;
    sh->err = s.cam ? t5_camera_photo(s.cam, sh->path, 90) : T5_ERR_STATE;
    t5ui_async(shot_done, sh);
}

static void shutter_clicked(t5ui_event_t *e)
{
    (void)e;
    if (!s.cam || s.busy) return;
    if (t5_storage_sd_state() != T5_SD_MOUNTED) {
        t5ui_snackbar("Insert an SD card to take photos", NULL, NULL, NULL);
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
    t5ui_obj_t *icon = t5ui_child(s.rec, 0);
    t5ui_set_str(icon, T5UI_P_ICON, on ? T5UI_ICON_STOP : T5UI_ICON_FIBER_MANUAL_RECORD);
    t5ui_set_color(icon, T5UI_P_TEXT_COLOR, on ? 0xffffff : t5ui_accent_color(T5UI_ACCENT_RED, T5UI_TONE_DARK));
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
        if (!j->start && j->err == T5_OK) {
            snprintf(s_last_video, sizeof s_last_video, "%s", s.rec_path);
            t5ui_snackbar("Video saved", "Play", play_last, NULL);
        } else if (!j->start) {
            t5ui_snackbar("Couldn't stop recording", NULL, NULL, NULL);
        }
        else if (j->err == T5_OK) t5_audio_play_sound(T5_SOUND_START);
        else t5ui_snackbar("Couldn't start recording", NULL, NULL, NULL);
        rec_paint();
    }
    free(j);
}

static void rec_task(void *arg)
{
    rec_job_t *j = arg;
    if (!s.cam) j->err = T5_ERR_STATE;
    else j->err = j->start ? t5_camera_record_start(s.cam, j->path, 0) : t5_camera_record_stop(s.cam);
    t5ui_async(rec_done, j);
}

static void rec_clicked(t5ui_event_t *e)
{
    (void)e;
    if (!s.cam || s.busy) return;
    rec_job_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->start = !t5_camera_recording(s.cam);
    if (j->start) {
        if (t5_storage_sd_state() != T5_SD_MOUNTED) {
            t5ui_snackbar("Insert an SD card to record", NULL, NULL, NULL);
            free(j);
            return;
        }
        make_dcim();
        /* Motion JPEG in AVI: the built-in player plays it (H.264 needs a
         * decoder the P4 doesn't have). */
        stamp(s.rec_path, sizeof s.rec_path, "VID", "avi");
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

static void mirror_clicked(t5ui_event_t *e)
{
    (void)e;
    s.mirror = !s.mirror;
    if (s.cam) t5_camera_set_mirror(s.cam, s.mirror);
}

static void tick(t5ui_timer_t *t, void *user)
{
    (void)t; (void)user;
    t5_camera_stats_t st;
    t5_camera_stats(&st);
    char buf[96];
    /* The pill only speaks when there is something to say: a recording's
     * running time, or why there is no picture. A live preview needs no
     * caption (the frame rate was a bench readout). */
    if (st.recording) {
        uint32_t sec = st.rec_ms / 1000;
        snprintf(buf, sizeof buf, "Recording %u:%02u%s", (unsigned)(sec / 60), (unsigned)(sec % 60),
                 st.rec_dropped ? ", dropping frames" : "");
    } else if (st.open) {
        buf[0] = 0;
    } else {
        snprintf(buf, sizeof buf, "%s", s.granted ? "Camera off" : "Allow the camera in Settings to use it");
    }
    sa_set_text(s.status, buf);
    t5ui_set_int(s.status, T5UI_P_HIDDEN, !buf[0]);
    t5ui_set_color(s.status, T5UI_P_TEXT_COLOR,
                   st.recording ? t5ui_accent_color(T5UI_ACCENT_RED, T5UI_TONE_DARK) : 0xffffff);
}

static void cam_stop(void);

/* ---- gallery ---------------------------------------------------------------------------------- */
typedef struct {
    char names[MAX_FILES][40];
    uint32_t sizes[MAX_FILES];
    int n;
    t5ui_obj_t *list;
} gallery_t;

static int cmp_desc(const void *a, const void *b)
{
    return -strcmp(a, b);
}

/* "IMG_20261109_224301.jpg" -> "9 Nov 2026, 22:43". False if the name
 * isn't one of ours. */
static bool pretty_stamp(const char *name, char *out, size_t n)
{
    static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    unsigned y, mo, d, h, mi, se;
    char pre[4];
    if (sscanf(name, "%3[A-Z]_%4u%2u%2u_%2u%2u%2u", pre, &y, &mo, &d, &h, &mi, &se) != 7) return false;
    if (mo < 1 || mo > 12) return false;
    snprintf(out, n, "%u %s %u, %02u:%02u", d, mon[mo - 1], y, h, mi);
    return true;
}

static void gal_bind(t5ui_event_t *e)
{
    gallery_t *g = t5ui_event_user(e);
    t5ui_obj_t *r = t5ui_event_target(e);
    int i = (int)t5ui_event_index(e);
    if (i < 0 || i >= g->n) return;
    char when[40];
    t5ui_set_str(r, T5UI_P_TEXT, pretty_stamp(g->names[i], when, sizeof when) ? when : g->names[i]);
    bool h264 = sa_ends_with(g->names[i], ".h264"), video = h264 || sa_video_can_open(g->names[i]);
    t5ui_set_str(r, T5UI_P_ICON, video ? T5UI_ICON_VIDEOCAM : T5UI_ICON_IMAGE);
    char b[24];
    sa_fmt_bytes(b, sizeof b, g->sizes[i]);
    /* Two-line rows: a one-line row has no second line, and this does nothing. */
    t5ui_set_strf(r, T5UI_P_SECONDARY,
                  h264 ? "Video, %s. Plays on a computer (H.264)" : video ? "Video, %s" : "Photo, %s", b);
}

static void gal_click(t5ui_event_t *e)
{
    gallery_t *g = t5ui_event_user(e);
    int i = (int)t5ui_event_index(e);
    if (i < 0 || i >= g->n) return;
    char path[96];
    snprintf(path, sizeof path, T5_DCIM_DIR "/%s", g->names[i]);
    if (sa_photo_can_open(path)) sa_photo_open(s.scr, path, NULL);
    else if (sa_video_can_open(path)) sa_video_open(s.scr, path, NULL);
}

static void cam_start(void);

static void gallery_closed(void)
{
    t5ui_set_int(s.view, T5UI_P_HIDDEN, false);
    t5_shell_set_fullscreen(true);
    cam_start();
}

static void open_gallery(t5ui_event_t *e)
{
    (void)e;
    gallery_t *g = calloc(1, sizeof *g);
    if (!g) return;
    DIR *d = opendir(T5_DCIM_DIR);
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
        snprintf(path, sizeof path, T5_DCIM_DIR "/%s", g->names[i]);
        g->sizes[i] = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
    }
    cam_stop();
    t5_shell_set_fullscreen(false);
    t5ui_set_int(s.view, T5UI_P_HIDDEN, true);
    t5ui_obj_t *body = sa_push(&s_nav, "Gallery");
    t5ui_set_int(body, T5UI_P_SCROLLABLE, false);
    g->list = t5ui_create(body, T5UI_MD_LIST);
    t5ui_on(g->list, T5UI_EV_BIND, gal_bind, g);
    t5ui_on(g->list, T5UI_EV_ITEM, gal_click, g);
    t5ui_set_int(g->list, T5UI_P_ROW_DP, 64);           /* two-line rows */
    t5ui_set_int(g->list, T5UI_P_COUNT, g->n);
    t5ui_set_int(g->list, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(g->list, T5UI_P_GROW, 1);
    if (!g->n) {
        /* An empty list would push the note to the bottom: drop the list. */
        t5ui_set_int(g->list, T5UI_P_HIDDEN, true);
        sa_note(body, t5_storage_sd_state() == T5_SD_MOUNTED
                          ? "No photos or videos yet. What you take is saved to the DCIM folder on the SD card."
                          : "Insert an SD card. Photos and videos are saved to it.");
    }
    sa_set_refresh(&s_nav, NULL, g, free);
}

/* ---- camera on/off ------------------------------------------------------------------------------- */
static void cam_start(void)
{
    if (s.cam || !s.granted) return;
    t5_err_t e = s.video_mode ? t5_camera_open_rgb(&s.cam) : t5_camera_open_photo(&s.cam);
    if (e != T5_OK) {
        char msg[64];
        snprintf(msg, sizeof msg, "Couldn't open the camera (%s)", t5_err_str(e));
        t5ui_snackbar(msg, NULL, NULL, NULL);
        return;
    }
    t5_camera_set_mirror(s.cam, s.mirror);
    /* The preview is the camera service's own object, taken in here. */
    s.preview = t5ui_sys_obj(t5_camera_preview_create(s.cam, t5ui_sys_native(s.frame)));
    if (s.preview) t5ui_move_to(s.preview, 0);   /* under the back and mirror buttons */
    rec_paint();
}

static void cam_stop(void)
{
    if (!s.cam) return;
    if (t5_camera_recording(s.cam)) t5_camera_record_stop(s.cam);
    t5_camera_close(s.cam);
    s.cam = NULL;
    if (s.preview) t5ui_delete(s.preview);
    s.preview = NULL;
}

static void perm_answer(t5_app_t *app, uint32_t perm, bool granted, void *user)
{
    (void)app; (void)perm; (void)user;
    s.granted = granted;
    if (granted) cam_start();
}

/* A dark translucent circle with a white icon (back, gallery, record, mirror). */
static t5ui_obj_t *round_button(t5ui_obj_t *parent, const char *icon, int size_dp, t5ui_event_cb_t cb)
{
    t5ui_obj_t *b = t5ui_create(parent, T5UI_BUTTON);
    t5ui_style_clear(b);
    t5ui_set_int(b, T5UI_P_WIDTH, t5ui_dp(size_dp));
    t5ui_set_int(b, T5UI_P_HEIGHT, t5ui_dp(size_dp));
    t5ui_set_int(b, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_color(b, T5UI_P_BG_COLOR, 0xffffff);
    t5ui_set_int(b, T5UI_P_BG_OPA, 38);                                          /* 15% */
    t5ui_set_int_at(b, T5UI_P_BG_OPA, T5UI_AT(T5UI_PART_MAIN, T5UI_STATE_PRESSED), 82);    /* 32% */
    t5ui_obj_t *l = t5ui_create(b, T5UI_MD_ICON);
    t5ui_set_str(l, T5UI_P_ICON, icon);
    t5ui_set_color(l, T5UI_P_TEXT_COLOR, 0xffffff);
    t5ui_set_int(l, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
    t5ui_on(b, T5UI_EV_CLICK, cb, NULL);
    return b;
}

/* The shutter: a white disc inside a white ring. */
static t5ui_obj_t *shutter_button(t5ui_obj_t *parent)
{
    t5ui_obj_t *b = t5ui_create(parent, T5UI_BUTTON);
    t5ui_style_clear(b);
    t5ui_set_int(b, T5UI_P_WIDTH, t5ui_dp(64));
    t5ui_set_int(b, T5UI_P_HEIGHT, t5ui_dp(64));
    t5ui_set_int(b, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_color(b, T5UI_P_BG_COLOR, 0xffffff);
    t5ui_set_int(b, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_int_at(b, T5UI_P_BG_OPA, T5UI_AT(T5UI_PART_MAIN, T5UI_STATE_PRESSED), 178);   /* 70% */
    t5ui_set_color(b, T5UI_P_OUTLINE_COLOR, 0xffffff);
    t5ui_set_int(b, T5UI_P_OUTLINE_WIDTH, t5ui_dp(3));
    t5ui_set_int(b, T5UI_P_OUTLINE_PAD, t5ui_dp(5));
    t5ui_on(b, T5UI_EV_CLICK, shutter_clicked, NULL);
    return b;
}

static void home_clicked(t5ui_event_t *e)
{
    (void)e;
    t5_app_exit(s.app);
}

/* Landscape: the preview, then the controls in a column on the right.
 * Portrait: the preview, then the controls in a row underneath. */
static void apply_layout(void)
{
    int32_t w, h;
    t5ui_screen_size(&w, &h);
    bool land = w >= h;
    t5ui_set_int(s.view, T5UI_P_FLOW, land ? T5UI_ROW : T5UI_COLUMN);
    t5ui_set_int(s.bar, T5UI_P_FLOW, land ? T5UI_COLUMN_REVERSE : T5UI_ROW);
    t5ui_set_int(s.frame, T5UI_P_WIDTH, land ? 0 : T5UI_PCT(100));
    t5ui_set_int(s.frame, T5UI_P_HEIGHT, land ? T5UI_PCT(100) : 0);
    t5ui_set_int(s.bar, T5UI_P_WIDTH, land ? t5ui_dp(112) : T5UI_PCT(100));
    t5ui_set_int(s.bar, T5UI_P_HEIGHT, land ? T5UI_PCT(100) : t5ui_dp(144));
    t5ui_set_int(s.frame, T5UI_P_GROW, 1);
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

/* A photo or video opened by another app (a launch argument), or from the
 * snackbar, was closed. */
static void viewer_closed(void)
{
    if (!s.destroying && s_nav.depth == 0) cam_start();
}

static void on_create(t5_app_t *app)
{
    memset(&s, 0, sizeof s);
    s.app = app;
    s.scr = t5ui_root(app);
    sa_nav_init(&s_nav, app);
    s_nav.root_back = gallery_closed;
    t5ui_set_color(s.scr, T5UI_P_BG_COLOR, 0x000000);
    t5_shell_set_fullscreen(true);
    s.view = t5ui_create(s.scr, T5UI_BOX);
    t5ui_set_int(s.view, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(s.view, T5UI_P_HEIGHT, T5UI_PCT(100));
    t5ui_set_int(s.view, T5UI_P_SCROLLABLE, false);

    /* The preview never sits under the controls: it is a video plane,
     * drawn straight into the framebuffer each frame, and whatever lies over
     * it is redrawn each frame too (t5_ui/plane.h). Only the small back
     * and mirror buttons and the status line float over it. */
    s.frame = t5ui_create(s.view, T5UI_BOX);
    t5ui_set_int(s.frame, T5UI_P_SCROLLABLE, false);
    t5ui_obj_t *back = round_button(s.frame, T5UI_ICON_ARROW_BACK, 44, home_clicked);
    t5ui_set_int(back, T5UI_P_ALIGN, T5UI_ALIGN_TOP_LEFT);
    t5ui_set_int(back, T5UI_P_X, t5ui_dp(12));
    t5ui_set_int(back, T5UI_P_Y, t5ui_dp(12));
    t5ui_obj_t *mirror = round_button(s.frame, T5UI_ICON_SWAP_HORIZ, 44, mirror_clicked);
    t5ui_set_int(mirror, T5UI_P_ALIGN, T5UI_ALIGN_TOP_RIGHT);
    t5ui_set_int(mirror, T5UI_P_X, -t5ui_dp(12));
    t5ui_set_int(mirror, T5UI_P_Y, t5ui_dp(12));
    s.status = t5ui_create(s.frame, T5UI_MD_TEXT);
    t5ui_set_int(s.status, T5UI_P_TEXT_STYLE, T5UI_TEXT_CAPTION);
    t5ui_set_str(s.status, T5UI_P_TEXT, "");
    t5ui_set_color(s.status, T5UI_P_TEXT_COLOR, 0xffffff);
    t5ui_set_color(s.status, T5UI_P_BG_COLOR, 0x000000);
    t5ui_set_int(s.status, T5UI_P_BG_OPA, 127);                  /* 50% */
    t5ui_set_int(s.status, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_int(s.status, T5UI_P_PAD_LEFT, t5ui_dp(12));
    t5ui_set_int(s.status, T5UI_P_PAD_RIGHT, t5ui_dp(12));
    t5ui_set_int(s.status, T5UI_P_PAD_TOP, t5ui_dp(4));
    t5ui_set_int(s.status, T5UI_P_PAD_BOTTOM, t5ui_dp(4));
    /* On the button row's centre line (12 dp + half of 44 dp, less half the
     * pill's 28 dp), clear of the picture's bottom edge however the preview
     * is letterboxed. */
    t5ui_set_int(s.status, T5UI_P_ALIGN, T5UI_ALIGN_TOP_MID);
    t5ui_set_int(s.status, T5UI_P_X, 0);
    t5ui_set_int(s.status, T5UI_P_Y, t5ui_dp(20));
    t5ui_set_int(s.status, T5UI_P_HIDDEN, true);

    s.bar = t5ui_create(s.view, T5UI_BOX);
    t5ui_set_int(s.bar, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(s.bar, T5UI_P_PLACE_MAIN, T5UI_PLACE_SPACE_EVENLY);
    t5ui_set_int(s.bar, T5UI_P_PLACE_CROSS, T5UI_PLACE_CENTER);
    t5ui_set_int(s.bar, T5UI_P_PLACE_TRACK, T5UI_PLACE_CENTER);
    s.gallery_btn = round_button(s.bar, T5UI_ICON_PHOTO_LIBRARY, 52, open_gallery);
    s.shutter = shutter_button(s.bar);
    s.rec = round_button(s.bar, T5UI_ICON_FIBER_MANUAL_RECORD, 52, rec_clicked);
    apply_layout();
    rec_paint();
    s.timer = t5ui_timer(500, tick, NULL);

    s.granted = t5_app_has_permission(app, T5_PERM_CAMERA);
    if (!s.granted) sa_report(t5_app_request_permission(app, T5_PERM_CAMERA, perm_answer, NULL), "ask for the camera");
    const char *arg = t5_app_launch_arg(app);
    if (arg[0] == '/' && sa_video_can_open(arg)) sa_video_open(s.scr, arg, viewer_closed);
    else if (arg[0] == '/') sa_photo_open(s.scr, arg, viewer_closed);
}

static void on_resume(t5_app_t *app)
{
    (void)app;
    if (!sa_photo_is_open() && !sa_video_is_open() && s_nav.depth == 0) cam_start();
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
    sa_video_close();
    cam_stop();
    if (s.timer) t5ui_timer_delete(s.timer);
    s.timer = NULL;
    sa_nav_destroy(&s_nav);
}

static const t5_app_desc_t s_desc = {
    .api = T5_API_LEVEL, .on_create = on_create, .on_resume = on_resume, .on_suspend = on_suspend,
    .on_destroy = on_destroy, .on_rotate = on_rotate,
};

const t5_builtin_app_t sysapp_camera = {
    .id = T5_APP_ID_CAMERA,
    .name = "Camera",
    .desc = &s_desc,
    .perms = T5_PERM_CAMERA | T5_PERM_MIC | T5_PERM_STORAGE,
    .orientation = T5_ORIENT_ANY,
    .icon = T5UI_ICON_CAMERA_ALT,
};
