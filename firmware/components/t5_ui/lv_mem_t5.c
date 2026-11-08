/* SPDX-License-Identifier: Apache-2.0
 * LVGL's allocator (LV_USE_CUSTOM_MALLOC): small allocations from a private
 * internal-RAM pool, everything else (and the overflow) from the system heap.
 *
 * Why: with plain malloc, LVGL's objects, styles and per-frame draw tasks
 * land in PSRAM whenever internal RAM runs short (malloc falls back to it),
 * and LVGL's tree walk then runs 2-4x slower: the walk misses the L2 cache,
 * which code executing from PSRAM also needs. The pool keeps them internal
 * no matter what the rest of the system does. See docs/bench-results.md.
 *
 * Counters (t5_ui_lvmem) say where LVGL's allocations landed.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "multi_heap.h"
#include "lvgl.h"
#include "t5_ui/t5_ui.h"

/* Pool size and the largest request it serves. Bigger requests (image and
 * layer buffers, label text of long documents) go to the system heap,
 * which puts anything over 4 KB in PSRAM. */
#ifndef T5_LV_POOL_BYTES
#define T5_LV_POOL_BYTES (88 * 1024)
#endif
#ifndef T5_LV_POOL_MAX_REQ
#define T5_LV_POOL_MAX_REQ (4096)
#endif

static multi_heap_handle_t s_pool;
static uint8_t *s_pool_lo, *s_pool_hi;
static size_t s_pool_size;
static portMUX_TYPE s_pool_mux = portMUX_INITIALIZER_UNLOCKED;
static t5_ui_lvmem_t s_m;
static bool (*s_foreign_free)(void *p);
static void *(*s_foreign_realloc)(void *p, size_t n, bool *handled);

void t5_ui_lv_set_foreign(bool (*free_fn)(void *p), void *(*realloc_fn)(void *p, size_t n, bool *handled))
{
    s_foreign_free = free_fn;
    s_foreign_realloc = realloc_fn;
}

static inline bool in_pool(const void *p)
{
    return (const uint8_t *)p >= s_pool_lo && (const uint8_t *)p < s_pool_hi;
}

/* Live bytes outside the pool, by memory type (the pool has its own info). */
static void track(void *p, int sign)
{
    if (!p || in_pool(p)) return;
    int32_t n = sign * (int32_t)heap_caps_get_allocated_size(p);
    if (esp_ptr_external_ram(p)) {
        s_m.psram_live += n;
        if (s_m.psram_live > s_m.psram_peak) s_m.psram_peak = s_m.psram_live;
    } else {
        s_m.int_live += n;
        if (s_m.int_live > s_m.int_peak) s_m.int_peak = s_m.int_live;
    }
}

static void count_alloc(void *p)
{
    if (!p) {
        s_m.failed++;
        return;
    }
    if (in_pool(p)) s_m.pool_allocs++;
    else if (esp_ptr_external_ram(p)) s_m.psram_allocs++;
    else s_m.int_allocs++;
    track(p, 1);
}

/* Outside the pool: PSRAM. Never plain malloc: once L2MEM runs short, the
 * system heap hands out LP SRAM and the TCM, which are uncached on the slow
 * LP bus; LVGL there ran 6x slower than in PSRAM (2026-11-06). */
static void *outside(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_mem_init(void)
{
    if (s_pool) return;
    /* L2MEM only (DMA-capable excludes LP SRAM and the TCM). Take less if
     * there isn't that much in one piece. */
    for (size_t sz = T5_LV_POOL_BYTES; sz >= 32 * 1024; sz -= 8 * 1024) {
        void *b = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!b) continue;
        s_pool = multi_heap_register(b, sz);
        if (!s_pool) {
            heap_caps_free(b);
            return;
        }
        multi_heap_set_lock(s_pool, &s_pool_mux);
        s_pool_lo = b;
        s_pool_hi = (uint8_t *)b + sz;
        s_pool_size = sz;
        return;
    }
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem; (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    void *p = NULL;
    if (s_pool && size <= T5_LV_POOL_MAX_REQ) p = multi_heap_malloc(s_pool, size);
    if (!p) {
        p = outside(size);
        if (size > T5_LV_POOL_MAX_REQ) {
            s_m.big_allocs++;
            if (size > s_m.big_max) s_m.big_max = (uint32_t)size;
        }
        else s_m.overflow_allocs++;
    }
    count_alloc(p);
    return p;
}

void lv_free_core(void *p)
{
    if (!p) return;
    if (in_pool(p)) {
        multi_heap_free(s_pool, p);
        return;
    }
    if (s_foreign_free && s_foreign_free(p)) return;
    track(p, -1);
    heap_caps_free(p);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    if (!p) return lv_malloc_core(new_size);
    if (!in_pool(p)) {
        if (s_foreign_realloc) {
            bool handled = false;
            void *f = s_foreign_realloc(p, new_size, &handled);
            if (handled) return f;
        }
        /* Grows past the pool's limit or started outside it: stay outside. */
        track(p, -1);
        void *n = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!n) {
            track(p, 1);
            return NULL;
        }
        if (n != p) count_alloc(n);
        else track(n, 1);
        return n;
    }
    if (new_size <= T5_LV_POOL_MAX_REQ) {
        void *n = multi_heap_realloc(s_pool, p, new_size);
        if (n) return n;
    }
    /* Move out of the pool. */
    size_t old = multi_heap_get_allocated_size(s_pool, p);
    void *n = outside(new_size);
    count_alloc(n);
    if (!n) return NULL;
    memcpy(n, p, old < new_size ? old : new_size);
    multi_heap_free(s_pool, p);
    return n;
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    if (!mon || !s_pool) return;
    multi_heap_info_t i;
    multi_heap_get_info(s_pool, &i);
    mon->total_size = s_pool_size;
    mon->free_size = i.total_free_bytes;
    mon->free_biggest_size = i.largest_free_block;
    mon->free_cnt = i.free_blocks;
    mon->used_cnt = i.allocated_blocks;
    mon->max_used = s_pool_size - i.minimum_free_bytes;
    mon->used_pct = (uint8_t)(100 - (100 * i.total_free_bytes) / s_pool_size);
    mon->frag_pct = i.total_free_bytes ? (uint8_t)(100 - (100 * i.largest_free_block) / i.total_free_bytes) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

void t5_ui_lvmem(t5_ui_lvmem_t *out)
{
    if (!out) return;
    *out = s_m;
    out->pool_bytes = (uint32_t)s_pool_size;
    if (s_pool) {
        out->pool_free = multi_heap_free_size(s_pool);
        out->pool_min_free = multi_heap_minimum_free_size(s_pool);
    }
}

void t5_ui_lvmem_reset(void)
{
    /* Counts only; the live and peak bytes describe what is allocated now. */
    s_m.pool_allocs = s_m.int_allocs = s_m.psram_allocs = s_m.failed = 0;
    s_m.big_allocs = s_m.overflow_allocs = 0;
    s_m.big_max = 0;
    s_m.int_peak = s_m.int_live;
    s_m.psram_peak = s_m.psram_live;
}

bool t5_ui_lvmem_check(void)
{
    return !s_pool || multi_heap_check(s_pool, true);
}
