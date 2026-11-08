/* SPDX-License-Identifier: Apache-2.0
 * Full-screen photo viewer (.jpg), shared by Files and the Camera's
 * gallery, over the calling app's screen. Photos go through the hardware
 * JPEG decoder and are scaled down to fit (again after a rotation).
 */
#include <stdlib.h>
#include <string.h>
#include "t5/camera.h"
#include "sysapps_priv.h"

typedef struct {
    lv_obj_t *root, *img;
    void (*on_close)(void);
    lv_image_dsc_t photo;
    uint16_t *photo_buf;
} photo_t;

static photo_t *s_p;

bool sa_photo_is_open(void)
{
    return s_p != NULL;
}

static bool ends_with(const char *s, const char *suffix)
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
    return ends_with(path, ".jpg") || ends_with(path, ".jpeg");
}

static t5_err_t decode_jpeg(photo_t *p, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return T5_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *in = len > 0 ? malloc((size_t)len) : NULL;
    if (!in || fread(in, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(in);
        return T5_ERR_IO;
    }
    fclose(f);
    uint16_t w = 0, h = 0;
    t5_err_t e = t5_jpeg_size(in, (size_t)len, &w, &h);
    uint16_t *out = e == T5_OK ? malloc((size_t)w * h * 2) : NULL;
    if (e == T5_OK && !out) e = T5_ERR_NO_MEM;
    /* The camera service decodes on core 0 (the engine's interrupt). */
    if (e == T5_OK) e = t5_jpeg_decode(in, (size_t)len, out, (size_t)w * h * 2, &w, &h);
    free(in);
    if (e != T5_OK) {
        free(out);
        return e;
    }
    p->photo_buf = out;
    p->photo.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->photo.header.cf = LV_COLOR_FORMAT_RGB565;
    p->photo.header.w = w;
    p->photo.header.h = h;
    p->photo.header.stride = (uint32_t)w * 2;
    p->photo.data_size = (uint32_t)w * h * 2;
    p->photo.data = (const uint8_t *)out;
    return T5_OK;
}

static void fit(photo_t *p)
{
    int32_t vw = lv_obj_get_width(p->root), vh = lv_obj_get_height(p->root);
    if (vw <= 0 || vh <= 0 || !p->photo.header.w) return;
    uint32_t zx = (uint32_t)vw * 256 / p->photo.header.w, zy = (uint32_t)vh * 256 / p->photo.header.h;
    uint32_t z = zx < zy ? zx : zy;
    lv_image_set_scale(p->img, z < 256 ? z : 256);
}

static void resized(lv_event_t *e)
{
    fit(lv_event_get_user_data(e));
}

static void close_clicked(lv_event_t *e)
{
    (void)e;
    sa_photo_close();
}

bool sa_photo_open(lv_obj_t *screen, const char *path, void (*on_close)(void))
{
    sa_photo_close();
    if (!sa_photo_can_open(path)) return false;
    photo_t *p = calloc(1, sizeof *p);
    if (!p) return false;
    if (decode_jpeg(p, path) != T5_OK) {
        free(p);
        md_snackbar_show("Can't show this photo", NULL, NULL, NULL);
        return false;
    }
    p->on_close = on_close;
    p->root = lv_obj_create(screen);
    lv_obj_remove_style_all(p->root);
    lv_obj_set_ignore_layout(p->root, true);
    lv_obj_set_size(p->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(p->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(p->root, false);
    lv_obj_set_clickable(p->root, true);              /* nothing underneath gets the taps */
    p->img = lv_image_create(p->root);
    lv_image_set_src(p->img, &p->photo);
    lv_obj_center(p->img);

    lv_obj_t *b = lv_button_create(p->root);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, T5_DP(48), T5_DP(48));
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_t *l = md_icon_create(b, MD_ICON_CLOSE, MD_ICON_DP);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, close_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -T5_DP(16), T5_DP(16));

    lv_obj_update_layout(p->root);
    fit(p);
    lv_obj_add_event_cb(p->root, resized, LV_EVENT_SIZE_CHANGED, p);
    s_p = p;
    return true;
}

void sa_photo_close(void)
{
    photo_t *p = s_p;
    if (!p) return;
    s_p = NULL;
    lv_obj_delete(p->root);
    free(p->photo_buf);
    void (*cb)(void) = p->on_close;
    free(p);
    if (cb) cb();
}
