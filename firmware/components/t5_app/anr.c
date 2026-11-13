/* SPDX-License-Identifier: Apache-2.0
 * UI watchdog (plan 5.12): if the UI task is stuck in an app for 2 s, show
 * "App not responding" with Wait and Close app.
 *
 * The UI task is the one that's stuck, so LVGL can't draw the dialog. It is
 * rendered ahead of time (when an app starts, with its name) into an RGB565
 * picture, and the watchdog task copies that picture straight into the
 * framebuffers and reads touch itself. Close app moves the UI task out of
 * the app's code (t5_core/fault.h) and closes the app without a restart;
 * only if the UI task is stuck somewhere it can't be moved from does it
 * restart the device to the launcher. Either way the crash guard records
 * it as "not responding".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_sys.h"
#include "t5_ui/t5_ui.h"
#include "t5_app/anr_test.h"
#include "t5_app/isolate.h"
#include "t5_app/wrappers.h"
#include "apps_hw.h"
#include "app_priv.h"

static const char *TAG = "t5.anr";

#define STALL_MS 2000
#define WAIT_MORE_MS 5000
#define POLL_MS 100
#define BOX_W_DP 300
#define ELEV_DIALOG_DP 24
#define SHAPE_MEDIUM_DP 4
#define TOUCH_DP 48           /* a touch target */
#define BUTTON_H_DP 36

static uint16_t *s_img;
static int32_t s_w, s_h;
static t5ui_rect_t s_btn_wait, s_btn_close;    /* relative to the picture */
static t5_app_t *volatile s_app;
static volatile bool s_rendering;

static void render(void *p)
{
    t5_app_t *a = p;
    s_rendering = true;
    void *saved = t5_sys_enter();
    /* Off screen: a hidden holder on the top layer, so nothing is drawn
     * there; the snapshot draws the box itself. */
    t5ui_obj_t *holder = t5ui_create(t5ui_sys_layer(T5UI_LAYER_TOP), T5UI_BOX);
    t5ui_set_int(holder, T5UI_P_HIDDEN, true);
    t5ui_obj_t *box = t5ui_create(holder, T5UI_PANEL);
    /* A dialog's surface; its shadow only in the light theme. */
    t5ui_set_color(box, T5UI_P_BG_COLOR, t5ui_theme_surface(ELEV_DIALOG_DP));
    if (!t5ui_theme_dark()) t5ui_set_int(box, T5UI_P_ELEVATION, ELEV_DIALOG_DP);
    t5ui_set_int(box, T5UI_P_RADIUS, t5ui_dp(SHAPE_MEDIUM_DP));
    t5ui_set_int(box, T5UI_P_PAD_ALL, t5ui_dp(24));
    t5ui_set_int(box, T5UI_P_PAD_BOTTOM, t5ui_dp(8));
    t5ui_set_int(box, T5UI_P_PAD_ROW, t5ui_dp(20));
    t5ui_set_int(box, T5UI_P_WIDTH, t5ui_dp(BOX_W_DP));
    t5ui_set_int(box, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(box, T5UI_P_FLOW, T5UI_COLUMN);
    t5ui_obj_t *title = t5ui_create(box, T5UI_MD_TEXT);
    t5ui_set_int(title, T5UI_P_TEXT_STYLE, T5UI_TEXT_HEADLINE6);
    t5ui_set_str(title, T5UI_P_TEXT, "App not responding");
    char text[96];
    snprintf(text, sizeof text, "%s isn't responding. Close it, or wait for it?", a ? a->man.name : "The app");
    t5ui_obj_t *t = t5ui_create(box, T5UI_MD_TEXT);
    t5ui_set_int(t, T5UI_P_TEXT_STYLE, T5UI_TEXT_BODY1);
    t5ui_set_str(t, T5UI_P_TEXT, text);
    t5ui_set_int(t, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(t, T5UI_P_LONG_MODE, T5UI_LONG_WRAP);
    t5ui_set_int(t, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    t5ui_obj_t *row = t5ui_create(box, T5UI_BOX);
    t5ui_set_int(row, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(row, T5UI_P_HEIGHT, T5UI_CONTENT);
    t5ui_set_int(row, T5UI_P_FLOW, T5UI_ROW);
    t5ui_set_int(row, T5UI_P_PLACE_MAIN, T5UI_PLACE_END);
    t5ui_set_int(row, T5UI_P_PLACE_CROSS, T5UI_PLACE_CENTER);
    t5ui_set_int(row, T5UI_P_PLACE_TRACK, T5UI_PLACE_CENTER);
    t5ui_set_int(row, T5UI_P_PAD_COLUMN, t5ui_dp(8));
    t5ui_obj_t *close = t5ui_create(row, T5UI_MD_BUTTON_TEXT);
    t5ui_set_str(close, T5UI_P_TEXT, "Close app");
    t5ui_obj_t *wait = t5ui_create(row, T5UI_MD_BUTTON_TEXT);
    t5ui_set_str(wait, T5UI_P_TEXT, "Wait");
    t5ui_layout_now(box);

    t5ui_rect_t bc, cc, wc;
    t5ui_coords(box, &bc);
    t5ui_coords(close, &cc);
    t5ui_coords(wait, &wc);
    /* Touch targets: the buttons grown to 48 dp. */
    int32_t g = t5ui_dp(TOUCH_DP - BUTTON_H_DP) / 2;
    s_btn_close = (t5ui_rect_t){ cc.x1 - bc.x1 - g, cc.y1 - bc.y1 - g, cc.x2 - bc.x1 + g, cc.y2 - bc.y1 + g };
    s_btn_wait = (t5ui_rect_t){ wc.x1 - bc.x1 - g, wc.y1 - bc.y1 - g, wc.x2 - bc.x1 + g, wc.y2 - bc.y1 + g };

    t5ui_pixels_t snap;
    if (t5ui_sys_snapshot(box, &snap) == T5_OK) {
        int32_t w = snap.w, h = snap.h;
        uint32_t stride = snap.stride ? snap.stride : (uint32_t)w * 2;
        uint16_t *img = s_img && w * h <= s_w * s_h ? s_img : malloc((size_t)w * (size_t)h * 2);
        if (img) {
            for (int32_t y = 0; y < h; y++) {
                memcpy(img + (size_t)y * (size_t)w, (const uint8_t *)snap.data + (size_t)y * stride, (size_t)w * 2);
            }
            if (s_img && img != s_img) free(s_img);
            s_img = img;
            s_w = w;
            s_h = h;
        }
        t5ui_sys_snapshot_free(&snap);
    }
    t5ui_delete(holder);
    t5_sys_leave(saved);
    s_rendering = false;
}

void anr_app_changed(t5_app_t *app)
{
    s_app = app;
    /* Already on the UI task (launch path). */
    if (app) render(app);
}

static bool hit(const t5ui_rect_t *a, int32_t x, int32_t y)
{
    return x >= a->x1 && x <= a->x2 && y >= a->y1 && y <= a->y2;
}

/* Memory and CPU checks (isolate.h), on the watchdog's poll. */
#define MEM_CHECK_MS 500
#define HOG_CHECK_MS 1000

static void isolation_checks(void)
{
    static uint32_t last_mem, last_hog;
    static t5_iso_mem_t mem;
    static t5_iso_l2_t l2;
    uint32_t now = t5_time_ms();
    if (now - last_mem >= MEM_CHECK_MS) {
        last_mem = now;
        if (t5_iso_mem_check(&mem, apps_hw_free_psram(), now)) apps_close_for_memory();
        switch (t5_iso_l2_check(&l2, apps_hw_free_l2(), now)) {
        case T5_ISO_L2_RELEASE:
            T5_LOGW(TAG, "internal RAM low (%u bytes free): releasing the reserve", (unsigned)apps_hw_free_l2());
            apps_hw_reserve_release();
            break;
        case T5_ISO_L2_RETAKE:
            apps_hw_reserve_take();
            break;
        case T5_ISO_L2_CLOSE_APP:
            apps_close_for_memory();
            break;
        default:
            break;
        }
    }
    t5_app_events_check_stuck();
    apps_throttle_tick();
    if (now - last_hog >= HOG_CHECK_MS) {
        last_hog = now;
        apps_check_hogs();
    }
}

static void watchdog(void *arg)
{
    (void)arg;
    bool showing = false, was_down = false;
    uint32_t threshold = STALL_MS;
    int32_t ox = 0, oy = 0;
    int32_t tx = 0, ty = 0;
    for (;;) {
        t5_delay_ms(POLL_MS);
        isolation_checks();
        uint32_t stalled = t5_ui_stalled_ms();
        if (!showing) {
            if (stalled < STALL_MS) threshold = STALL_MS;
            if (stalled < threshold || s_rendering) continue;
            t5_event_post(T5_EV_UI_STALL, NULL, 0);
            t5_app_t *a = s_app;
            if (!a || !s_img) {
                T5_LOGW(TAG, "UI task stuck for %u ms (system code)", (unsigned)stalled);
                threshold = stalled + WAIT_MORE_MS;
                continue;
            }
            T5_LOGW(TAG, "%s not responding (%u ms)", a->id, (unsigned)stalled);
            int32_t lw, lh;
            t5_rot_size(t5_ui_rotation(), &lw, &lh);
            ox = (lw - s_w) / 2;
            oy = (lh - s_h) / 2;
            if (t5_ui_overlay_show(s_img, s_w, s_h, ox, oy) != T5_OK) {
                threshold = stalled + WAIT_MORE_MS;
                continue;
            }
            showing = true;
            was_down = false;
            continue;
        }
        /* Recovered (by itself, or Close app stopped it): the stall only
         * grows while it lasts, so anything under the threshold means the
         * UI task has finished an iteration since. */
        if (stalled < STALL_MS) {
            t5_ui_overlay_restore();
            showing = false;
            continue;
        }
        int32_t x, y;
        bool down = t5_ui_touch_peek(&x, &y);
        if (down) {
            if (!was_down) T5_LOGI(TAG, "touch at %d,%d (dialog %d,%d)", (int)x, (int)y, (int)(x - ox), (int)(y - oy));
            tx = x - ox;
            ty = y - oy;
        } else if (was_down) {          /* act on release, like a button */
            if (hit(&s_btn_close, tx, ty)) {
                T5_LOGW(TAG, "closing %s", s_app ? s_app->id : "?");
                /* Returns only if the app was stopped without a restart:
                 * the UI task recovers and the overlay comes down below. */
                apps_restart_after_anr(s_app);
                threshold = STALL_MS;
            } else if (hit(&s_btn_wait, tx, ty)) {
                t5_ui_overlay_restore();
                showing = false;
                threshold = stalled + WAIT_MORE_MS;
            }
        }
        was_down = down;
    }
}

t5_err_t anr_init(void)
{
    /* Core 0, above app tasks: it must run while core 1 is stuck. */
    return t5_task_create_lp("t5_anr", watchdog, NULL, 4096, 10, 0, NULL);
}

/* Diagnostics: draw or remove the dialog over a healthy UI. */
t5_err_t anr_test_show(void)
{
    if (!s_img) return T5_ERR_STATE;
    int32_t lw, lh;
    t5_rot_size(t5_ui_rotation(), &lw, &lh);
    return t5_ui_overlay_show(s_img, s_w, s_h, (lw - s_w) / 2, (lh - s_h) / 2);
}

t5_err_t anr_test_hide(void)
{
    return t5_ui_overlay_restore();
}

t5_err_t anr_test_close(void)
{
    if (t5_ui_stalled_ms() < STALL_MS || !s_app) return T5_ERR_STATE;
    apps_restart_after_anr(s_app);
    return T5_OK;
}
