/* SPDX-License-Identifier: Apache-2.0
 * Video planes (plane.h). One producer, one consumer: the producer writes
 * the pending frame only while none is waiting (posted == taken), and the
 * consumer reads it only once posted > taken, so the frame slot needs no
 * lock, only the order the sequence numbers are published in.
 */
#include <stdlib.h>
#include <string.h>
#include "t5_core/plane.h"

#define PLANE_MAGIC 0x504c4e32u

struct t5_plane {
    uint32_t magic;
    int w, h;
    int refs;                   /* atomic */
    bool closed;                /* atomic */
    uint32_t posted, taken;     /* atomic sequence numbers */
    t5_plane_src_t pending;
    void (*end_cb)(t5_plane_t *p, void *arg);
    void *end_arg;
};

void t5_plane_set_end_cb(t5_plane_t *p, void (*cb)(t5_plane_t *p, void *arg), void *arg)
{
    p->end_arg = arg;
    p->end_cb = cb;
}

void t5_plane_end(t5_plane_t *p)
{
    if (!p) return;
    /* A frame posted but never taken is dropped: its buffer goes too. */
    __atomic_store_n(&p->taken, __atomic_load_n(&p->posted, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    if (p->end_cb && !t5_plane_closed(p)) p->end_cb(p, p->end_arg);
}

void t5_plane_src_out(const t5_plane_src_t *s, int *w, int *h)
{
    /* The PPA's own rounding (esp_driver_ppa, ppa_srm.c). */
    int sw = (s->k / 16) * s->w + (s->k % 16) * s->w / 16;
    int ky = s->ky ? s->ky : s->k;
    int sh = (ky / 16) * s->h + (ky % 16) * s->h / 16;
    bool turned = s->rot == 90 || s->rot == 270;
    *w = turned ? sh : sw;
    *h = turned ? sw : sh;
}

t5_plane_t *t5_plane_new(int w, int h)
{
    if (w < 1 || h < 1 || w > 4096 || h > 4096) return NULL;
    t5_plane_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->magic = PLANE_MAGIC;
    p->w = w;
    p->h = h;
    p->refs = 1;
    return p;
}

void t5_plane_hold(t5_plane_t *p)
{
    if (p) __atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
}

void t5_plane_release(t5_plane_t *p)
{
    if (!p || __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    p->magic = 0;
    free(p);
}

int t5_plane_w(const t5_plane_t *p)
{
    return p->w;
}

int t5_plane_h(const t5_plane_t *p)
{
    return p->h;
}

bool t5_plane_ready(const t5_plane_t *p)
{
    return p && !__atomic_load_n(&p->closed, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&p->posted, __ATOMIC_ACQUIRE) == __atomic_load_n(&p->taken, __ATOMIC_ACQUIRE);
}

uint32_t t5_plane_post(t5_plane_t *p, const t5_plane_src_t *src)
{
    if (!src || !t5_plane_ready(p)) return 0;
    p->pending = *src;
    return __atomic_add_fetch(&p->posted, 1, __ATOMIC_ACQ_REL);
}

bool t5_plane_released(const t5_plane_t *p, uint32_t seq)
{
    return !p || __atomic_load_n(&p->closed, __ATOMIC_ACQUIRE) || __atomic_load_n(&p->taken, __ATOMIC_ACQUIRE) > seq;
}

bool t5_plane_take(t5_plane_t *p, t5_plane_src_t *out)
{
    if (!p) return false;
    uint32_t posted = __atomic_load_n(&p->posted, __ATOMIC_ACQUIRE);
    if (posted == __atomic_load_n(&p->taken, __ATOMIC_ACQUIRE)) return false;
    *out = p->pending;
    __atomic_store_n(&p->taken, posted, __ATOMIC_RELEASE);
    return true;
}

void t5_plane_close(t5_plane_t *p)
{
    if (p) __atomic_store_n(&p->closed, true, __ATOMIC_RELEASE);
}

bool t5_plane_closed(const t5_plane_t *p)
{
    return !p || __atomic_load_n(&p->closed, __ATOMIC_ACQUIRE);
}

uint32_t t5_plane_posted(const t5_plane_t *p)
{
    return __atomic_load_n(&p->posted, __ATOMIC_ACQUIRE);
}

uint32_t t5_plane_taken(const t5_plane_t *p)
{
    return __atomic_load_n(&p->taken, __ATOMIC_ACQUIRE);
}
