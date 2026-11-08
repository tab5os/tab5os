/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_app/crashguard.h"
#include "t5_storage/t5_storage.h"

#define DB_VERSION 1

const char *t5_crash_kind_name(t5_crash_kind_t k)
{
    switch (k) {
    case T5_CRASH_PANIC: return "crash";
    case T5_CRASH_ANR: return "not responding";
    case T5_CRASH_ALARM_OVERRUN: return "alarm overrun";
    case T5_CRASH_OOM: return "out of memory";
    default: return "none";
    }
}

t5_crash_kind_t t5_crash_classify(const t5_crash_boot_t *b, bool crash_reset)
{
    if (!b || !b->valid || !b->running[0]) return T5_CRASH_NONE;
    if (b->pending_kind != T5_CRASH_NONE) return (t5_crash_kind_t)b->pending_kind;
    /* App code was running when the device reset. A power cut (button,
     * pack pulled) also leaves the id behind, but RTC memory doesn't
     * survive that, so a valid record plus a crash reset means a crash. */
    return crash_reset ? T5_CRASH_PANIC : T5_CRASH_NONE;
}

t5_crash_rec_t *t5_crash_find(t5_crash_db_t *db, const char *id)
{
    for (int i = 0; i < db->n; i++) {
        if (!strncmp(db->rec[i].id, id, T5_CRASH_ID_LEN)) return &db->rec[i];
    }
    return NULL;
}

t5_crash_rec_t *t5_crash_record(t5_crash_db_t *db, const char *id, t5_crash_kind_t kind, int64_t now)
{
    t5_crash_rec_t *r = t5_crash_find(db, id);
    if (!r) {
        if (db->n < T5_CRASH_MAX_APPS) {
            r = &db->rec[db->n++];
        } else {
            /* Evict the record whose last crash is oldest. */
            r = &db->rec[0];
            for (int i = 1; i < db->n; i++) {
                if (db->rec[i].last_time < r->last_time) r = &db->rec[i];
            }
        }
        memset(r, 0, sizeof *r);
        strncpy(r->id, id, T5_CRASH_ID_LEN - 1);
    }
    if (r->consecutive < UINT16_MAX) r->consecutive++;
    if (r->total < UINT16_MAX) r->total++;
    r->last_kind = (uint8_t)kind;
    r->last_time = now;
    return r;
}

void t5_crash_clean_exit(t5_crash_db_t *db, const char *id)
{
    t5_crash_rec_t *r = t5_crash_find(db, id);
    if (r) r->consecutive = 0;
}

bool t5_crash_autolaunch_allowed(const t5_crash_db_t *db, const char *id)
{
    for (int i = 0; i < db->n; i++) {
        if (!strncmp(db->rec[i].id, id, T5_CRASH_ID_LEN)) return db->rec[i].consecutive < T5_CRASH_BLOCK_AFTER;
    }
    return true;
}

void t5_crash_forget(t5_crash_db_t *db, const char *id)
{
    t5_crash_rec_t *r = t5_crash_find(db, id);
    if (!r) return;
    int i = (int)(r - db->rec);
    memmove(&db->rec[i], &db->rec[i + 1], sizeof(t5_crash_rec_t) * (size_t)(db->n - i - 1));
    db->n--;
}

t5_err_t t5_crash_db_load(t5_crash_db_t *db, const char *path)
{
    memset(db, 0, sizeof *db);
    size_t len = 0;
    t5_err_t e = t5_storage_blob_read(path, DB_VERSION, db->rec, sizeof db->rec, &len);
    if (e != T5_OK) return e;
    if (len % sizeof(t5_crash_rec_t)) {
        memset(db, 0, sizeof *db);
        return T5_ERR_IO;
    }
    db->n = (int)(len / sizeof(t5_crash_rec_t));
    for (int i = 0; i < db->n; i++) db->rec[i].id[T5_CRASH_ID_LEN - 1] = 0;
    return T5_OK;
}

t5_err_t t5_crash_db_save(const t5_crash_db_t *db, const char *path)
{
    return t5_storage_blob_write(path, DB_VERSION, db->rec, sizeof(t5_crash_rec_t) * (size_t)db->n);
}
