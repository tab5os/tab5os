/* SPDX-License-Identifier: Apache-2.0
 * PPA scaling and fills for the photo viewer and the video player (each on
 * its own client: they run on different tasks).
 */
#include "driver/ppa.h"
#include "sysapps_priv.h"

static ppa_client_handle_t s_srm[SA_PPA_CLIENTS], s_fill[SA_PPA_CLIENTS];

static bool ready(int client)
{
    if (client < 0 || client >= SA_PPA_CLIENTS) return false;
    if (!s_srm[client]) {
        const ppa_client_config_t c = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
        if (ppa_register_client(&c, &s_srm[client]) != ESP_OK) return false;
    }
    if (!s_fill[client]) {
        const ppa_client_config_t c = { .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1 };
        if (ppa_register_client(&c, &s_fill[client]) != ESP_OK) return false;
    }
    return true;
}

int sa_ppa_scaled(int n, int k)
{
    /* The PPA's own rounding (esp_driver_ppa, ppa_srm.c). */
    return (k / 16) * n + (k % 16) * n / 16;
}

t5_err_t sa_ppa_scale(int client, const sa_pic_t *src, int bx, int by, int bw, int bh, int k, const sa_pic_t *dst,
                      int ox, int oy)
{
    if (!ready(client) || k < 1 || bw < 1 || bh < 1) return T5_ERR_INVALID_ARG;
    float s = (float)k / 16.0f;
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src->px, .pic_w = (uint32_t)src->w, .pic_h = (uint32_t)src->h,
            .block_w = (uint32_t)bw, .block_h = (uint32_t)bh,
            .block_offset_x = (uint32_t)bx, .block_offset_y = (uint32_t)by,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst->px, .buffer_size = (uint32_t)dst->bytes, .pic_w = (uint32_t)dst->w,
            .pic_h = (uint32_t)dst->h, .block_offset_x = (uint32_t)ox, .block_offset_y = (uint32_t)oy,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = s, .scale_y = s,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_srm[client], &op) == ESP_OK ? T5_OK : T5_ERR_IO;
}

t5_err_t sa_ppa_fill(int client, const sa_pic_t *dst, int x, int y, int w, int h)
{
    if (!ready(client) || w < 1 || h < 1) return T5_ERR_INVALID_ARG;
    ppa_fill_oper_config_t op = {
        .out = {
            .buffer = dst->px, .buffer_size = (uint32_t)dst->bytes, .pic_w = (uint32_t)dst->w,
            .pic_h = (uint32_t)dst->h, .block_offset_x = (uint32_t)x, .block_offset_y = (uint32_t)y,
            .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = (uint32_t)w, .fill_block_h = (uint32_t)h,
        .fill_argb_color = { .val = 0xFF000000u },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(s_fill[client], &op) == ESP_OK ? T5_OK : T5_ERR_IO;
}
