/* SPDX-License-Identifier: Apache-2.0
 * Per-app resource tracker (plan 5.4). Pure logic, host-tested.
 *
 * Two parts: a table of handles (tasks, timers, animations, UI roots, port
 * claims, rail holds, wake locks...) each with its release function, freed
 * in t5_res_kind_t order; and the app's heap, where every block carries a
 * small header linking it into the app's list, so whatever the app forgot
 * to free is freed on exit.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/res.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_TRACK_MAX 256

typedef struct {
    t5_res_kind_t kind;
    void *handle;
    t5_res_release_fn_t release;
} t5_track_ent_t;

typedef struct t5_heap_blk t5_heap_blk_t;

typedef struct {
    t5_track_ent_t ent[T5_TRACK_MAX];
    int n;
    uint32_t dropped;               /* adds refused because the table was full */
    /* heap */
    t5_heap_blk_t *blocks;          /* doubly linked */
    size_t heap_bytes, heap_peak;
    uint32_t heap_blocks;
    uint32_t heap_limit_kb;         /* 0 = none */
    uint32_t heap_refused;
    uint32_t counts[T5_RES_KIND_COUNT];
} t5_tracker_t;

void t5_tracker_init(t5_tracker_t *t, uint32_t heap_limit_kb);
/* Returns false if the table is full (the handle then leaks on exit). A
 * handle already present is updated, not added twice. */
bool t5_tracker_add(t5_tracker_t *t, t5_res_kind_t kind, void *handle, t5_res_release_fn_t release);
bool t5_tracker_remove(t5_tracker_t *t, void *handle);
bool t5_tracker_has(const t5_tracker_t *t, void *handle);
int t5_tracker_count(const t5_tracker_t *t, t5_res_kind_t kind);
/* Drop entries for which still_alive() says false (e.g. finished animations). */
void t5_tracker_prune(t5_tracker_t *t, t5_res_kind_t kind, bool (*still_alive)(void *handle));
/* Release everything, kind by kind in enum order, newest first within a
 * kind; then free the heap. Returns how many handles were released. */
int t5_tracker_release_all(t5_tracker_t *t);

/* Heap: blocks come from the system allocator with a header in front. */
void *t5_tracker_malloc(t5_tracker_t *t, size_t n);
void *t5_tracker_calloc(t5_tracker_t *t, size_t n, size_t sz);
void *t5_tracker_realloc(t5_tracker_t *t, void *p, size_t n);
/* Frees p if it is one of t's blocks and returns true; false for memory
 * that isn't (the caller then hands it to the system free). */
bool t5_tracker_free(t5_tracker_t *t, void *p);
/* True if p was allocated by any tracker (header check). */
bool t5_tracker_owns(const void *p);
/* Size of a tracked block (0 if p isn't one). */
size_t t5_tracker_block_size(const void *p);
void t5_tracker_free_all(t5_tracker_t *t);

/* Allocator under the tracker (system malloc; tests can count calls). */
typedef struct {
    void *(*malloc)(size_t);
    void (*free)(void *);
} t5_tracker_alloc_t;
void t5_tracker_set_allocator(const t5_tracker_alloc_t *a);

#ifdef __cplusplus
}
#endif
