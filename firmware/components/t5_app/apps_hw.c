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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "private/elf_symbol.h"
#include "t5_app/abi.h"
#include "t5_core/crc.h"
#include "t5_core/log.h"
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
    if (t5_abi_is_runtime_helper(name)) {
        a = elf_find_sym_default(name);
        if (a) return a;
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
    free(img);           /* relocation copied the sections into their own memory */
    if (r != 0) {
        T5_LOGE(TAG, "relocate %s: %d (%u unresolved)", path, r, (unsigned)s_missing);
        esp_elf_deinit(elf);
        free(elf);
        return s_missing ? T5_ERR_NOT_SUPPORTED : T5_ERR_IO;
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

void apps_hw_task_delete(void *task)
{
    if (task == xTaskGetCurrentTaskHandle()) return;   /* never ourselves */
    vTaskDelete((TaskHandle_t)task);
}

void apps_hw_restart(void)
{
    esp_restart();
}

size_t apps_hw_free_heap(void)
{
    return esp_get_free_heap_size();
}
