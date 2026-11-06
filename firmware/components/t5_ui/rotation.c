/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_ui/rotation.h"

const char *t5_rot_name(t5_rotation_t r)
{
    switch (r) {
    case T5_ROT_0: return "0";
    case T5_ROT_90: return "90";
    case T5_ROT_180: return "180";
    case T5_ROT_270: return "270";
    }
    return "?";
}

void t5_rot_size(t5_rotation_t r, int32_t *w, int32_t *h)
{
    bool land = t5_rot_landscape(r);
    if (w) *w = land ? T5_PANEL_H : T5_PANEL_W;
    if (h) *h = land ? T5_PANEL_W : T5_PANEL_H;
}

void t5_rot_to_panel(t5_rotation_t r, int32_t lx, int32_t ly, int32_t *px, int32_t *py)
{
    switch (r) {
    case T5_ROT_90: *px = T5_PANEL_W - 1 - ly; *py = lx; break;
    case T5_ROT_180: *px = T5_PANEL_W - 1 - lx; *py = T5_PANEL_H - 1 - ly; break;
    case T5_ROT_270: *px = ly; *py = T5_PANEL_H - 1 - lx; break;
    default: *px = lx; *py = ly; break;
    }
}

void t5_rot_from_panel(t5_rotation_t r, int32_t px, int32_t py, int32_t *lx, int32_t *ly)
{
    switch (r) {
    case T5_ROT_90: *lx = py; *ly = T5_PANEL_W - 1 - px; break;
    case T5_ROT_180: *lx = T5_PANEL_W - 1 - px; *ly = T5_PANEL_H - 1 - py; break;
    case T5_ROT_270: *lx = T5_PANEL_H - 1 - py; *ly = px; break;
    default: *lx = px; *ly = py; break;
    }
}

t5_rect_t t5_rot_rect_to_panel(t5_rotation_t r, t5_rect_t a)
{
    int32_t x1, y1, x2, y2;
    t5_rot_to_panel(r, a.x1, a.y1, &x1, &y1);
    t5_rot_to_panel(r, a.x2, a.y2, &x2, &y2);
    t5_rect_t o = {
        x1 < x2 ? x1 : x2, y1 < y2 ? y1 : y2,
        x1 < x2 ? x2 : x1, y1 < y2 ? y2 : y1,
    };
    return o;
}

bool t5_rect_clip(t5_rect_t *a, const t5_rect_t *b)
{
    if (a->x1 < b->x1) a->x1 = b->x1;
    if (a->y1 < b->y1) a->y1 = b->y1;
    if (a->x2 > b->x2) a->x2 = b->x2;
    if (a->y2 > b->y2) a->y2 = b->y2;
    return a->x1 <= a->x2 && a->y1 <= a->y2;
}
