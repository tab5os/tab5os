/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include "t5_app/energy.h"

void t5_energy_init(t5_energy_db_t *db)
{
    memset(db, 0, sizeof *db);
    db->version = T5_ENERGY_DB_VERSION;
}

const t5_energy_rec_t *t5_energy_find(const t5_energy_db_t *db, const char *id)
{
    for (int i = 0; i < T5_ENERGY_MAX; i++) {
        if (db->rec[i].id[0] && !strncmp(db->rec[i].id, id, T5_ENERGY_ID_LEN - 1)) return &db->rec[i];
    }
    return NULL;
}

void t5_energy_add(t5_energy_db_t *db, const char *id, int32_t mw, uint32_t ms)
{
    if (!id || !id[0] || mw < 0 || !ms) return;
    t5_energy_rec_t *r = (t5_energy_rec_t *)t5_energy_find(db, id);
    if (!r) {
        int victim = 0;
        for (int i = 0; i < T5_ENERGY_MAX; i++) {
            if (!db->rec[i].id[0]) {
                victim = i;
                break;
            }
            if (db->rec[i].last_seen < db->rec[victim].last_seen) victim = i;
        }
        r = &db->rec[victim];
        memset(r, 0, sizeof *r);
        strncpy(r->id, id, T5_ENERGY_ID_LEN - 1);
    }
    r->mj += (uint64_t)mw * ms / 1000u;
    r->ms += ms;
    r->last_seen = ++db->seq;
}

int32_t t5_energy_avg_mw(const t5_energy_rec_t *r)
{
    if (!r || r->ms < 10000) return -1;
    return (int32_t)(r->mj * 1000u / r->ms);
}

static int by_avg(const void *a, const void *b)
{
    int32_t x = t5_energy_avg_mw(a), y = t5_energy_avg_mw(b);
    return (y > x) - (y < x);
}

int t5_energy_sorted(const t5_energy_db_t *db, t5_energy_rec_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < T5_ENERGY_MAX && n < max; i++) {
        if (db->rec[i].id[0]) out[n++] = db->rec[i];
    }
    qsort(out, (size_t)n, sizeof *out, by_avg);
    return n;
}

void t5_energy_forget(t5_energy_db_t *db, const char *id)
{
    t5_energy_rec_t *r = (t5_energy_rec_t *)t5_energy_find(db, id);
    if (r) memset(r, 0, sizeof *r);
}
