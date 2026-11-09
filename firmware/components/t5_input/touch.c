/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_bsp/panel.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_input/t5_touch.h"
#include "t5_input/t5_touch_test.h"
#include "t5_pins/pins.h"
#include "touch_hw.h"

static const char *TAG = "t5.touch";

/* ST712x registers (16-bit addresses) */
#define ST_MAX_X_H    0x0005
#define ST_ADV_INFO   0x0010
#define ST_REPORTS    0x0014
#define ST_REPORT_LEN 7
#define ST_ADV_WITH_COORD 0x08
#define ST_MAX_REPORTS 10
/* GT911 */
#define GT_PRODUCT_ID 0x8140
#define GT_STATUS     0x814E
#define GT_POINTS     0x814F
#define GT_POINT_LEN  8
#define GT_RESOLUTION 0x8048   /* x max lo/hi, y max lo/hi */

#define ACTIVE_POLL_MS 10   /* while a finger is down */
#define IDLE_POLL_MS 16     /* polling-only controllers */
#define FAILED_AFTER_ERRORS 100   /* T5_EV_TOUCH_FAILED once, at this many read errors */

static t5_i2c_dev_t *s_dev;
static t5_panel_t s_kind;
static uint8_t s_max_reports = ST_MAX_REPORTS;
static t5_touch_state_t s_state;
static t5_touch_info_t s_info;
static t5_mutex_t s_lock;
static bool s_mirror_x, s_mirror_y;
static t5_touch_cb_t s_cb;
static void *s_cb_arg;
static t5_touch_cb_t s_activity;
static void *s_activity_arg;

int t5_touch_parse_st712x(const uint8_t *r, int n, t5_touch_point_t *out, int max)
{
    int k = 0;
    for (int i = 0; i < n && k < max; i++, r += ST_REPORT_LEN) {
        if (!(r[0] & 0x80)) {
            continue;   /* slot not valid */
        }
        out[k].x = (uint16_t)(((r[0] & 0x3F) << 8) | r[1]);
        out[k].y = (uint16_t)((r[2] << 8) | r[3]);
        out[k].strength = r[4];
        out[k].id = (uint8_t)i;
        k++;
    }
    return k;
}

int t5_touch_parse_gt911(uint8_t status, const uint8_t *p, t5_touch_point_t *out, int max)
{
    if (!(status & 0x80)) {
        return -1;   /* buffer not ready: no new data */
    }
    int n = status & 0x0F;
    if (n > max) n = max;
    for (int i = 0; i < n; i++, p += GT_POINT_LEN) {
        out[i].id = p[0];
        out[i].x = (uint16_t)(p[1] | (p[2] << 8));
        out[i].y = (uint16_t)(p[3] | (p[4] << 8));
        out[i].strength = (uint8_t)(p[5] | (p[6] << 8));
    }
    return n;
}

void t5_touch_scale(t5_touch_point_t *p, uint16_t max_x, uint16_t max_y)
{
    const uint32_t w = T5_PANEL_NATIVE_W, h = T5_PANEL_NATIVE_H;
    uint32_t x = p->x, y = p->y;
    if (max_x && max_x != w) x = x * w / max_x;
    if (max_y && max_y != h) y = y * h / max_y;
    p->x = (uint16_t)(x >= w ? w - 1 : x);
    p->y = (uint16_t)(y >= h ? h - 1 : y);
}

void t5_touch_mirror(t5_touch_point_t *p, bool mirror_x, bool mirror_y)
{
    if (mirror_x) p->x = (uint16_t)(T5_PANEL_NATIVE_W - 1 - p->x);
    if (mirror_y) p->y = (uint16_t)(T5_PANEL_NATIVE_H - 1 - p->y);
}

t5_err_t t5_touch_sample(t5_touch_state_t *out)
{
    t5_touch_point_t pts[T5_TOUCH_MAX_POINTS];
    int n = 0;
    if (s_kind == T5_PANEL_ST7123 || s_kind == T5_PANEL_ST7121) {
        uint8_t adv;
        T5_RETURN_ON_ERR(t5_i2c_read16(s_dev, ST_ADV_INFO, &adv, 1));
        if (adv & ST_ADV_WITH_COORD) {
            uint8_t r[ST_MAX_REPORTS * ST_REPORT_LEN];
            T5_RETURN_ON_ERR(t5_i2c_read16(s_dev, ST_REPORTS, r, (size_t)s_max_reports * ST_REPORT_LEN));
            n = t5_touch_parse_st712x(r, s_max_reports, pts, T5_TOUCH_MAX_POINTS);
        }
    } else {
        uint8_t buf[1 + T5_TOUCH_MAX_POINTS * GT_POINT_LEN];
        T5_RETURN_ON_ERR(t5_i2c_read16(s_dev, GT_STATUS, buf, sizeof buf));
        n = t5_touch_parse_gt911(buf[0], &buf[1], pts, T5_TOUCH_MAX_POINTS);
        if (n < 0) {
            *out = s_state;   /* nothing new */
            return T5_OK;
        }
        const uint8_t zero = 0;
        T5_RETURN_ON_ERR(t5_i2c_write16(s_dev, GT_STATUS, &zero, 1));
    }
    out->count = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        t5_touch_scale(&pts[i], s_info.max_x, s_info.max_y);
        t5_touch_mirror(&pts[i], s_mirror_x, s_mirror_y);
        out->p[i] = pts[i];
    }
    out->t_ms = t5_time_ms();
    return T5_OK;
}

static void publish(const t5_touch_state_t *st)
{
    t5_mutex_lock(s_lock);
    s_state = *st;
    s_info.samples++;
    t5_mutex_unlock(s_lock);
    if (s_activity) {
        s_activity(st, s_activity_arg);
    }
    if (s_cb) {
        s_cb(st, s_cb_arg);
    }
}

static void touch_task(void *arg)
{
    (void)arg;
    bool down = false;
    for (;;) {
        if (!s_info.polling && !down) {
            if (t5_touch_hw_wait(1000)) {
                s_info.irqs++;
            } else {
                continue;
            }
        } else {
            t5_delay_ms(down ? ACTIVE_POLL_MS : IDLE_POLL_MS);
        }
        t5_touch_state_t st;
        if (t5_touch_sample(&st) != T5_OK) {
            s_info.errors++;
            if (s_info.errors == FAILED_AFTER_ERRORS) {
                t5_event_post(T5_EV_TOUCH_FAILED, NULL, 0);
            }
            down = false;
            continue;
        }
        if (st.count || down) {
            publish(&st);   /* includes the release sample */
        }
        down = st.count > 0;
    }
}

static t5_err_t init_st712x(void)
{
    uint8_t info[5] = {0};
    T5_RETURN_ON_ERR(t5_i2c_read16(s_dev, ST_MAX_X_H, info, sizeof info));
    s_info.max_x = (uint16_t)((info[0] << 8) | info[1]);
    s_info.max_y = (uint16_t)((info[2] << 8) | info[3]);
    s_max_reports = info[4] >= 1 && info[4] <= ST_MAX_REPORTS ? info[4] : ST_MAX_REPORTS;
    return T5_OK;
}

static t5_err_t init_gt911(void)
{
    uint8_t id[4] = {0};
    T5_RETURN_ON_ERR(t5_i2c_read16(s_dev, GT_PRODUCT_ID, id, 4));
    uint8_t res[4];
    T5_RETURN_ON_ERR(t5_i2c_read16(s_dev, GT_RESOLUTION, res, 4));
    s_info.max_x = (uint16_t)(res[0] | (res[1] << 8));
    s_info.max_y = (uint16_t)(res[2] | (res[3] << 8));
    T5_LOGI(TAG, "GT911 product %.4s", (const char *)id);
    return T5_OK;
}

t5_err_t t5_touch_init(void)
{
    if (s_dev) {
        return T5_OK;
    }
    T5_RETURN_ON_ERR(t5_bsp_panel_probe());
    const t5_panel_info_t *pi = t5_bsp_panel();
    s_kind = pi->panel;
    s_mirror_x = pi->touch_mirror_x;
    s_mirror_y = pi->touch_mirror_y;
    s_info.fw_version = pi->touch_fw;
    if (!s_lock) s_lock = t5_mutex_create();
    T5_RETURN_ON_ERR(t5_i2c_open(pi->touch_addr, T5_SYS_I2C_HZ, "touch", &s_dev));
    s_info.polling = s_kind == T5_PANEL_ILI9881C_GT911;
    t5_err_t err = s_info.polling ? init_gt911() : init_st712x();
    if (err != T5_OK) {
        T5_LOGE(TAG, "touch controller init failed: %s", t5_err_str(err));
        return err;
    }
    T5_LOGI(TAG, "%s: fw %u, %ux%u, %s", t5_bsp_panel_name(s_kind), s_info.fw_version,
            s_info.max_x, s_info.max_y, s_info.polling ? "polling" : "interrupt on G23");
    T5_RETURN_ON_ERR(t5_touch_hw_init(!s_info.polling));
    /* Core 1 with the UI: touch latency matters more than anything on core 0. */
    return t5_task_create("t5_touch", touch_task, NULL, 4096, 12, 1, NULL);
}

t5_err_t t5_touch_read(t5_touch_state_t *out)
{
    if (!s_dev || !out) {
        return T5_ERR_STATE;
    }
    t5_mutex_lock(s_lock);
    *out = s_state;
    t5_mutex_unlock(s_lock);
    return T5_OK;
}

void t5_touch_info(t5_touch_info_t *out)
{
    if (out) *out = s_info;
}

void t5_touch_set_callback(t5_touch_cb_t cb, void *arg)
{
    s_cb_arg = arg;
    s_cb = cb;
}

void t5_touch_set_activity_hook(t5_touch_cb_t cb, void *arg)
{
    s_activity_arg = arg;
    s_activity = cb;
}

void t5_touch_setup_for_test(t5_i2c_dev_t *dev, const t5_panel_info_t *panel, uint16_t max_x, uint16_t max_y,
                             uint8_t reports)
{
    s_dev = dev;
    s_kind = panel->panel;
    s_mirror_x = panel->touch_mirror_x;
    s_mirror_y = panel->touch_mirror_y;
    s_info.max_x = max_x;
    s_info.max_y = max_y;
    s_max_reports = reports;
}
