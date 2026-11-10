/* SPDX-License-Identifier: Apache-2.0
 * Installed apps' own icons (t5_shell/app_icon.h).
 *
 * The PNG is decoded, resized to the size it's drawn at and cropped to an
 * anti-aliased circle here, once, so drawing it is a plain ARGB8888 blit:
 * no scaling or clipping layer per frame, and every icon has the launcher's
 * round shape whatever the artwork. Each size is cached per app until
 * t5_pkg_generation() moves (an install, upgrade or removal), which is
 * checked on every lookup, so a reinstalled icon shows on the next return
 * home. An entry still shown by an image outlives its generation until
 * that image is deleted.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop: not in lvgl.h */
#include "t5_core/log.h"
#include "t5_pkg/pkg.h"
#include "t5_pkg/png.h"
#include "t5_shell/app_icon.h"

static const char *TAG = "t5.shell";

#define ICON_FILE_MAX (512 * 1024)
#define ICON_MIN_PX 32
#define ICON_MAX_PX 256
/* Filter taps per output pixel: the scale is at most
 * T5_PNG_MAX_DIM / ICON_MIN_PX, and a tent that wide spans twice that. */
#define TAPS_MAX (2 * T5_PNG_MAX_DIM / ICON_MIN_PX + 3)

typedef struct icon_s {
    struct icon_s *next;
    char id[T5_APP_ID_LEN];
    int32_t px;
    uint32_t gen;
    int refs;               /* images showing it */
    lv_image_dsc_t dsc;     /* dsc.data NULL: the app has no usable icon */
} icon_t;

static icon_t *s_icons;

/* Output pixel i's source taps along one axis: a tent as wide as the scale
 * when shrinking (close to an area average, so fine detail doesn't
 * shimmer), and plain bilinear when growing. Returns the count, from *lo. */
static int taps(int i, int src, int dst, int *lo, float *w)
{
    float scale = (float)src / (float)dst;
    float support = scale > 1.0f ? scale : 1.0f;
    float center = ((float)i + 0.5f) * scale - 0.5f;
    int a = (int)ceilf(center - support), b = (int)floorf(center + support);
    int n = 0;
    float sum = 0;
    *lo = a;
    for (int k = a; k <= b && n < TAPS_MAX; k++, n++) {
        float v = 1.0f - fabsf((float)k - center) / support;
        w[n] = v > 0 ? v : 0;
        sum += w[n];
    }
    for (int k = 0; k < n && sum > 0; k++) w[k] /= sum;
    return n;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* src (straight RGBA) to px by px LVGL ARGB8888 (B, G, R, A in memory),
 * cropped to a circle. Filtering runs on premultiplied values, so
 * transparent pixels don't bleed their colour into the edges. */
static uint8_t *render(const t5_png_t *src, int px)
{
    uint8_t *out = malloc((size_t)px * (size_t)px * 4);
    float *row = malloc((size_t)src->w * 4 * sizeof(float));
    float wy[TAPS_MAX], wx[TAPS_MAX];
    if (!out || !row) {
        free(out);
        free(row);
        return NULL;
    }
    int sw = (int)src->w, sh = (int)src->h;
    float r = (float)px / 2.0f;
    for (int y = 0; y < px; y++) {
        /* This output row, filtered down the columns. */
        int y0, ny = taps(y, sh, px, &y0, wy);
        memset(row, 0, (size_t)sw * 4 * sizeof(float));
        for (int k = 0; k < ny; k++) {
            if (wy[k] == 0) continue;
            const uint8_t *s = src->rgba + (size_t)clampi(y0 + k, 0, sh - 1) * (size_t)sw * 4;
            for (int x = 0; x < sw; x++, s += 4) {
                float a = s[3] * wy[k];
                float *d = row + 4 * x;
                d[0] += s[0] * a / 255.0f;
                d[1] += s[1] * a / 255.0f;
                d[2] += s[2] * a / 255.0f;
                d[3] += a;
            }
        }
        /* ...then along it, un-premultiplied and masked to the circle. */
        uint8_t *o = out + (size_t)y * (size_t)px * 4;
        float dy = (float)y + 0.5f - r;
        for (int x = 0; x < px; x++, o += 4) {
            int x0, nx = taps(x, sw, px, &x0, wx);
            float c[4] = { 0 };
            for (int k = 0; k < nx; k++) {
                const float *s = row + 4 * clampi(x0 + k, 0, sw - 1);
                for (int j = 0; j < 4; j++) c[j] += s[j] * wx[k];
            }
            float dx = (float)x + 0.5f - r;
            float cover = r - sqrtf(dx * dx + dy * dy) + 0.5f;
            cover = cover < 0 ? 0 : cover > 1 ? 1 : cover;
            float a = c[3];
            for (int j = 0; j < 3; j++) {
                float v = a > 0 ? c[j] * 255.0f / a : 0;
                o[2 - j] = (uint8_t)(v > 255 ? 255 : v + 0.5f);   /* R, G, B into B, G, R order */
            }
            float av = a * cover;
            o[3] = (uint8_t)(av > 255 ? 255 : av + 0.5f);
        }
    }
    free(row);
    return out;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n > 0 && n <= ICON_FILE_MAX && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

/* Fill e from the app's package; leaves dsc.data NULL if there's no icon. */
static void load(icon_t *e)
{
    t5_pkg_info_t *info = malloc(sizeof *info);
    if (!info) return;
    if (t5_pkg_find(e->id, info) != T5_OK || !info->manifest.icon[0]) {
        free(info);
        return;
    }
    char path[T5_PKG_PATH_MAX + T5_MANIFEST_FILE_LEN];
    snprintf(path, sizeof path, "%s/%s", info->dir, info->manifest.icon);
    free(info);
    size_t len = 0;
    uint8_t *file = read_file(path, &len);
    if (!file) {
        T5_LOGW(TAG, "%s: can't read its icon %s (missing, empty or over %d KB)", e->id, path, ICON_FILE_MAX / 1024);
        return;
    }
    t5_png_t png;
    t5_err_t err = t5_png_decode(file, len, T5_PNG_MAX_DIM, &png);
    free(file);
    if (err != T5_OK) {
        T5_LOGW(TAG, "%s: icon %s isn't a usable PNG (%s): it needs to be at most %dx%d and not interlaced", e->id,
                path, t5_err_str(err), T5_PNG_MAX_DIM, T5_PNG_MAX_DIM);
        return;
    }
    uint8_t *px = render(&png, (int)e->px);
    free(png.rgba);
    if (!px) return;
    e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    e->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    e->dsc.header.w = (uint32_t)e->px;
    e->dsc.header.h = (uint32_t)e->px;
    e->dsc.header.stride = (uint32_t)e->px * 4;
    e->dsc.data_size = (uint32_t)(e->px * e->px * 4);
    e->dsc.data = px;
}

/* Free the entries of older generations that nothing shows any more. */
static void sweep(uint32_t gen)
{
    for (icon_t **pp = &s_icons; *pp;) {
        icon_t *e = *pp;
        if (e->gen == gen || e->refs > 0) {
            pp = &e->next;
            continue;
        }
        *pp = e->next;
        if (e->dsc.data) {
            lv_image_cache_drop(&e->dsc);
            free((void *)e->dsc.data);
        }
        free(e);
    }
}

static icon_t *lookup(const char *id, int32_t px)
{
    uint32_t gen = t5_pkg_generation();
    sweep(gen);
    for (icon_t *e = s_icons; e; e = e->next) {
        if (e->gen == gen && e->px == px && !strcmp(e->id, id)) return e;
    }
    icon_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    snprintf(e->id, sizeof e->id, "%s", id);
    e->px = px;
    e->gen = gen;
    load(e);
    e->next = s_icons;
    s_icons = e;
    return e;
}

static void image_deleted(lv_event_t *ev)
{
    icon_t *e = lv_event_get_user_data(ev);
    e->refs--;
}

lv_obj_t *t5_shell_app_icon_create(lv_obj_t *parent, const char *id, int32_t size_px)
{
    if (!parent || !id || !id[0] || size_px < ICON_MIN_PX || size_px > ICON_MAX_PX) return NULL;
    icon_t *e = lookup(id, size_px);
    if (!e || !e->dsc.data) return NULL;
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &e->dsc);
    lv_obj_set_size(img, size_px, size_px);
    lv_obj_set_clickable(img, false);
    e->refs++;
    lv_obj_add_event_cb(img, image_deleted, LV_EVENT_DELETE, e);
    return img;
}
