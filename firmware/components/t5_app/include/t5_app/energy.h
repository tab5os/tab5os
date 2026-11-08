/* SPDX-License-Identifier: Apache-2.0
 * Per-app power accounting (plan 6.8). The INA226 measures the whole pack,
 * so an app's draw is the system draw while it is in the foreground. Only
 * samples on battery count (on USB the pack current says nothing about the
 * load). Pure logic, host-tested.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define T5_ENERGY_MAX 32
#define T5_ENERGY_ID_LEN 48

/* Pseudo-ids for time with no app in front. */
#define T5_ENERGY_LAUNCHER "launcher"
#define T5_ENERGY_SCREEN_OFF "screen-off"

typedef struct {
    char id[T5_ENERGY_ID_LEN];
    uint64_t mj;              /* millijoules on battery */
    uint32_t ms;              /* foreground time on battery */
    uint32_t last_seen;       /* sequence number, for eviction */
} t5_energy_rec_t;

typedef struct {
    uint32_t version;
    uint32_t seq;
    t5_energy_rec_t rec[T5_ENERGY_MAX];
} t5_energy_db_t;

#define T5_ENERGY_DB_VERSION 1

void t5_energy_init(t5_energy_db_t *db);
/* Charge mw for ms to id. Negative mw (charging) is ignored. When the table
 * is full the least recently used entry is replaced. */
void t5_energy_add(t5_energy_db_t *db, const char *id, int32_t mw, uint32_t ms);
const t5_energy_rec_t *t5_energy_find(const t5_energy_db_t *db, const char *id);
/* Average draw in mW, -1 with under 10 s of samples. */
int32_t t5_energy_avg_mw(const t5_energy_rec_t *r);
/* Copy records sorted by average draw (highest first); returns the count. */
int t5_energy_sorted(const t5_energy_db_t *db, t5_energy_rec_t *out, int max);
void t5_energy_forget(t5_energy_db_t *db, const char *id);

#ifdef __cplusplus
}
#endif
