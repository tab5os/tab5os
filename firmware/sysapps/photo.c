/* SPDX-License-Identifier: Apache-2.0
 * Full-screen photo viewer (.jpg), shared by Files and the Camera's
 * gallery, over the calling app's screen, with zoom: pinch, double tap, and
 * drag to pan once zoomed in.
 *
 * Photos go through the hardware JPEG decoder. The view is a video plane
 * (t5_ui/plane.h) the size of the screen: each change of zoom or position
 * posts the part of the photo to show and its scale, and the display
 * pipeline scales it straight into the framebuffer (one PPA pass). The UI
 * only draws the back button over it, so pinching and panning stay smooth
 * (a UI-scaled full-screen image redraws in software, ~10x slower).
 *
 * The PPA scales in steps of 1/16. Zoomed in, that is fine-grained enough;
 * zoomed far out on a big photo the steps are coarse, so there the scale is
 * split over two passes: a/16 here into an intermediate buffer, then b/16
 * on the way to the screen.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "t5/camera.h"
#include "t5_gui/t5ui_icons.h"
#include "t5_gui/t5ui_sys.h"
#include "t5_ui/plane.h"
#include "sysapps_priv.h"

#define PPA_CLIENT 0              /* the UI task's */
#define ZOOM_ANIM_MS 180

typedef struct {
    t5ui_obj_t *root, *view, *back;
    t5ui_obj_t *anim;             /* hidden: carries the zoom animation's progress (TRANSLATE_X, 0..1024) */
    void (*on_close)(void);
    /* the photo, decoded: pic_w x pic_h (rounded up to 16), iw x ih of it real */
    uint16_t *px;
    size_t px_bytes;
    int pic_w, pic_h, iw, ih;
    /* the view: a plane the size of root */
    t5_plane_t *plane;
    int vw, vh;
    float z, fit, zmax;           /* screen pixels per photo pixel */
    float cx, cy;                 /* the photo point at the view's centre */
    bool render_pending;          /* the plane hadn't taken the last render yet */
    /* the two-pass intermediates: one the screen may still read, one to fill */
    uint16_t *mid[2];
    size_t mid_bytes[2];
    uint32_t mid_seq[2];
    int mid_next;
    t5ui_timer_t *timer;
    /* gestures */
    bool pinching;
    int32_t last_x, last_y;       /* the pointer at the last press or pressing: the pan's step */
    float pinch_d0, pinch_z0, pinch_px, pinch_py;
    /* the double-tap zoom animation: z0 -> z1 keeping the photo point
     * (ax, ay) under the screen point (sx, sy) */
    float az0, az1, ax, ay, sx, sy;
    bool zooming;
    int32_t zoom_v;               /* the animation's progress last applied */
} photo_t;

static photo_t *s_p;

bool sa_photo_is_open(void)
{
    return s_p != NULL;
}

bool sa_ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

bool sa_photo_can_open(const char *path)
{
    return sa_ends_with(path, ".jpg") || sa_ends_with(path, ".jpeg");
}

static t5_err_t decode_jpeg(photo_t *p, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return T5_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* Straight from the file to the decoder and from it to us: no copies. */
    uint8_t *in = len > 0 ? t5_jpeg_alloc((size_t)len) : NULL;
    if (!in || fread(in, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(in);
        return in ? T5_ERR_IO : T5_ERR_NO_MEM;
    }
    fclose(f);
    uint16_t w = 0, h = 0, rw = 0, rh = 0;
    t5_err_t e = t5_jpeg_size(in, (size_t)len, &w, &h);
    if (e == T5_OK) e = t5_jpeg_dims(in, (size_t)len, &rw, &rh);
    size_t bytes = ((size_t)w * h * 2 + 127) & ~(size_t)127;
    uint16_t *out = e == T5_OK ? t5_jpeg_alloc(bytes) : NULL;
    if (e == T5_OK && !out) e = T5_ERR_NO_MEM;
    if (e == T5_OK) e = t5_jpeg_decode_dma(in, (size_t)len, out, bytes, &w, &h);
    free(in);
    if (e != T5_OK) {
        free(out);
        return e;
    }
    p->px = out;
    p->px_bytes = bytes;
    p->pic_w = w;
    p->pic_h = h;
    p->iw = rw && rw <= w ? rw : w;
    p->ih = rh && rh <= h ? rh : h;
    return T5_OK;
}

/* ---- rendering ------------------------------------------------------------------- */
/* How a zoom z is done: one pass at a/16 (b = 0), or two, a/16 then b/16.
 * Returns the scale that results. */
static float plan(float z, int *a, int *b)
{
    int k = (int)floorf(z * 16.0f + 1e-4f);
    if (k < 1) k = 1;
    if (k > 255 * 16) k = 255 * 16;
    *a = k;
    *b = 0;
    float best = (float)k / 16.0f;
    if (z >= 0.5f) return best;
    for (int x = 1; x <= 16; x++) {
        for (int y = x; y <= 16; y++) {
            float s = (float)(x * y) / 256.0f;
            if (s <= z + 1e-4f && s > best * 1.03f) {
                best = s;
                *a = y;           /* the bigger step first: a smaller intermediate */
                *b = x;
            }
        }
    }
    return best;
}

static float scale_of(float z)
{
    int a, b;
    return plan(z, &a, &b);
}

/* Keep the photo on screen: centred along an axis where it is smaller
 * than the view, its edges no further in than the view's elsewhere. */
static void clamp_centre(photo_t *p)
{
    float r = scale_of(p->z);
    float hw = (float)p->vw / 2.0f / r, hh = (float)p->vh / 2.0f / r;
    if ((float)p->iw <= 2.0f * hw) p->cx = (float)p->iw / 2.0f;
    else p->cx = fminf(fmaxf(p->cx, hw), (float)p->iw - hw);
    if ((float)p->ih <= 2.0f * hh) p->cy = (float)p->ih / 2.0f;
    else p->cy = fminf(fmaxf(p->cy, hh), (float)p->ih - hh);
}

static uint16_t *mid_buf(photo_t *p, int i, size_t bytes)
{
    if (p->mid_bytes[i] >= bytes) return p->mid[i];
    free(p->mid[i]);
    p->mid[i] = t5_jpeg_alloc(bytes);
    p->mid_bytes[i] = p->mid[i] ? ((bytes + 127) & ~(size_t)127) : 0;
    return p->mid[i];
}

/* One axis: the photo block [*b0, *b0 + *bn) that lands at view offset *o. */
static void span(float c, float r, int view, int img, int *b0, int *bn, int *o)
{
    float first = c - (float)view / 2.0f / r;
    int b = first < 0 ? 0 : (int)floorf(first);
    float off = ((float)b - c) * r + (float)view / 2.0f;
    int oo = off < 0 ? 0 : (int)lroundf(off);
    int n = (int)floorf((float)(view - oo) / r);
    if (n > img - b) n = img - b;
    *b0 = b;
    *bn = n < 1 ? 1 : n;
    *o = oo;
}

static void render(photo_t *p)
{
    if (!t5_plane_ready(p->plane)) {
        p->render_pending = true;     /* the timer tries again */
        return;
    }
    int a, b;
    float r = plan(p->z, &a, &b);
    int bx, bw, ox, by, bh, oy;
    span(p->cx, r, p->vw, p->iw, &bx, &bw, &ox);
    span(p->cy, r, p->vh, p->ih, &by, &bh, &oy);
    t5_plane_src_t src = {
        .buf = p->px, .fmt = T5_PLANE_RGB565, .pic_w = (uint16_t)p->pic_w, .pic_h = (uint16_t)p->pic_h,
        .x = (uint16_t)bx, .y = (uint16_t)by, .w = (uint16_t)bw, .h = (uint16_t)bh, .k = (uint16_t)a,
        .dx = (uint16_t)ox, .dy = (uint16_t)oy,
    };
    if (b) {
        /* The first step here, into an intermediate the screen is done with. */
        int i = p->mid_next;
        if (p->mid_seq[i] && !t5_plane_released(p->plane, p->mid_seq[i])) {
            p->render_pending = true;
            return;
        }
        int mw = sa_ppa_scaled(bw, a), mh = sa_ppa_scaled(bh, a);
        uint16_t *m = mid_buf(p, i, (size_t)mw * mh * 2);
        if (!m) return;
        sa_pic_t in = { p->px, p->pic_w, p->pic_h, p->px_bytes };
        sa_pic_t mid = { m, mw, mh, p->mid_bytes[i] };
        if (sa_ppa_scale(PPA_CLIENT, &in, bx, by, bw, bh, a, &mid, 0, 0) != T5_OK) return;
        src = (t5_plane_src_t){
            .buf = m, .fmt = T5_PLANE_RGB565, .pic_w = (uint16_t)mw, .pic_h = (uint16_t)mh,
            .w = (uint16_t)mw, .h = (uint16_t)mh, .k = (uint16_t)b, .dx = (uint16_t)ox, .dy = (uint16_t)oy,
        };
        p->render_pending = false;
        p->mid_seq[i] = t5_plane_post(p->plane, &src);
        p->mid_next = i ^ 1;
        return;
    }
    p->render_pending = false;
    t5_plane_post(p->plane, &src);
}

static void zoom_step(photo_t *p, int32_t v);

static void timer_cb(t5ui_timer_t *t, void *user)
{
    (void)t;
    photo_t *p = user;
    /* The double-tap zoom: its progress, eased, as the animation has it now. */
    if (p->zooming) {
        int32_t v = t5ui_get_int(p->anim, T5UI_P_TRANSLATE_X, p->zoom_v);
        if (v != p->zoom_v) zoom_step(p, v);
    }
    if (p->render_pending) render(p);
}

/* Zoom to z keeping the photo point (px, py) under the view point (vx, vy). */
static void zoom_about(photo_t *p, float z, float px, float py, float vx, float vy)
{
    p->z = fminf(fmaxf(z, p->fit), p->zmax);
    float r = scale_of(p->z);
    p->cx = px - (vx - (float)p->vw / 2.0f) / r;
    p->cy = py - (vy - (float)p->vh / 2.0f) / r;
    clamp_centre(p);
    render(p);
}

/* The photo point under view point (vx, vy) now. */
static void photo_point(const photo_t *p, float vx, float vy, float *px, float *py)
{
    float r = scale_of(p->z);
    *px = p->cx + (vx - (float)p->vw / 2.0f) / r;
    *py = p->cy + (vy - (float)p->vh / 2.0f) / r;
}

/* ---- the view and its size ------------------------------------------------------------ */
static void make_view(photo_t *p)
{
    t5ui_rect_t rc;
    t5ui_coords(p->root, &rc);
    int32_t vw = rc.x2 - rc.x1 + 1, vh = rc.y2 - rc.y1 + 1;
    if (vw < 16 || vh < 16) return;
    float rel = p->fit > 0 ? p->z / p->fit : 1.0f;    /* keep the zoom across a rotation */
    t5_plane_t *pl = t5_plane_new(vw, vh);
    if (!pl) return;
    /* The plane's view is platform code's: an object of its own, taken in here. */
    t5ui_obj_t *view = t5ui_sys_obj(t5_ui_plane_create(t5ui_sys_native(p->root), pl));
    if (!view) {
        t5_plane_release(pl);
        return;
    }
    if (p->view) t5ui_delete(p->view);    /* closed: it reads neither intermediate now */
    t5_plane_release(p->plane);
    p->plane = pl;
    p->view = view;
    p->mid_seq[0] = p->mid_seq[1] = 0;
    t5ui_move_to(view, 0);            /* under the back button */
    p->vw = vw;
    p->vh = vh;
    float fx = (float)vw / (float)p->iw, fy = (float)vh / (float)p->ih;
    p->fit = fminf(fminf(fx, fy), 1.0f);
    p->zmax = fmaxf(p->fit * 4.0f, 3.0f);
    p->z = fminf(p->fit * rel, p->zmax);
    if (p->cx == 0 && p->cy == 0) {
        p->cx = (float)p->iw / 2.0f;
        p->cy = (float)p->ih / 2.0f;
    }
    clamp_centre(p);
    render(p);
}

static void resized(t5ui_event_t *e)
{
    photo_t *p = t5ui_event_user(e);
    t5ui_rect_t rc;
    t5ui_coords(p->root, &rc);
    if (rc.x2 - rc.x1 + 1 != p->vw || rc.y2 - rc.y1 + 1 != p->vh) make_view(p);
}

/* ---- gestures --------------------------------------------------------------------------- */
/* One step of the double-tap zoom, v of 1024 along. */
static void zoom_step(photo_t *p, int32_t v)
{
    p->zoom_v = v;
    float t = (float)v / 1024.0f;
    zoom_about(p, p->az0 + (p->az1 - p->az0) * t, p->ax, p->ay, p->sx, p->sy);
}

static void zoom_stop(photo_t *p)
{
    p->zooming = false;
    t5ui_animate_stop(p->anim, T5UI_P_TRANSLATE_X);
}

static void zoom_end(t5ui_event_t *e)
{
    photo_t *p = t5ui_event_user(e);
    if (!p->zooming || t5ui_event_index(e) != T5UI_P_TRANSLATE_X) return;
    p->zooming = false;
    zoom_step(p, 1024);
}

static void double_tap(photo_t *p, int32_t x, int32_t y)
{
    bool zoomed = p->z > p->fit * 1.05f;
    float target = zoomed ? p->fit : fminf(fmaxf(p->fit * 2.5f, 1.0f), p->zmax);
    p->az0 = p->z;
    p->az1 = target;
    photo_point(p, (float)x, (float)y, &p->ax, &p->ay);
    /* Zooming in, the tapped point stays under the finger; out, the photo
     * settles back to the centre. */
    p->sx = zoomed ? (float)p->vw / 2.0f : (float)x;
    p->sy = zoomed ? (float)p->vh / 2.0f : (float)y;
    if (zoomed) {
        p->ax = (float)p->iw / 2.0f;
        p->ay = (float)p->ih / 2.0f;
    }
    /* The animation eases a number from 0 to 1024; the timer applies it
     * (zoom_step) as it goes, and zoom_end the last step. */
    p->zoom_v = -1;
    p->zooming = t5ui_animate(p->anim, T5UI_P_TRANSLATE_X, 0, 1024, ZOOM_ANIM_MS, 0, T5UI_EASE_OUT) == T5_OK;
    if (p->zooming) zoom_step(p, 0);
}

static void to_view(const photo_t *p, int32_t sx, int32_t sy, float *vx, float *vy)
{
    t5ui_rect_t c;
    t5ui_coords(p->root, &c);
    *vx = (float)(sx - c.x1);
    *vy = (float)(sy - c.y1);
}

static void gesture(t5ui_event_t *e)
{
    photo_t *p = t5ui_event_user(e);
    t5ui_ev_t code = t5ui_event_kind(e);
    int32_t px = 0, py = 0;
    bool have_pt = t5ui_event_point(e, &px, &py);
    if (code == T5UI_EV_PRESS) {
        p->pinching = false;
        p->last_x = px;
        p->last_y = py;
        zoom_stop(p);
        return;
    }
    if (code == T5UI_EV_RELEASE) {
        p->pinching = false;
        return;
    }
    if (code == T5UI_EV_DOUBLE_CLICK && have_pt) {
        float vx, vy;
        to_view(p, px, py, &vx, &vy);
        double_tap(p, (int32_t)vx, (int32_t)vy);
        return;
    }
    if (code != T5UI_EV_PRESSING) return;
    /* The pointer's move since the last press or pressing: the pan's step. */
    int32_t vx_step = have_pt ? px - p->last_x : 0, vy_step = have_pt ? py - p->last_y : 0;
    if (have_pt) {
        p->last_x = px;
        p->last_y = py;
    }
    t5ui_touch_t tp[2];
    int n = t5ui_touches(tp, 2);
    if (n >= 2) {
        float dx = (float)(tp[1].x - tp[0].x), dy = (float)(tp[1].y - tp[0].y);
        float d = sqrtf(dx * dx + dy * dy);
        float mx, my;
        to_view(p, (tp[0].x + tp[1].x) / 2, (tp[0].y + tp[1].y) / 2, &mx, &my);
        if (d < 1.0f) return;
        if (!p->pinching) {
            p->pinching = true;
            p->pinch_d0 = d;
            p->pinch_z0 = p->z;
            photo_point(p, mx, my, &p->pinch_px, &p->pinch_py);
            return;
        }
        zoom_about(p, p->pinch_z0 * d / p->pinch_d0, p->pinch_px, p->pinch_py, mx, my);
        return;
    }
    /* One finger: pan, once zoomed in (not right after a pinch, when the
     * other finger lifts first and the jump would show). */
    if (p->pinching || !have_pt || p->z <= p->fit * 1.001f) return;
    if (!vx_step && !vy_step) return;
    float r = scale_of(p->z);
    p->cx -= (float)vx_step / r;
    p->cy -= (float)vy_step / r;
    clamp_centre(p);
    render(p);
}

static void close_clicked(t5ui_event_t *e)
{
    (void)e;
    sa_photo_close();
}

bool sa_photo_open(t5ui_obj_t *screen, const char *path, void (*on_close)(void))
{
    sa_photo_close();
    if (!sa_photo_can_open(path)) return false;
    photo_t *p = calloc(1, sizeof *p);
    if (!p) return false;
    t5_err_t err = decode_jpeg(p, path);
    if (err != T5_OK) {
        free(p);
        sa_report(err, "open this photo");
        return false;
    }
    p->on_close = on_close;
    p->root = t5ui_create(screen, T5UI_BOX);
    t5ui_set_int(p->root, T5UI_P_FLOATING, true);
    t5ui_set_int(p->root, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(p->root, T5UI_P_HEIGHT, T5UI_PCT(100));
    t5ui_set_color(p->root, T5UI_P_BG_COLOR, 0x000000);
    t5ui_set_int(p->root, T5UI_P_BG_OPA, T5UI_OPA_COVER);
    t5ui_set_int(p->root, T5UI_P_SCROLLABLE, false);
    t5ui_set_int(p->root, T5UI_P_CLICKABLE, true);    /* nothing underneath gets the taps */
    t5ui_on(p->root, T5UI_EV_PRESS, gesture, p);
    t5ui_on(p->root, T5UI_EV_PRESSING, gesture, p);
    t5ui_on(p->root, T5UI_EV_RELEASE, gesture, p);
    t5ui_on(p->root, T5UI_EV_DOUBLE_CLICK, gesture, p);
    p->anim = t5ui_create(p->root, T5UI_BOX);
    t5ui_set_int(p->anim, T5UI_P_HIDDEN, true);
    t5ui_set_int(p->anim, T5UI_P_FLOATING, true);
    t5ui_on(p->anim, T5UI_EV_ANIM_END, zoom_end, p);

    /* Back, top left, in the Camera's round-button style: the viewer is a
     * page the user returns from, not a dialog to dismiss. A dark disc so it
     * reads over bright photos. */
    t5ui_obj_t *b = t5ui_create(p->root, T5UI_BUTTON);
    t5ui_style_clear(b);
    t5ui_set_int(b, T5UI_P_WIDTH, t5ui_dp(44));
    t5ui_set_int(b, T5UI_P_HEIGHT, t5ui_dp(44));
    t5ui_set_int(b, T5UI_P_RADIUS, T5UI_RADIUS_CIRCLE);
    t5ui_set_color(b, T5UI_P_BG_COLOR, 0x000000);
    t5ui_set_int(b, T5UI_P_BG_OPA, 127);                                         /* 50% */
    t5ui_set_int_at(b, T5UI_P_BG_OPA, T5UI_AT(T5UI_PART_MAIN, T5UI_STATE_PRESSED), 204);   /* 80% */
    t5ui_obj_t *l = t5ui_create(b, T5UI_MD_ICON);
    t5ui_set_str(l, T5UI_P_ICON, T5UI_ICON_ARROW_BACK);
    t5ui_set_color(l, T5UI_P_TEXT_COLOR, 0xffffff);
    t5ui_set_int(l, T5UI_P_ALIGN, T5UI_ALIGN_CENTER);
    t5ui_on(b, T5UI_EV_CLICK, close_clicked, NULL);
    t5ui_set_int(b, T5UI_P_ALIGN, T5UI_ALIGN_TOP_LEFT);
    t5ui_set_int(b, T5UI_P_X, t5ui_dp(12));
    t5ui_set_int(b, T5UI_P_Y, t5ui_dp(12));
    p->back = b;

    s_p = p;
    t5ui_layout_now(p->root);
    make_view(p);
    if (!p->view) {
        sa_photo_close();
        sa_report(T5_ERR_NO_MEM, "open this photo");
        return false;
    }
    t5ui_on(p->root, T5UI_EV_RESIZE, resized, p);
    p->timer = t5ui_timer(10, timer_cb, p);
    return true;
}

void sa_photo_close(void)
{
    photo_t *p = s_p;
    if (!p) return;
    s_p = NULL;
    zoom_stop(p);
    if (p->timer) t5ui_timer_delete(p->timer);
    t5ui_delete(p->root);           /* the view lets go of the plane, and of our buffers */
    t5_plane_release(p->plane);
    free(p->px);
    free(p->mid[0]);
    free(p->mid[1]);
    void (*cb)(void) = p->on_close;
    free(p);
    if (cb) cb();
}
