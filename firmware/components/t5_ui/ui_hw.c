/* SPDX-License-Identifier: Apache-2.0
 * PPA scale-rotate-mirror for the UI pipeline (plan 4.2).
 */
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "t5_core/log.h"
#include "ui_hw.h"

static const char *TAG = "t5.ui";

/* The PPA requires PSRAM buffers aligned to the L2 cache line (64 bytes in
 * this build; LVGL's draw buffers use the same, see CONFIG_LV_DRAW_BUF_ALIGN). */
#define PPA_ALIGN CONFIG_CACHE_L2_CACHE_LINE_SIZE

static ppa_client_handle_t s_srm[2];

t5_err_t t5_ui_hw_init(void)
{
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
