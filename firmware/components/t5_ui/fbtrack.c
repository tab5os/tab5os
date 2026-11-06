/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_ui/fbtrack.h"

/* Rectangles closer than this are merged: a few extra copied pixels are
 * cheaper than another PPA transaction. */
#define MERGE_GAP 16

static t5_rect_t bbox(t5_rect_t a, t5_rect_t b)
{
    t5_rect_t o = {
        a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1,
        a.x2 > b.x2 ? a.x2 : b.x2, a.y2 > b.y2 ? a.y2 : b.y2,
    };
    return o;
}

static bool near(t5_rect_t a, t5_rect_t b)
{
    return a.x1 <= b.x2 + MERGE_GAP && b.x1 <= a.x2 + MERGE_GAP &&
           a.y1 <= b.y2 + MERGE_GAP && b.y1 <= a.y2 + MERGE_GAP;
}

static uint32_t area(t5_rect_t a)
{
    return (uint32_t)t5_rect_w(&a) * (uint32_t)t5_rect_h(&a);
}

static void list_add(t5_fbt_list_t *l, t5_rect_t r)
{
    /* Absorb into an existing rectangle when that doesn't grow the copy by
     * more than the two parts would cost apart; repeat, since one merge can
     * make another possible. */
    for (;;) {
        bool merged = false;
        for (int i = 0; i < l->n; i++) {
            if (!near(l->r[i], r)) continue;
            t5_rect_t m = bbox(l->r[i], r);
            if (area(m) <= area(l->r[i]) + area(r) + (uint32_t)MERGE_GAP * (uint32_t)t5_rect_w(&m)) {
                r = m;
                l->r[i] = l->r[--l->n];
                merged = true;
                break;
            }
        }
        if (!merged) break;
    }
    if (l->n < T5_FBT_RECTS) {
        l->r[l->n++] = r;
        return;
    }
    /* Full: collapse everything into one box. */
    t5_rect_t b = r;
    for (int i = 0; i < l->n; i++) b = bbox(b, l->r[i]);
    l->r[0] = b;
    l->n = 1;
}

void t5_fbt_init(t5_fbt_t *t, int32_t w, int32_t h)
{
    memset(t, 0, sizeof *t);
    t->bounds = (t5_rect_t){ 0, 0, w - 1, h - 1 };
    t5_fbt_all_dirty(t);
}

void t5_fbt_all_dirty(t5_fbt_t *t)
{
    for (int i = 0; i < T5_FBT_BUFS; i++) {
        t->list[i].r[0] = t->bounds;
        t->list[i].n = 1;
    }
}

void t5_fbt_add(t5_fbt_t *t, t5_rect_t r)
{
    if (!t5_rect_clip(&r, &t->bounds)) return;
    for (int i = 0; i < T5_FBT_BUFS; i++) {
        list_add(&t->list[i], r);
    }
}

int t5_fbt_take(t5_fbt_t *t, int i, t5_rect_t *out, int max)
{
    if (i < 0 || i >= T5_FBT_BUFS) return 0;
    t5_fbt_list_t *l = &t->list[i];
    if (l->n > max) {
        t5_rect_t b = l->r[0];
        for (int k = 1; k < l->n; k++) b = bbox(b, l->r[k]);
        l->r[0] = b;
        l->n = 1;
    }
    int n = l->n;
    memcpy(out, l->r, sizeof(t5_rect_t) * (size_t)n);
    l->n = 0;
    return n;
}

uint32_t t5_fbt_pixels(const t5_fbt_t *t, int i)
{
    uint32_t p = 0;
    for (int k = 0; k < t->list[i].n; k++) p += area(t->list[i].r[k]);
    return p;
}

int t5_fbt_pick(int nbufs, int scanout, int queued)
{
    for (int i = 0; i < nbufs; i++) {
        if (i != scanout && i != queued) return i;
    }
    return -1;
}
