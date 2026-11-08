/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include "t5_app/tracker.h"

/* Header in front of every tracked block. 16-byte multiple keeps the
 * caller's pointer as aligned as malloc's. */
struct t5_heap_blk {
    uint32_t magic;
    uint32_t size;
    t5_heap_blk_t *prev, *next;
    t5_tracker_t *owner;
    uint32_t check;                 /* magic ^ address: rejects stale or foreign headers */
    uint32_t pad[2];
};

#define BLK_MAGIC 0x7A5EB10Cu
_Static_assert(sizeof(t5_heap_blk_t) % 8 == 0, "header keeps 8-byte alignment");

static void *sys_malloc(t5_tracker_t *t, size_t n) { (void)t; return malloc(n); }
static void sys_free(t5_tracker_t *t, void *p) { (void)t; free(p); }
static t5_tracker_alloc_t s_alloc = { sys_malloc, sys_free, NULL };

void t5_tracker_set_allocator(const t5_tracker_alloc_t *a)
{
    s_alloc = a ? *a : (t5_tracker_alloc_t){ sys_malloc, sys_free, NULL };
}

void t5_tracker_init(t5_tracker_t *t, uint32_t heap_limit_kb)
{
    memset(t, 0, sizeof *t);
    t->heap_limit_kb = heap_limit_kb;
}

static int find(const t5_tracker_t *t, void *h)
{
    for (int i = t->n - 1; i >= 0; i--) {
        if (t->ent[i].handle == h) return i;
    }
    return -1;
}

bool t5_tracker_add(t5_tracker_t *t, t5_res_kind_t kind, void *handle, t5_res_release_fn_t release)
{
    if (!handle || kind >= T5_RES_KIND_COUNT) return false;
    int i = find(t, handle);
    if (i >= 0) {
        t->counts[t->ent[i].kind]--;
        t->ent[i].kind = kind;
        t->ent[i].release = release;
        t->counts[kind]++;
        return true;
    }
    if (t->n >= T5_TRACK_MAX) {
        t->dropped++;
        return false;
    }
    t->ent[t->n++] = (t5_track_ent_t){ kind, handle, release };
    t->counts[kind]++;
    return true;
}

static void remove_at(t5_tracker_t *t, int i)
{
    t->counts[t->ent[i].kind]--;
    /* Keep order (release order within a kind is newest first). */
    memmove(&t->ent[i], &t->ent[i + 1], sizeof(t5_track_ent_t) * (size_t)(t->n - i - 1));
    t->n--;
}

bool t5_tracker_remove(t5_tracker_t *t, void *handle)
{
    int i = find(t, handle);
    if (i < 0) return false;
    remove_at(t, i);
    return true;
}

bool t5_tracker_has(const t5_tracker_t *t, void *handle)
{
    return find(t, handle) >= 0;
}

int t5_tracker_count(const t5_tracker_t *t, t5_res_kind_t kind)
{
    return kind < T5_RES_KIND_COUNT ? (int)t->counts[kind] : 0;
}

void t5_tracker_prune(t5_tracker_t *t, t5_res_kind_t kind, bool (*alive)(void *))
{
    for (int i = t->n - 1; i >= 0; i--) {
        if (t->ent[i].kind == kind && !alive(t->ent[i].handle)) remove_at(t, i);
    }
}

int t5_tracker_release_all(t5_tracker_t *t)
{
    int released = 0;
    for (int k = 0; k < T5_RES_KIND_COUNT; k++) {
        /* Releasing one handle can release others (deleting a UI root
         * deletes child timers' owners...), and release functions may call
         * t5_tracker_remove(); so rescan after every release. */
        for (;;) {
            int i;
            for (i = t->n - 1; i >= 0 && t->ent[i].kind != (t5_res_kind_t)k; i--) {}
            if (i < 0) break;
            t5_track_ent_t e = t->ent[i];
            remove_at(t, i);
            if (e.release) e.release(e.handle);
            released++;
        }
    }
    t5_tracker_free_all(t);
    return released;
}

/* ---- heap ------------------------------------------------------------------ */
static uint32_t check_of(const t5_heap_blk_t *b)
{
    return BLK_MAGIC ^ (uint32_t)(uintptr_t)b;
}

static t5_heap_blk_t *hdr(const void *p)
{
    return p ? (t5_heap_blk_t *)((uint8_t *)p - sizeof(t5_heap_blk_t)) : NULL;
}

bool t5_tracker_owns(const void *p)
{
    const t5_heap_blk_t *b = hdr(p);
    return b && b->magic == BLK_MAGIC && b->check == check_of(b) && b->owner;
}

size_t t5_tracker_block_size(const void *p)
{
    return t5_tracker_owns(p) ? hdr(p)->size : 0;
}

static void link_blk(t5_tracker_t *t, t5_heap_blk_t *b, size_t n)
{
    b->magic = BLK_MAGIC;
    b->size = (uint32_t)n;
    b->owner = t;
    b->check = check_of(b);
    b->prev = NULL;
    b->next = t->blocks;
    if (t->blocks) t->blocks->prev = b;
    t->blocks = b;
    t->heap_bytes += n;
    t->heap_blocks++;
    if (t->heap_bytes > t->heap_peak) t->heap_peak = t->heap_bytes;
}

static void unlink_blk(t5_tracker_t *t, t5_heap_blk_t *b)
{
    if (b->prev) b->prev->next = b->next;
    else t->blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    t->heap_bytes -= b->size;
    t->heap_blocks--;
    b->magic = 0;
    b->check = 0;
    b->owner = NULL;
}

static bool over_limit(t5_tracker_t *t, size_t n)
{
    return t->heap_limit_kb && (t->heap_bytes + n) > (size_t)t->heap_limit_kb * 1024u;
}

void *t5_tracker_malloc(t5_tracker_t *t, size_t n)
{
    if (n > UINT32_MAX - sizeof(t5_heap_blk_t) || over_limit(t, n)) {
        t->heap_refused++;
        return NULL;
    }
    t5_heap_blk_t *b = s_alloc.malloc(t, sizeof *b + (n ? n : 1));
    if (!b) return NULL;
    link_blk(t, b, n);
    return b + 1;
}

void *t5_tracker_calloc(t5_tracker_t *t, size_t n, size_t sz)
{
    if (sz && n > SIZE_MAX / sz) return NULL;
    void *p = t5_tracker_malloc(t, n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

void *t5_tracker_realloc(t5_tracker_t *t, void *p, size_t n)
{
    if (!p) return t5_tracker_malloc(t, n);
    t5_heap_blk_t *b = hdr(p);
    if (!t5_tracker_owns(p) || b->owner != t) return NULL;   /* caller falls back */
    if (n == 0) {
        t5_tracker_free(t, p);
        return NULL;
    }
    void *q = t5_tracker_malloc(t, n);
    if (!q) return NULL;
    memcpy(q, p, b->size < n ? b->size : n);
    t5_tracker_free(t, p);
    return q;
}

bool t5_tracker_free(t5_tracker_t *t, void *p)
{
    if (!p) return true;
    if (!t5_tracker_owns(p)) return false;
    t5_heap_blk_t *b = hdr(p);
    /* Another app's block (passed between apps through some service):
     * free it from its own owner so that owner's list stays right. */
    t5_tracker_t *owner = b->owner;
    (void)t;
    unlink_blk(owner, b);
    s_alloc.free(owner, b);
    return true;
}

void t5_tracker_free_all(t5_tracker_t *t)
{
    if (s_alloc.drop_all && s_alloc.drop_all(t)) {
        t->blocks = NULL;
        t->heap_bytes = 0;
        t->heap_blocks = 0;
        return;
    }
    while (t->blocks) {
        t5_heap_blk_t *b = t->blocks;
        unlink_blk(t, b);
        s_alloc.free(t, b);
    }
}
