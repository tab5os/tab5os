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
#include "lvgl.h"
#include "md/md.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
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

static uint16_t *s_img;
static int32_t s_w, s_h;
static lv_area_t s_btn_wait, s_btn_close;    /* relative to the picture */
static t5_app_t *volatile s_app;
static volatile bool s_rendering;

static void render(void *p)
{
    t5_app_t *a = p;
    s_rendering = true;
    void *saved = t5_sys_enter();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *box = lv_obj_create(scr);
    md_set_elevation(box, MD_ELEV_DIALOG, false);
    lv_obj_set_style_radius(box, T5_DP(MD_SHAPE_MEDIUM_DP), 0);
    lv_obj_set_style_pad_all(box, T5_DP(24), 0);
    lv_obj_set_style_pad_bottom(box, T5_DP(8), 0);
    lv_obj_set_style_pad_row(box, T5_DP(20), 0);
    lv_obj_set_width(box, T5_DP(BOX_W_DP));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    md_label_create(box, "App not responding", MD_TYPE_HEADLINE6);
    char text[96];
    snprintf(text, sizeof text, "%s isn't responding. Close it, or wait for it?", a ? a->man.name : "The app");
    lv_obj_t *t = md_label_create(box, text, MD_TYPE_BODY1);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_opa(t, md_scheme()->on_surface.medium, 0);
    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, T5_DP(8), 0);
    lv_obj_t *close = md_button_create(row, "Close app", MD_BUTTON_TEXT);
    lv_obj_t *wait = md_button_create(row, "Wait", MD_BUTTON_TEXT);
    lv_obj_update_layout(scr);

    lv_area_t bc, cc, wc;
    lv_obj_get_coords(box, &bc);
    lv_obj_get_coords(close, &cc);
    lv_obj_get_coords(wait, &wc);
    /* Touch targets: the buttons grown to 48 dp. */
    int32_t g = T5_DP(MD_TOUCH_DP - MD_BUTTON_H_DP) / 2;
    s_btn_close = (lv_area_t){ cc.x1 - bc.x1 - g, cc.y1 - bc.y1 - g, cc.x2 - bc.x1 + g, cc.y2 - bc.y1 + g };
    s_btn_wait = (lv_area_t){ wc.x1 - bc.x1 - g, wc.y1 - bc.y1 - g, wc.x2 - bc.x1 + g, wc.y2 - bc.y1 + g };

    lv_draw_buf_t *snap = lv_snapshot_take(box, LV_COLOR_FORMAT_RGB565);
    if (snap) {
        int32_t w = (int32_t)snap->header.w, h = (int32_t)snap->header.h;
        uint16_t *img = s_img && w * h <= s_w * s_h ? s_img : malloc((size_t)w * (size_t)h * 2);
        if (img) {
            for (int32_t y = 0; y < h; y++) {
                memcpy(img + (size_t)y * (size_t)w, snap->data + (size_t)y * snap->header.stride, (size_t)w * 2);
            }
            if (s_img && img != s_img) free(s_img);
            s_img = img;
            s_w = w;
            s_h = h;
        }
        lv_draw_buf_destroy(snap);
    }
    lv_obj_delete(scr);
    t5_sys_leave(saved);
    s_rendering = false;
}

void anr_app_changed(t5_app_t *app)
{
    s_app = app;
    /* Already on the UI task (launch path). */
    if (app) render(app);
}

static bool hit(const lv_area_t *a, int32_t x, int32_t y)
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
