/* UVC Monitor (plan 7.9): shows a USB capture dongle or webcam on the
 * screen. Plug it into USB-A (a hub works too); the picture appears full
 * screen. Tap to show or hide the info line.
 *
 * Frames arrive as MJPEG on the USB driver's task. The newest one is copied
 * (older ones are dropped while a decode runs), a worker task decodes it
 * with the hardware JPEG decoder (t5_jpeg_decode) into the hidden half of a
 * double buffer, and the UI task swaps the image's source.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/camera.h>
#include <t5/shell.h>
#include <t5/sys.h>
#include <t5/ui.h>
#include <t5/usb.h>

#define W 1280
#define H 720
#define JPEG_MAX (512 * 1024)

void lv_image_cache_drop(const void *src);

typedef struct {
    t5_app_t *app;
    t5_usb_uvc_t *uvc;
    lv_obj_t *img, *info, *hint;
    lv_image_dsc_t dsc[2];
    uint16_t *px[2];
    int shown;
    uint8_t *jpeg;
    volatile size_t jpeg_len;     /* 0: free for the next frame */
    SemaphoreHandle_t have_frame;
    volatile bool swap_pending, running;
    uint32_t frames, decoded, dropped, errors, fps, last_frames;
    uint16_t src_w, src_h;
    lv_timer_t *timer;
} mon_t;

static mon_t *m;

static void frame_cb(const uint8_t *jpeg, size_t len, void *arg)
{
    mon_t *s = arg;
    s->frames++;
    if (s->jpeg_len || len > JPEG_MAX) {
        s->dropped++;
        return;
    }
    memcpy(s->jpeg, jpeg, len);
    s->jpeg_len = len;
    xSemaphoreGive(s->have_frame);
}

static void swap(void *arg)
{
    mon_t *s = arg;
    int next = s->shown ^ 1;
    lv_image_cache_drop(&s->dsc[next]);
    lv_image_set_src(s->img, &s->dsc[next]);
    s->shown = next;
    s->swap_pending = false;
}

static void decoder(void *arg)
{
    mon_t *s = arg;
    while (t5_app_running(s->app)) {
        if (xSemaphoreTake(s->have_frame, pdMS_TO_TICKS(200)) != pdTRUE || !s->jpeg_len) continue;
        if (s->swap_pending) {           /* the screen hasn't caught up: drop this one */
            s->dropped++;
            s->jpeg_len = 0;
            continue;
        }
        int target = s->shown ^ 1;
        uint16_t w = 0, h = 0;
        t5_err_t e = t5_jpeg_decode(s->jpeg, s->jpeg_len, s->px[target], (size_t)W * H * 2, &w, &h);
        s->jpeg_len = 0;
        if (e != T5_OK) {
            s->errors++;
            continue;
        }
        s->src_w = w;
        s->src_h = h;
        s->decoded++;
        s->swap_pending = true;
        if (t5_ui_async(swap, s) != T5_OK) s->swap_pending = false;
    }
}

static void tick(lv_timer_t *t)
{
    mon_t *s = lv_timer_get_user_data(t);
    s->fps = s->decoded - s->last_frames;
    s->last_frames = s->decoded;
    if (!s->uvc && t5_usb_uvc_present()) {
        t5_err_t e = t5_usb_uvc_open(W, H, 30, frame_cb, s, &s->uvc);
        if (e != T5_OK) lv_label_set_text_fmt(s->hint, "Couldn't start the camera: %s", t5_err_str(e));
    }
    lv_obj_set_hidden(s->hint, s->uvc != NULL);
    lv_label_set_text_fmt(s->info, "%ux%u · %u fps shown · %u received · %u dropped · %u errors", s->src_w, s->src_h,
                          (unsigned)s->fps, (unsigned)s->frames, (unsigned)s->dropped, (unsigned)s->errors);
}

static void toggle_info(lv_event_t *e)
{
    lv_obj_set_hidden(m->info, !lv_obj_is_hidden(m->info));
}

static void perm_answer(t5_app_t *app, uint32_t perm, bool granted, void *user)
{
    if (!granted) lv_label_set_text(m->hint, "UVC Monitor needs the camera permission (App Manager).");
}

static void on_create(t5_app_t *app)
{
    m = calloc(1, sizeof *m);
    m->app = app;
    t5_app_set_user(app, m);
    m->jpeg = malloc(JPEG_MAX);
    m->have_frame = xSemaphoreCreateBinary();
    for (int i = 0; i < 2; i++) {
        m->px[i] = calloc((size_t)W * H, 2);
        lv_image_dsc_t *d = &m->dsc[i];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = W;
        d->header.h = H;
        d->header.stride = W * 2;
        d->data_size = W * H * 2;
        d->data = (const uint8_t *)m->px[i];
    }
    t5_shell_set_fullscreen(true);
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollable(scr, false);
    m->img = lv_image_create(scr);
    lv_obj_center(m->img);
    lv_image_set_src(m->img, &m->dsc[0]);
    lv_obj_set_clickable(m->img, true);
    lv_obj_add_event_cb(m->img, toggle_info, LV_EVENT_CLICKED, NULL);
    m->hint = md_label_create(scr, "Plug a UVC camera or capture dongle into USB-A.", MD_TYPE_HEADLINE6);
    lv_obj_set_style_text_color(m->hint, lv_color_white(), 0);
    lv_obj_center(m->hint);
    m->info = md_label_create(scr, "", MD_TYPE_CAPTION);
    lv_obj_set_style_text_color(m->info, lv_color_white(), 0);
    lv_obj_set_style_bg_color(m->info, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(m->info, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(m->info, T5_DP(8), 0);
    lv_obj_align(m->info, LV_ALIGN_BOTTOM_LEFT, T5_DP(8), -T5_DP(8));
    m->timer = lv_timer_create(tick, 1000, m);
    if (!t5_app_has_permission(app, T5_PERM_CAMERA) &&
        t5_app_request_permission(app, T5_PERM_CAMERA, perm_answer, NULL) != T5_OK) {
        perm_answer(app, T5_PERM_CAMERA, false, NULL);
    }
    t5_task_create("uvc.decode", decoder, m, 4096, 5, T5_CORE_ANY, NULL);
}

static void on_suspend(t5_app_t *app)
{
    if (m->uvc) t5_usb_uvc_close(m->uvc);
    m->uvc = NULL;                  /* the timer reopens it on resume */
}

T5_APP(.on_create = on_create, .on_suspend = on_suspend);
