/* SPDX-License-Identifier: Apache-2.0
 * Inflate the deflated MD2 assets (see md_zasset.h) with the ROM's miniz.
 */
#include "md_zasset.h"
#ifdef ESP_PLATFORM
#include <stdbool.h>
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "miniz.h"

static const char *TAG = "md.assets";
static volatile bool s_loaded;

void md_assets_load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    int64_t t0 = esp_timer_get_time();
    size_t total = 0;
    for (int i = 0; md_zassets[i]; i++) {
        const md_zasset_t *a = md_zassets[i];
        /* 64-byte aligned, like LV_DRAW_BUF_ALIGN: the PPA reads shadow masks by DMA. */
        uint8_t *buf = heap_caps_aligned_alloc(64, (a->len + 63) & ~63u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGE(TAG, "no PSRAM for asset %d (%u bytes)", i, (unsigned)a->len);
            abort();
        }
        size_t n = tinfl_decompress_mem_to_mem(buf, a->len, a->z, a->zlen, TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (n != a->len) {
            ESP_LOGE(TAG, "asset %d: inflated %d of %u bytes", i, (int)n, (unsigned)a->len);
            abort();
        }
        esp_cache_msync(buf, (a->len + 63) & ~63u, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        *a->target = buf;
        total += a->len;
    }
    ESP_LOGI(TAG, "%u KB of fonts and shadows inflated in %d ms", (unsigned)(total / 1024),
             (int)((esp_timer_get_time() - t0) / 1000));
}
#else
void md_assets_load(void)
{
}
#endif
