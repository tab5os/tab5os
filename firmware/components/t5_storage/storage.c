/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/crc.h"
#include "t5_storage/t5_storage.h"
#include "t5_storage_hw.h"

static const char *TAG = "t5.storage";

#define MAX_SYNC_HOOKS 8
#define DEAD_POLLS 2   /* consecutive failed status checks before we call it removed */

static bool s_data_ok;
static t5_mutex_t s_lock;
static t5_sd_state_t s_sd = T5_SD_ABSENT;
static uint32_t s_next_try_ms;
static int s_dead;
static struct { t5_sync_hook_t fn; void *arg; } s_hooks[MAX_SYNC_HOOKS];

const char *t5_storage_sd_state_str(t5_sd_state_t s)
{
    switch (s) {
    case T5_SD_ABSENT: return "absent";
    case T5_SD_MOUNTED: return "mounted";
    case T5_SD_FAILED: return "failed";
    case T5_SD_EJECTED: return "ejected";
    }
    return "?";
}

static void lock(void)
{
    if (!s_lock) {
        s_lock = t5_mutex_create();
    }
    t5_mutex_lock(s_lock);
}

static void unlock(void)
{
    t5_mutex_unlock(s_lock);
}

void t5_storage_sd_poll(uint32_t now_ms)
{
    lock();
    switch (s_sd) {
    case T5_SD_MOUNTED:
        if (t5_storage_hw_sd_alive()) {
            s_dead = 0;
        } else if (++s_dead >= DEAD_POLLS) {
            T5_LOGW(TAG, "SD card gone; unmounting");
            t5_storage_hw_sd_unmount();
            s_sd = T5_SD_ABSENT;
            s_dead = 0;
            s_next_try_ms = now_ms + T5_SD_RETRY_MS;
            t5_event_post(T5_EV_SD_REMOVED, NULL, 0);
        }
        break;
    case T5_SD_ABSENT:
    case T5_SD_FAILED:
        if ((int32_t)(now_ms - s_next_try_ms) < 0) {
            break;
        }
        {
            t5_err_t err = t5_storage_hw_sd_mount();
            if (err == T5_OK) {
                s_sd = T5_SD_MOUNTED;
                s_dead = 0;
                T5_LOGI(TAG, "SD card mounted at " T5_SD_PATH);
                t5_event_post(T5_EV_SD_INSERTED, NULL, 0);
            } else if (err == T5_ERR_NOT_FOUND) {
                s_sd = T5_SD_ABSENT;
                s_next_try_ms = now_ms + T5_SD_RETRY_MS;
            } else {
                if (s_sd != T5_SD_FAILED) {
                    T5_LOGW(TAG, "SD card present but mount failed: %s", t5_err_str(err));
                    t5_event_post(T5_EV_SD_MOUNT_FAILED, NULL, 0);
                }
                s_sd = T5_SD_FAILED;
                s_next_try_ms = now_ms + T5_SD_FAILED_RETRY_MS;
            }
        }
        break;
    case T5_SD_EJECTED:
        break;
    }
    unlock();
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        t5_storage_sd_poll(t5_time_ms());
        t5_delay_ms(T5_SD_POLL_MS);
    }
}

t5_err_t t5_storage_init(void)
{
    t5_err_t err = t5_storage_hw_data_mount();
    s_data_ok = err == T5_OK;
    if (!s_data_ok) {
        T5_LOGE(TAG, T5_DATA_PATH " unavailable: %s", t5_err_str(err));
    }
    /* First SD attempt inline so /sd is ready by the time the shell starts. */
    t5_storage_sd_poll(t5_time_ms());
    t5_err_t terr = t5_task_create_lp("t5_sd", poll_task, NULL, 4096, 3, 0, NULL);
    return err != T5_OK ? err : terr;
}

bool t5_storage_data_mounted(void)
{
    return s_data_ok;
}

t5_err_t t5_storage_data_usage(uint64_t *total, uint64_t *used)
{
    if (!s_data_ok) {
        return T5_ERR_STATE;
    }
    return t5_storage_hw_data_usage(total, used);
}

t5_sd_state_t t5_storage_sd_state(void)
{
    return s_sd;
}

t5_err_t t5_storage_sd_info(t5_sd_info_t *out)
{
    if (!out) {
        return T5_ERR_INVALID_ARG;
    }
    lock();
    t5_err_t err = s_sd == T5_SD_MOUNTED ? t5_storage_hw_sd_info(out) : T5_ERR_STATE;
    unlock();
    return err;
}

t5_err_t t5_storage_sd_eject(void)
{
    t5_storage_sync();
    lock();
    t5_err_t err = T5_ERR_STATE;
    if (s_sd == T5_SD_MOUNTED) {
        t5_storage_hw_sd_unmount();
        err = T5_OK;
        T5_LOGI(TAG, "SD card ejected; safe to remove");
        t5_event_post(T5_EV_SD_REMOVED, NULL, 0);
    }
    s_sd = T5_SD_EJECTED;
    unlock();
    return err;
}

t5_err_t t5_storage_sd_rescan(void)
{
    lock();
    if (s_sd != T5_SD_MOUNTED) {
        s_sd = T5_SD_ABSENT;
        s_next_try_ms = 0;
    }
    unlock();
    t5_storage_sd_poll(t5_time_ms());
    return s_sd == T5_SD_MOUNTED ? T5_OK : T5_ERR_NOT_FOUND;
}

t5_err_t t5_storage_write_atomic(const char *path, const void *data, size_t len)
{
    if (!path || (len && !data)) {
        return T5_ERR_INVALID_ARG;
    }
    char tmp[256];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        return T5_ERR_INVALID_ARG;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return errno == ENOENT ? T5_ERR_NOT_FOUND : T5_ERR_IO;
    }
    const uint8_t *p = data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            close(fd);
            unlink(tmp);
            return errno == ENOSPC ? T5_ERR_NO_MEM : T5_ERR_IO;
        }
        p += n;
        left -= (size_t)n;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp);
        return T5_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        /* FAT won't rename over an existing file. */
        if (unlink(path) != 0 || rename(tmp, path) != 0) {
            unlink(tmp);
            return T5_ERR_IO;
        }
    }
    return T5_OK;
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t len;
    uint32_t crc;
} blob_hdr_t;

#define BLOB_MAGIC 0x42355431u   /* "1T5B" */
#define BLOB_MAX (64 * 1024)

t5_err_t t5_storage_blob_write(const char *path, uint16_t version, const void *data, size_t len)
{
    if (!path || (len && !data) || len > BLOB_MAX) {
        return T5_ERR_INVALID_ARG;
    }
    size_t total = sizeof(blob_hdr_t) + len;
    uint8_t *buf = malloc(total);
    if (!buf) {
        return T5_ERR_NO_MEM;
    }
    blob_hdr_t h = { BLOB_MAGIC, version, 0, (uint32_t)len, t5_crc32(0, data, len) };
    memcpy(buf, &h, sizeof h);
    if (len) {
        memcpy(buf + sizeof h, data, len);
    }
    t5_err_t err = t5_storage_write_atomic(path, buf, total);
    free(buf);
    return err;
}

t5_err_t t5_storage_blob_read(const char *path, uint16_t version, void *out, size_t cap, size_t *len)
{
    if (!path || (cap && !out)) {
        return T5_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return T5_ERR_NOT_FOUND;
    }
    blob_hdr_t h;
    t5_err_t err = T5_ERR_IO;
    if (fread(&h, 1, sizeof h, f) == sizeof h && h.magic == BLOB_MAGIC && h.version == version &&
        h.len <= BLOB_MAX) {
        if (h.len > cap) {
            err = T5_ERR_NO_MEM;
        } else if (fread(out, 1, h.len, f) == h.len && t5_crc32(0, out, h.len) == h.crc) {
            if (len) *len = h.len;
            err = T5_OK;
        }
    }
    fclose(f);
    return err;
}

t5_err_t t5_storage_register_sync(t5_sync_hook_t fn, void *arg)
{
    for (int i = 0; i < MAX_SYNC_HOOKS; i++) {
        if (!s_hooks[i].fn) {
            s_hooks[i].fn = fn;
            s_hooks[i].arg = arg;
            return T5_OK;
        }
    }
    return T5_ERR_NO_MEM;
}

t5_err_t t5_storage_sync(void)
{
    t5_err_t first = T5_OK;
    for (int i = 0; i < MAX_SYNC_HOOKS; i++) {
        if (s_hooks[i].fn) {
            t5_err_t e = s_hooks[i].fn(s_hooks[i].arg);
            if (e != T5_OK && first == T5_OK) {
                first = e;
            }
        }
    }
    return first;
}

void t5_storage_reset_for_test(void)
{
    s_sd = T5_SD_ABSENT;
    s_next_try_ms = 0;
    s_dead = 0;
    s_data_ok = false;
    memset(s_hooks, 0, sizeof s_hooks);
}

/* USB mass storage (plan 7.8): unmount /sd and hand the raw card over.
 * The poller leaves an ejected card alone until t5_storage_sd_reclaim(). */
t5_err_t t5_storage_sd_lend(void **card)
{
    if (!card) return T5_ERR_INVALID_ARG;
    if (s_sd == T5_SD_MOUNTED) T5_RETURN_ON_ERR(t5_storage_sd_eject());
    lock();
    s_sd = T5_SD_EJECTED;
    t5_err_t e = t5_storage_hw_sd_raw_open(card);
    unlock();
    return e;
}

t5_err_t t5_storage_sd_reclaim(void *card)
{
    lock();
    t5_storage_hw_sd_raw_close(card);
    unlock();
    return t5_storage_sd_rescan();
}

/* Erase the SD card: a new FAT filesystem, mounted again at /sd. */
t5_err_t t5_storage_sd_format(void)
{
    lock();
    t5_err_t e = s_sd == T5_SD_MOUNTED ? t5_storage_hw_sd_format() : T5_ERR_STATE;
    unlock();
    if (e == T5_OK) T5_LOGW(TAG, "SD card formatted");
    return e;
}
