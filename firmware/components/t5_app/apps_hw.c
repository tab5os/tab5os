/* SPDX-License-Identifier: Apache-2.0
 * App runtime hardware layer: crash guard memory, core dumps, ELF loading.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_elf.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "multi_heap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "private/elf_symbol.h"
#include "private/elf_types.h"
#include "t5_app/isolate.h"
#include "t5_app/tracker.h"
#include "t5_app/abi.h"
#include "t5_core/crc.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/task_tls.h"
#include "app_priv.h"
#include "apps_hw.h"

static const char *TAG = "t5.app";

/* ---- crash guard: RTC no-init memory survives panics and watchdog resets ---- */
#define GUARD_MAGIC 0x47524431u   /* "GRD1" */

typedef struct {
    uint32_t magic;
    char id[T5_CRASH_ID_LEN];
    uint8_t headless;
    uint8_t pending;
    uint8_t crc;
} guard_t;

static RTC_NOINIT_ATTR guard_t s_guard;

static uint8_t guard_crc(void)
{
    return t5_crc8(&s_guard, offsetof(guard_t, crc));
}

void apps_hw_guard_read(t5_crash_boot_t *out)
{
    memset(out, 0, sizeof *out);
    if (s_guard.magic != GUARD_MAGIC || s_guard.crc != guard_crc()) return;
    out->valid = true;
    memcpy(out->running, s_guard.id, sizeof out->running);
    out->running[sizeof out->running - 1] = 0;
    out->headless = s_guard.headless;
    out->pending_kind = s_guard.pending;
}

void apps_hw_guard_set(const char *id, bool headless)
{
    memset(&s_guard, 0, sizeof s_guard);
    s_guard.magic = GUARD_MAGIC;
    strncpy(s_guard.id, id, sizeof s_guard.id - 1);
    s_guard.headless = headless;
    s_guard.crc = guard_crc();
}

void apps_hw_guard_pending(uint8_t kind)
{
    if (s_guard.magic != GUARD_MAGIC) return;
    s_guard.pending = kind;
    s_guard.crc = guard_crc();
}

void apps_hw_guard_clear(void)
{
    memset(&s_guard, 0, sizeof s_guard);
}

/* ---- core dump ---------------------------------------------------------------------- */
bool apps_hw_coredump_save(const char *path)
{
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || !size) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t *buf = malloc(4096);
    bool ok = buf != NULL;
    for (size_t off = 0; ok && off < size; off += 4096) {
        size_t n = size - off < 4096 ? size - off : 4096;
        ok = esp_flash_read(NULL, buf, addr + off, n) == ESP_OK && fwrite(buf, 1, n, f) == n;
    }
    free(buf);
    ok = fclose(f) == 0 && ok;
    if (ok) {
        esp_core_dump_image_erase();
        T5_LOGI(TAG, "core dump (%u bytes) saved to %s", (unsigned)size, path);
    } else {
        remove(path);
    }
    return ok;
}

/* ---- ELF loading ---------------------------------------------------------------------- */
/* One load at a time: launches run on the UI task, alarms at boot. */
static t5_app_t *s_loading;
static uint32_t s_missing;

static uintptr_t resolve(const char *name)
{
    bool denied = false;
    uintptr_t a = t5_abi_resolve(name, s_loading ? s_loading->man.perms : 0, &denied);
    if (a) {
        if (denied && s_loading) {
            s_loading->denied_symbols++;
            T5_LOGW(TAG, "%s: %s needs a permission the manifest doesn't declare; bound to a stub",
                    s_loading->id, name);
        }
        return a;
    }
    s_missing++;
    T5_LOGE(TAG, "%s imports %s, which API level %d doesn't export", s_loading ? s_loading->id : "?", name,
            T5_API_LEVEL);
    return 0;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

t5_err_t apps_hw_elf_load(t5_app_t *app, const char *path, void **handle, const t5_app_desc_t **desc)
{
    size_t len = 0;
    uint8_t *img = read_file(path, &len);
    if (!img) return T5_ERR_NOT_FOUND;
    esp_elf_t *elf = calloc(1, sizeof *elf);
    if (!elf) {
        free(img);
        return T5_ERR_NO_MEM;
    }
    esp_elf_init(elf);
    s_loading = app;
    s_missing = 0;
    elf_set_symbol_resolver(resolve);
    int r = esp_elf_relocate(elf, img, len);
    elf_reset_symbol_resolver();
    s_loading = NULL;
    /* Relocation copied the segments into their own memory; img is freed
     * once its program headers are read. */
    if (r != 0) {
        free(img);
        T5_LOGE(TAG, "relocate %s: %d (%u unresolved)", path, r, (unsigned)s_missing);
        esp_elf_deinit(elf);
        free(elf);
        return s_missing ? T5_ERR_NOT_SUPPORTED : T5_ERR_IO;
    }
    /* The executable PT_LOAD segments, as offsets into the loaded image
     * (fault blame, isolate.h). */
    uint32_t xlo = UINT32_MAX, xhi = 0;
    const elf32_hdr_t *eh = (const elf32_hdr_t *)img;
    if (len >= sizeof *eh && eh->phoff && eh->phentsize == sizeof(elf32_phdr_t) &&
        eh->phoff + (size_t)eh->phnum * sizeof(elf32_phdr_t) <= len) {
        const elf32_phdr_t *ph = (const elf32_phdr_t *)(img + eh->phoff);
        for (int i = 0; i < eh->phnum; i++) {
            if (ph[i].type != PT_LOAD || !(ph[i].flags & 1u /* PF_X */)) continue;
            if (ph[i].vaddr < xlo) xlo = ph[i].vaddr;
            if (ph[i].vaddr + ph[i].memsz > xhi) xhi = ph[i].vaddr + ph[i].memsz;
        }
    }
    free(img);
    img = NULL;
    app->text_lo = app->text_hi = 0;
    if (elf->psegment && xhi > xlo && xlo >= elf->svaddr && xhi - elf->svaddr <= elf->segment_size) {
        app->text_lo = (uintptr_t)elf->psegment + (xlo - elf->svaddr);
        app->text_hi = (uintptr_t)elf->psegment + (xhi - elf->svaddr);
    } else if (elf->psegment) {
        /* No program headers to go by: the whole image. */
        app->text_lo = (uintptr_t)elf->psegment;
        app->text_hi = (uintptr_t)elf->psegment + elf->segment_size;
    }
    const t5_app_desc_t *d = NULL;
    char *argv[1] = { (char *)&d };
    /* Runs the app's constructors, then t5_app_entry(), which hands back
     * the descriptor through argv[0] (see T5_APP). */
    esp_elf_request(elf, 0, 1, argv);
    if (!d) {
        esp_elf_deinit(elf);
        free(elf);
        return T5_ERR_INVALID_ARG;
    }
    *handle = elf;
    *desc = d;
    return T5_OK;
}

void apps_hw_elf_unload(void *handle)
{
    esp_elf_t *elf = handle;
    if (!elf) return;
    esp_elf_deinit(elf);
    free(elf);
}

/* ---- app heap (isolate.h) ------------------------------------------------------ */
/* L2 RAM: DMA-capable internal memory (not LP RAM or the TCM). */
#define CAPS_L2 (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

size_t apps_hw_free_psram(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

size_t apps_hw_free_l2(void)
{
    return heap_caps_get_free_size(CAPS_L2);
}

/* An app's heap: PSRAM chunks of its own, each a private TLSF heap, with the
 * bookkeeping outside them. Every call comes in under the apps lock, so the
 * heaps need no spinlock of their own: a fault in one (a corrupted heap)
 * doesn't happen inside a critical section and can be recovered from. */
typedef struct chunk {
    multi_heap_handle_t heap;
    uint8_t *lo, *hi;
    uint32_t blocks;
    struct chunk *next;
} chunk_t;

typedef struct {
    chunk_t *chunks;
    size_t last;                /* size of the last chunk grown by the schedule */
} arena_t;

static chunk_t *chunk_add(arena_t *ar, size_t size)
{
    size = (size + 4095) & ~(size_t)4095;
    if (!t5_iso_heap_room(size, apps_hw_free_psram())) return NULL;
    chunk_t *c = heap_caps_malloc(sizeof *c, CAPS_PSRAM);
    uint8_t *m = heap_caps_malloc(size, CAPS_PSRAM);
    multi_heap_handle_t h = m ? multi_heap_register(m, size) : NULL;
    if (!c || !h) {
        heap_caps_free(c);
        heap_caps_free(m);
        return NULL;
    }
    *c = (chunk_t){ h, m, m + size, 0, ar->chunks };
    ar->chunks = c;
    return c;
}

static void *chunk_alloc(chunk_t *c, size_t n)
{
    void *p = multi_heap_malloc(c->heap, n);
    if (p) c->blocks++;
    return p;
}

static void *app_heap_malloc(t5_tracker_t *t, size_t n)
{
    arena_t *ar = t->arena;
    if (!ar) {
        ar = heap_caps_calloc(1, sizeof *ar, CAPS_PSRAM);
        if (!ar) return NULL;
        t->arena = ar;
    }
    for (chunk_t *c = ar->chunks; c; c = c->next) {
        void *p = chunk_alloc(c, n);
        if (p) return p;
    }
    /* Room for TLSF's tables and headers on top of the request, and for its
     * size classes: it looks for a free block of the next class up. */
    size_t need = n + n / 16 + 8192;
    size_t want = t5_iso_arena_next(ar->last);
    chunk_t *c = NULL;
    if (need <= want && (c = chunk_add(ar, want))) ar->last = want;
    else c = chunk_add(ar, need);
    return c ? chunk_alloc(c, n) : NULL;
}

static void app_heap_free(t5_tracker_t *t, void *p)
{
    arena_t *ar = t->arena;
    chunk_t **pp = ar ? &ar->chunks : NULL;
    for (; pp && *pp; pp = &(*pp)->next) {
        chunk_t *c = *pp;
        if ((uint8_t *)p < c->lo || (uint8_t *)p >= c->hi) continue;
        multi_heap_free(c->heap, p);
        /* An empty chunk goes back, unless it is the only one. */
        if (--c->blocks == 0 && (ar->chunks != c || c->next)) {
            *pp = c->next;
            heap_caps_free(c->lo);
            heap_caps_free(c);
        }
        return;
    }
}

static bool app_heap_drop_all(t5_tracker_t *t)
{
    arena_t *ar = t->arena;
    for (chunk_t *c = ar ? ar->chunks : NULL; c;) {
        chunk_t *next = c->next;
        heap_caps_free(c->lo);
        heap_caps_free(c);
        c = next;
    }
    if (ar) heap_caps_free(ar);
    t->arena = NULL;
    return true;
}

bool apps_hw_heap_contains(const t5_tracker_t *t, const void *p)
{
    const arena_t *ar = t->arena;
    for (const chunk_t *c = ar ? ar->chunks : NULL; c; c = c->next) {
        if ((const uint8_t *)p >= c->lo && (const uint8_t *)p < c->hi) return true;
    }
    return false;
}

/* ---- internal RAM reserve (isolate.h) ------------------------------------------- */
static void *volatile s_reserve;

void apps_hw_reserve_take(void)
{
    if (!s_reserve) s_reserve = heap_caps_malloc(T5_ISO_L2_RESERVE, CAPS_L2);
}

void apps_hw_reserve_release(void)
{
    void *p = __atomic_exchange_n(&s_reserve, NULL, __ATOMIC_ACQ_REL);
    if (p) heap_caps_free(p);
}

/* The first internal allocation to fail frees the reserve for the next one. */
static void alloc_failed(size_t size, uint32_t caps, const char *fn)
{
    (void)fn;
    if ((caps & MALLOC_CAP_SPIRAM) || size > T5_ISO_L2_RESERVE) return;
    if (apps_hw_free_l2() < T5_ISO_L2_CRITICAL) apps_hw_reserve_release();
}

void apps_hw_heap_install(void)
{
    static const t5_tracker_alloc_t a = { app_heap_malloc, app_heap_free, app_heap_drop_all };
    t5_tracker_set_allocator(&a);
    apps_hw_reserve_take();
    heap_caps_register_failed_alloc_callback(alloc_failed);
}

uint64_t apps_hw_task_runtime_us(void *task)
{
    TaskStatus_t st;
    vTaskGetInfo((TaskHandle_t)task, &st, pdFALSE, eRunning);
    return (uint64_t)st.ulRunTimeCounter;
}

void apps_hw_task_set_prio(void *task, int prio)
{
    vTaskPrioritySet((TaskHandle_t)task, (UBaseType_t)prio);
}

void apps_hw_task_pause(void *task, bool pause)
{
    if (pause) vTaskSuspend((TaskHandle_t)task);
    else vTaskResume((TaskHandle_t)task);
}

/* Deleting a task that holds an OS mutex leaves it locked for good: the I2C
 * bus, the filesystem, the app runtime's own lock. FreeRTOS counts each
 * task's held mutexes in its TCB but has no call to read them, so find the
 * field: it is the word pvTaskIncrementMutexHeldCount() moves by one. Found
 * at run time, and -1 (no waiting) rather than a wrong answer if that ever
 * stops being true. Mutexes the app created itself don't matter: everything
 * that could wait on them is deleted too. */
static int s_mutexes_held_word = -2;

/* vTaskSuspend() of a task running on the other core only marks it: it
 * keeps running until that core next switches tasks (it may be inside a
 * critical section). Until it has, it can still take a lock, so nothing may
 * treat it as stopped. True once the scheduler has it off every core. */
#define SETTLE_MS 100
static bool suspend_and_settle(TaskHandle_t t)
{
    vTaskSuspend(t);
    for (uint32_t ms = 0; ms < SETTLE_MS; ms++) {
        if (eTaskGetState(t) == eSuspended) return true;
        t5_delay_ms(1);
    }
    return eTaskGetState(t) == eSuspended;
}

static void find_mutexes_held(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    enum { WORDS = sizeof(StaticTask_t) / sizeof(UBaseType_t) };
    UBaseType_t before[WORDS];
    volatile UBaseType_t *tcb = (volatile UBaseType_t *)xTaskGetCurrentTaskHandle();
    int found = -1, changed = 0;
    taskENTER_CRITICAL(&mux);
    for (int i = 0; i < WORDS; i++) before[i] = tcb[i];
    (void)pvTaskIncrementMutexHeldCount();
    for (int i = 0; i < WORDS; i++) {
        if (tcb[i] == before[i]) continue;
        changed++;
        if (tcb[i] == before[i] + 1) found = i;
    }
    if (found >= 0) tcb[found]--;
    taskEXIT_CRITICAL(&mux);
    s_mutexes_held_word = changed == 1 ? found : -1;
    if (s_mutexes_held_word < 0) T5_LOGE(TAG, "can't find the TCB's mutex count; app tasks are deleted without waiting");
}

bool apps_hw_task_park(void *task, int (*app_held)(void *task))
{
    if (s_mutexes_held_word == -2) find_mutexes_held();
    if (!suspend_and_settle((TaskHandle_t)task)) {
        vTaskResume((TaskHandle_t)task);
        return false;
    }
    /* Inside lwIP is held like a lock: deleted there, the task would leave
     * lwIP pointing into its stack and at its freed semaphore (isolate.h). */
    if (apps_hw_net_inside(task)) {
        vTaskResume((TaskHandle_t)task);
        return false;
    }
    if (s_mutexes_held_word < 0) return true;
    UBaseType_t held = ((const volatile UBaseType_t *)task)[s_mutexes_held_word];
    /* None, or only the app's own (app_held is -1 when it can't tell). */
    if (held == 0 || (app_held && app_held(task) == (int)held)) return true;
    vTaskResume((TaskHandle_t)task);
    return false;
}

void apps_hw_task_delete(void *task)
{
    if (task == xTaskGetCurrentTaskHandle()) return;   /* never ourselves */
    /* Already parked by the exit's quiesce: never let it run again here, the
     * release walk that called us isn't safe against app code. */
    if (!suspend_and_settle((TaskHandle_t)task)) {
        T5_LOGE(TAG, "%s didn't stop within %u ms; deleting it anyway", pcTaskGetName((TaskHandle_t)task), SETTLE_MS);
    }
    /* Still inside lwIP once the exit can't wait any longer (quiesce gave
     * up): not deleted there. It leaves the app and runs on until the call
     * returns, then ends itself (netguard.c). Its stack isn't the app's. */
    if (apps_hw_net_orphan(task)) {
        T5_LOGW(TAG, "%s is inside a network call; it ends itself when the call returns",
                pcTaskGetName((TaskHandle_t)task));
        vTaskSetThreadLocalStoragePointer((TaskHandle_t)task, T5_TASK_TLS_APP_CTX, NULL);
        vTaskResume((TaskHandle_t)task);
        return;
    }
    t5_task_delete((t5_task_t)task);
}

void apps_hw_restart(void)
{
    esp_restart();
}

/* A periodic callback on the esp_timer task (energy accounting). */
static void periodic_cb(void *arg)
{
    ((void (*)(void))arg)();
}

void apps_hw_periodic(void (*fn)(void), uint32_t period_ms)
{
    const esp_timer_create_args_t args = { .callback = periodic_cb, .arg = (void *)fn, .name = "t5.apps" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_periodic(t, (uint64_t)period_ms * 1000u);
}
