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

/* a minus b: up to four pieces (above, below, left, right of the overlap). */
static int rect_minus(t5_rect_t a, t5_rect_t b, t5_rect_t out[4])
{
    if (b.x2 < a.x1 || b.x1 > a.x2 || b.y2 < a.y1 || b.y1 > a.y2) {
        out[0] = a;
        return 1;
    }
    int n = 0;
    if (b.y1 > a.y1) out[n++] = (t5_rect_t){ a.x1, a.y1, a.x2, b.y1 - 1 };
    if (b.y2 < a.y2) out[n++] = (t5_rect_t){ a.x1, b.y2 + 1, a.x2, a.y2 };
    int32_t y1 = b.y1 > a.y1 ? b.y1 : a.y1, y2 = b.y2 < a.y2 ? b.y2 : a.y2;
    if (b.x1 > a.x1) out[n++] = (t5_rect_t){ a.x1, y1, b.x1 - 1, y2 };
    if (b.x2 < a.x2) out[n++] = (t5_rect_t){ b.x2 + 1, y1, a.x2, y2 };
    return n;
}

int t5_fbt_subtract(const t5_rect_t *a, int na, const t5_rect_t *cut, int ncut, t5_rect_t *out, int max)
{
    /* Work list in out[]; each cut splits every piece. If the pieces don't
     * fit, the caller gets the uncut rectangles (copying a little more is
     * always correct). */
    if (na > max) na = max;
    for (int i = 0; i < na; i++) out[i] = a[i];
    int n = na;
    for (int c = 0; c < ncut; c++) {
        t5_rect_t next[64];
        int m = 0;
        bool overflow = false;
        for (int i = 0; i < n && !overflow; i++) {
            t5_rect_t parts[4];
            int k = rect_minus(out[i], cut[c], parts);
            if (m + k > max || m + k > 64) {
                overflow = true;
                break;
            }
            for (int j = 0; j < k; j++) next[m++] = parts[j];
        }
        if (overflow) return n;      /* keep what we have: a superset of the answer */
        for (int i = 0; i < m; i++) out[i] = next[i];
        n = m;
    }
    return n;
}
