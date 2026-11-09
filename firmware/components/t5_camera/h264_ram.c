/* SPDX-License-Identifier: Apache-2.0
 * The H.264 encoder wants ~92 KB of contiguous internal RAM for its
 * reference frame, which a running system no longer has in one piece: while
 * recording, the UI lends its draw block and the encoder's allocator (wrapped
 * here, CMakeLists.txt) hands it out. esp_h264's frees come to
 * t5_camera_h264_free (firmware/CMakeLists.txt), which keeps the lent block:
 * it never goes back to the heap, where small allocations would split it
 * within a recording (and TLSF's size classes can't hand back a block of
 * exactly the size just freed anyway); reclaim returns it to the UI.
 */
#include <string.h>
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"   /* esp_cache_get_alignment, as esp_h264 uses */
#include "freertos/FreeRTOS.h"
#include "h264_ram.h"

/* Only the reference frame is this big; smaller internal buffers come from the heap. */
#define REF_MIN_BYTES (64 * 1024)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static void *s_block;
static size_t s_len;
static bool s_used;     /* the encoder has it */
static bool s_orphan;   /* taken back while the encoder still had it: its free frees it */

void h264_ram_give(void *block, size_t len)
{
    taskENTER_CRITICAL(&s_mux);
    s_block = block;
    s_len = block ? len : 0;
    s_used = s_orphan = false;
    taskEXIT_CRITICAL(&s_mux);
}

void *h264_ram_take(void)
{
    taskENTER_CRITICAL(&s_mux);
    void *b = s_block;
    if (s_used) {
        s_orphan = true;
        b = NULL;
    } else {
        s_block = NULL;
    }
    taskEXIT_CRITICAL(&s_mux);
    return b;
}

void *__real_esp_h264_aligned_calloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size,
                                     uint32_t caps);
void *__wrap_esp_h264_aligned_calloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size,
                                     uint32_t caps)
{
    size_t al = 0;
    esp_cache_get_alignment(caps, &al);
    size_t want = al ? (n * size + al - 1) / al * al : n * size;
    void *b = NULL;
    if ((caps & MALLOC_CAP_INTERNAL) && want >= REF_MIN_BYTES && alignment <= 64) {
        taskENTER_CRITICAL(&s_mux);
        if (s_block && !s_used && !s_orphan && want <= s_len) {
            b = s_block;
            s_used = true;
        }
        taskEXIT_CRITICAL(&s_mux);
    }
    if (!b) return __real_esp_h264_aligned_calloc(alignment, n, size, actual_size, caps);
    memset(b, 0, want);
    esp_cache_msync(b, want, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    *actual_size = (uint32_t)want;
    return b;
}

void t5_camera_h264_free(void *p);
void t5_camera_h264_free(void *p)
{
    bool keep = false;
    taskENTER_CRITICAL(&s_mux);
    if (p && p == s_block && s_used) {
        s_used = false;
        keep = !s_orphan;
        if (s_orphan) s_block = NULL;
        s_orphan = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!keep) heap_caps_free(p);
}
