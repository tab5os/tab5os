/* SPDX-License-Identifier: Apache-2.0
 * PPA scale-rotate-mirror for the UI pipeline (plan 4.2).
 */
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "t5_core/log.h"
#include "ui_hw.h"
#include "esp_timer.h"

static const char *TAG = "t5.ui";

/* The PPA requires PSRAM buffers aligned to the L2 cache line (64 bytes in
 * this build; LVGL's draw buffers use the same, see CONFIG_LV_DRAW_BUF_ALIGN). */
#define PPA_ALIGN CONFIG_CACHE_L2_CACHE_LINE_SIZE

static ppa_client_handle_t s_srm[2];
static ppa_client_handle_t s_async;
static t5_ui_hw_done_t s_done_cb;
static volatile int s_pending;
static SemaphoreHandle_t s_idle;

/* How long the PPA spent on each async copy: from when it could start (its
 * submission, or the previous copy's end) to its interrupt. Copies finish
 * in submission order. For the frame log. */
#define QN 64
static int64_t s_q[QN];
static volatile uint32_t s_qh, s_qt;
static int64_t s_last_done;
volatile uint32_t t5_ui_copy_exec_us, t5_ui_copy_n;

static bool on_done(ppa_client_handle_t c, ppa_event_data_t *ev, void *user)
{
    (void)c; (void)ev;
    int64_t now = esp_timer_get_time();
    if (s_qt != s_qh) {
        int64_t sub = s_q[s_qt % QN];
        s_qt++;
        int64_t from = sub > s_last_done ? sub : s_last_done;
        t5_ui_copy_exec_us += (uint32_t)(now - from);
        t5_ui_copy_n++;
    }
    s_last_done = now;
    BaseType_t woken = pdFALSE;
    if (user && s_done_cb) s_done_cb(user);
    if (__atomic_sub_fetch(&s_pending, 1, __ATOMIC_SEQ_CST) == 0) xSemaphoreGiveFromISR(s_idle, &woken);
    return woken == pdTRUE;
}

t5_err_t t5_ui_hw_init(void)
{
    if (!s_async) {
        /* A frame's catch-up copies (up to 48) plus two bands in flight. */
        const ppa_client_config_t cfg = {
            .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 56,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        };
        if (ppa_register_client(&cfg, &s_async) != ESP_OK) return T5_ERR_IO;
        const ppa_event_callbacks_t cbs = { .on_trans_done = on_done };
        ppa_client_register_event_callbacks(s_async, &cbs);
        s_idle = xSemaphoreCreateBinary();
        if (!s_idle) return T5_ERR_NO_MEM;
    }
    for (int i = 0; i < 2; i++) {
        if (s_srm[i]) continue;
        /* Client 1 (the watchdog overlay) writes while nothing else is
         * paced: short bursts leave the display DMA its share of PSRAM. */
        const ppa_client_config_t cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
            .data_burst_length = i == 0 ? PPA_DATA_BURST_LENGTH_128 : PPA_DATA_BURST_LENGTH_16,
        };
        esp_err_t e = ppa_register_client(&cfg, &s_srm[i]);
        if (e != ESP_OK) {
            T5_LOGE(TAG, "PPA client: %s", esp_err_to_name(e));
            return T5_ERR_IO;
        }
    }
    return T5_OK;
}

void *t5_ui_hw_alloc(size_t bytes)
{
    size_t n = (bytes + PPA_ALIGN - 1) & ~(size_t)(PPA_ALIGN - 1);
    return heap_caps_aligned_alloc(PPA_ALIGN, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *t5_ui_hw_alloc_internal(size_t bytes)
{
    size_t n = (bytes + PPA_ALIGN - 1) & ~(size_t)(PPA_ALIGN - 1);
    return heap_caps_aligned_alloc(PPA_ALIGN, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

/* The PPA rotates counter-clockwise; our rotations are clockwise. */
static ppa_srm_rotation_angle_t angle(t5_rotation_t r)
{
    switch (r) {
    case T5_ROT_90: return PPA_SRM_ROTATION_ANGLE_270;
    case T5_ROT_180: return PPA_SRM_ROTATION_ANGLE_180;
    case T5_ROT_270: return PPA_SRM_ROTATION_ANGLE_90;
    default: return PPA_SRM_ROTATION_ANGLE_0;
    }
}

t5_err_t t5_ui_hw_copy(int client, const uint16_t *src, int32_t src_w, int32_t src_h, t5_rect_t a,
                       t5_rect_t dst, uint16_t *fb, t5_rotation_t rot)
{
    if (client < 0 || client > 1 || !s_srm[client]) return T5_ERR_STATE;
    t5_rect_t p = t5_rot_rect_to_panel(rot, dst);
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src, .pic_w = (uint32_t)src_w, .pic_h = (uint32_t)src_h,
            .block_w = (uint32_t)t5_rect_w(&a), .block_h = (uint32_t)t5_rect_h(&a),
            .block_offset_x = (uint32_t)a.x1, .block_offset_y = (uint32_t)a.y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = fb, .buffer_size = (uint32_t)T5_PANEL_W * T5_PANEL_H * 2,
            .pic_w = T5_PANEL_W, .pic_h = T5_PANEL_H,
            .block_offset_x = (uint32_t)p.x1, .block_offset_y = (uint32_t)p.y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle(rot),
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t e = ppa_do_scale_rotate_mirror(s_srm[client], &op);
    if (e != ESP_OK) {
        T5_LOGE(TAG, "PPA copy (%d,%d)-(%d,%d): %s", (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, esp_err_to_name(e));
        return T5_ERR_IO;
    }
    return T5_OK;
}

void t5_ui_hw_set_done_cb(t5_ui_hw_done_t cb)
{
    s_done_cb = cb;
}

static t5_err_t submit(ppa_srm_oper_config_t *op, void *user)
{
    op->mode = PPA_TRANS_MODE_NON_BLOCKING;
    op->user_data = user;
    if (s_qh - s_qt < QN) s_q[s_qh++ % QN] = esp_timer_get_time();
    __atomic_add_fetch(&s_pending, 1, __ATOMIC_SEQ_CST);
    xSemaphoreTake(s_idle, 0);
    esp_err_t e = ppa_do_scale_rotate_mirror(s_async, op);
    if (e != ESP_OK) {
        __atomic_sub_fetch(&s_pending, 1, __ATOMIC_SEQ_CST);
        T5_LOGE(TAG, "PPA async: %s", esp_err_to_name(e));
        return T5_ERR_IO;
    }
    return T5_OK;
}

t5_err_t t5_ui_hw_band_async(const uint16_t *src, int32_t stride_px, t5_rect_t dst, uint16_t *fb, t5_rotation_t rot,
                             void *user)
{
    if (!s_async) return T5_ERR_STATE;
    t5_rect_t p = t5_rot_rect_to_panel(rot, dst);
    int32_t w = t5_rect_w(&dst), h = t5_rect_h(&dst);
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src, .pic_w = (uint32_t)stride_px, .pic_h = (uint32_t)h,
            .block_w = (uint32_t)w, .block_h = (uint32_t)h, .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = fb, .buffer_size = (uint32_t)T5_PANEL_W * T5_PANEL_H * 2,
            .pic_w = T5_PANEL_W, .pic_h = T5_PANEL_H,
            .block_offset_x = (uint32_t)p.x1, .block_offset_y = (uint32_t)p.y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle(rot),
        .scale_x = 1.0f, .scale_y = 1.0f,
    };
    return submit(&op, user);
}

t5_err_t t5_ui_hw_fb_copy_async(const uint16_t *src_fb, uint16_t *dst_fb, t5_rect_t r, void *user)
{
    if (!s_async) return T5_ERR_STATE;
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src_fb, .pic_w = T5_PANEL_W, .pic_h = T5_PANEL_H,
            .block_w = (uint32_t)t5_rect_w(&r), .block_h = (uint32_t)t5_rect_h(&r),
            .block_offset_x = (uint32_t)r.x1, .block_offset_y = (uint32_t)r.y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst_fb, .buffer_size = (uint32_t)T5_PANEL_W * T5_PANEL_H * 2,
            .pic_w = T5_PANEL_W, .pic_h = T5_PANEL_H,
            .block_offset_x = (uint32_t)r.x1, .block_offset_y = (uint32_t)r.y1,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f, .scale_y = 1.0f,
    };
    return submit(&op, user);
}

bool t5_ui_hw_busy(void)
{
    return __atomic_load_n(&s_pending, __ATOMIC_SEQ_CST) > 0;
}

bool t5_ui_hw_wait_idle(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (__atomic_load_n(&s_pending, __ATOMIC_SEQ_CST) > 0) {
        if (xSemaphoreTake(s_idle, pdMS_TO_TICKS(timeout_ms - waited > 5 ? 5 : 1)) != pdTRUE) waited += 5;
        if (waited >= timeout_ms) return false;
    }
    return true;
}
