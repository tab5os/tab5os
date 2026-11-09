/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdlib.h>
#include "t5_ui/orient.h"

void t5_orient_default_cfg(t5_orient_cfg_t *c)
{
    c->hold_ms = 500;
    c->hyst_deg = 15;
    c->flat_min_mg = 570;
    c->min_mg = 500;
    c->max_mg = 1500;
}

void t5_orient_init(t5_orient_t *o, const t5_orient_cfg_t *cfg, t5_rotation_t initial)
{
    if (cfg) {
        o->cfg = *cfg;
    } else {
        t5_orient_default_cfg(&o->cfg);
    }
    o->current = initial;
    o->pending = false;
    o->candidate = initial;
    o->since_ms = 0;
    o->flat = false;
    o->angle_deg = 0;
}

/* Distance between two angles in degrees, 0..180. */
static int ang_dist(int a, int b)
{
    int d = abs(a - b) % 360;
    return d > 180 ? 360 - d : d;
}

bool t5_orient_update(t5_orient_t *o, int32_t ux, int32_t uy, int32_t uz, uint32_t now_ms,
                      uint32_t allowed, t5_rotation_t *out)
{
    bool changed = false;
    double plane = sqrt((double)ux * ux + (double)uy * uy);
    double total = sqrt(plane * plane + (double)uz * uz);

    if (total < o->cfg.min_mg || total > o->cfg.max_mg) {
        goto done;                      /* bad sample or shaking: ignore, keep pending */
    }
    o->flat = plane < o->cfg.flat_min_mg;
    if (o->flat) {
        o->pending = false;
        goto done;
    }
    /* Angle of "up" measured clockwise from the panel's top: portrait 0,
     * right edge up 90, upside down 180, left edge up 270. */
    int ang = (int)lround(atan2((double)ux, (double)-uy) * 180.0 / M_PI);
    if (ang < 0) ang += 360;
    o->angle_deg = (int16_t)ang;

    t5_rotation_t near = (t5_rotation_t)(((ang + 45) / 90) % 4);
    if (near == o->current || !(allowed & T5_ROT_MASK(near)) ||
        ang_dist(ang, (int)near * 90) > 45 - (int)o->cfg.hyst_deg) {
        /* Inside the current sector, not allowed, or in the dead band. A
         * reading in the dead band keeps a pending candidate's timer. */
        if (near == o->current || !(allowed & T5_ROT_MASK(near))) {
            o->pending = false;
        }
        goto done;
    }
    if (!o->pending || o->candidate != near) {
        o->pending = true;
        o->candidate = near;
        o->since_ms = now_ms;
        goto done;
    }
    if (now_ms - o->since_ms >= o->cfg.hold_ms) {
        o->current = near;
        o->pending = false;
        changed = true;
    }
done:
    /* The allowed set can change under us (an app with a fixed orientation). */
    if (allowed) {
        t5_rotation_t r = t5_orient_fallback(o->current, allowed);
        if (r != o->current) {
            o->current = r;
            o->pending = false;
            changed = true;
        }
    }
    if (out) *out = o->current;
    return changed;
}

t5_rotation_t t5_orient_fallback(t5_rotation_t current, uint32_t allowed)
{
    allowed &= T5_ROT_ALL;
    if (!allowed || (allowed & T5_ROT_MASK(current))) return current;
    t5_rotation_t partner = (t5_rotation_t)((current + 2) % 4);
    if (allowed & T5_ROT_MASK(partner)) return partner;
    for (int i = 0; i < 4; i++) {
        if (allowed & T5_ROT_MASK(i)) return (t5_rotation_t)i;
    }
    return current;
}

void t5_axis_map_apply(const t5_axis_map_t *m, const int16_t chip[3], int32_t panel[3])
{
    for (int i = 0; i < 3; i++) {
        panel[i] = (int32_t)chip[m->src[i] % 3] * m->sign[i];
    }
}
