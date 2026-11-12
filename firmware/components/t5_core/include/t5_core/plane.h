/* SPDX-License-Identifier: Apache-2.0
 * Video planes: a picture that a producer (the camera's preview, the video
 * player, the photo viewer) updates on its own task and the UI shows
 * without drawing it through LVGL.
 *
 * The producer doesn't copy pixels anywhere. It posts a description of
 * where its frame is (its own buffer, RGB565 or YUV420), which part of it
 * to show, how much to scale it (k/16, the PPA's steps) and how to turn it.
 * The display pipeline does that in one PPA pass straight into the
 * framebuffer, turned for the panel at the same time (t5_ui/plane.h): one
 * pass per frame, where scaling into a buffer and then copying that to the
 * screen took two.
 *
 * So the producer's buffer is read after it posts: it must stay as it is
 * until the plane releases that frame, which happens when the next frame
 * is taken (the UI may draw the current one again meanwhile, when
 * something over it changes) or when the plane is closed. One frame waits
 * at a time: post again once t5_plane_ready().
 *
 * Reference counted: the UI holds one reference for as long as the picture
 * is on screen, and each producer holds its own. Once the UI lets go
 * (t5_plane_closed) every frame is released, and the producer should stop.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct t5_plane t5_plane_t;

typedef enum {
    T5_PLANE_RGB565,
    T5_PLANE_YUV420,          /* the PPA's packed layout (the camera's O_UYY_E_VYY) */
} t5_plane_fmt_t;

/* One frame. The block (x, y, w, h) of the pic_w x pic_h picture at buf
 * is scaled by k/16 (1..4095; rows by ky/16 if ky isn't 0), turned rot
 * degrees counter-clockwise (0, 90, 180, 270), then mirrored left to right
 * if mirror, and lands at (dx, dy) in the plane. What it doesn't cover of the plane is black. Buffers are PSRAM
 * the PPA can read (DMA-capable, 64-byte aligned); YUV420 blocks have even
 * offsets and sizes. */
typedef struct {
    const void *buf;
    uint8_t fmt;              /* t5_plane_fmt_t */
    bool mirror;
    uint16_t rot;
    uint16_t pic_w, pic_h;
    uint16_t x, y, w, h;
    uint16_t k, ky;
    uint16_t dx, dy;
} t5_plane_src_t;

/* The size the block lands as: scaled as the PPA rounds, then turned. */
void t5_plane_src_out(const t5_plane_src_t *s, int *w, int *h);

/* A w x h plane (logical pixels). One reference, the caller's. NULL if out
 * of memory or the size is bad. */
t5_plane_t *t5_plane_new(int w, int h);
void t5_plane_hold(t5_plane_t *p);
/* Frees the plane with the last reference. NULL is ignored. */
void t5_plane_release(t5_plane_t *p);

int t5_plane_w(const t5_plane_t *p);
int t5_plane_h(const t5_plane_t *p);

/* Producer: may a frame be posted (none is waiting, and the plane is
 * open)? */
bool t5_plane_ready(const t5_plane_t *p);
/* Producer: show this frame (copied). Returns its number, 0 if not ready. */
uint32_t t5_plane_post(t5_plane_t *p, const t5_plane_src_t *src);
/* Producer: is the UI done with frame seq's buffer? */
bool t5_plane_released(const t5_plane_t *p, uint32_t seq);

/* Producer: no more frames, and the buffers are about to go. Returns once
 * the UI has stopped reading them (it keeps a copy of the last frame to
 * show). Any task. */
void t5_plane_end(t5_plane_t *p);

/* Consumer: take the posted frame, if there is one, as the current one
 * (releasing the one before). */
bool t5_plane_take(t5_plane_t *p, t5_plane_src_t *out);
/* Consumer: what t5_plane_end runs (on the producer's task) to let go of
 * the current frame. */
void t5_plane_set_end_cb(t5_plane_t *p, void (*cb)(t5_plane_t *p, void *arg), void *arg);

/* Consumer: nothing will show this plane any more; every frame is
 * released. */
void t5_plane_close(t5_plane_t *p);
bool t5_plane_closed(const t5_plane_t *p);

/* Frames posted and taken since it was made. */
uint32_t t5_plane_posted(const t5_plane_t *p);
uint32_t t5_plane_taken(const t5_plane_t *p);

#ifdef __cplusplus
}
#endif
