/* Album art (art.h). */
#include "art.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/ui.h>
#include "m3u.h"
#include "third_party/tjpgd/tjpgd.h"

/* In the app ABI; its header isn't one the SDK's ui.h includes. */
void lv_image_cache_drop(const void *src);

#define PENDING_MAX 48
#define JPEG_MAX (4 * 1024 * 1024)
#define WORK_BYTES 8192

typedef struct {
    bool used;
    uint32_t seq;                  /* newest first */
    uint32_t key;
    int px;
    char path[M3U_PATH_MAX];
    uint32_t off, len;
    art_done_fn fn;
    void *user;
} req_t;

static struct {
    t5_mutex_t mx;
    req_t q[PENDING_MAX];
    uint32_t seq;
    volatile bool run, done;
} s;

/* ---- decoding ----------------------------------------------------------------------- */
typedef struct {
    const uint8_t *src;
    size_t len, at;
    uint16_t *out;                 /* the scaled image, RGB565 */
    uint16_t w, h;
} jctx_t;

static size_t j_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jctx_t *c = jd->device;
    if (n > c->len - c->at) n = c->len - c->at;
    if (buf) memcpy(buf, c->src + c->at, n);
    c->at += n;
    return n;
}

static int j_out(JDEC *jd, void *bitmap, JRECT *r)
{
    jctx_t *c = jd->device;
    const uint8_t *p = bitmap;                         /* B, G, R */
    for (unsigned y = r->top; y <= r->bottom; y++) {
        for (unsigned x = r->left; x <= r->right; x++, p += 3) {
            if (x < c->w && y < c->h) {
                c->out[y * c->w + x] = (uint16_t)((p[2] & 0xf8) << 8 | (p[1] & 0xfc) << 3 | p[0] >> 3);
            }
        }
    }
    return 1;
}

static uint8_t *read_range(const char *path, uint32_t off, uint32_t len)
{
    if (!len || len > JPEG_MAX) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = malloc(len);
    bool ok = b && fseek(f, (long)off, SEEK_SET) == 0 && fread(b, 1, len, f) == len;
    fclose(f);
    if (!ok) {
        free(b);
        return NULL;
    }
    return b;
}

/* cover.jpg and friends next to the track */
static uint8_t *folder_cover(const char *track, uint32_t *len)
{
    static const char *const names[] = { "cover.jpg", "folder.jpg", "front.jpg", "album.jpg", "albumart.jpg", "cover.jpeg",
                                         "folder.jpeg" };
    const char *slash = strrchr(track, '/');
    if (!slash) return NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[M3U_PATH_MAX + 16];
        snprintf(p, sizeof p, "%.*s/%s", (int)(slash - track), track, names[i]);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fclose(f);
        if (n <= 0 || n > JPEG_MAX) continue;
        uint8_t *b = read_range(p, 0, (uint32_t)n);
        if (b) {
            *len = (uint32_t)n;
            return b;
        }
    }
    return NULL;
}

static lv_image_dsc_t *decode(const uint8_t *jpg, size_t len, int px)
{
    void *work = malloc(WORK_BYTES);
    if (!work) return NULL;
    JDEC jd;
    jctx_t c = { .src = jpg, .len = len };
    lv_image_dsc_t *img = NULL;
    if (jd_prepare(&jd, j_in, work, WORK_BYTES, &c) == JDR_OK) {
        uint8_t scale = 0;
        unsigned short_side = jd.width < jd.height ? jd.width : jd.height;
        while (scale < 3 && (short_side >> (scale + 1)) >= (unsigned)px) scale++;
        c.w = (uint16_t)((jd.width + (1u << scale) - 1) >> scale);
        c.h = (uint16_t)((jd.height + (1u << scale) - 1) >> scale);
        c.out = calloc((size_t)c.w * c.h, sizeof(uint16_t));
        if (c.out && jd_decomp(&jd, j_out, scale) == JDR_OK) {
            img = calloc(1, sizeof *img);
            uint16_t *pix = malloc((size_t)px * px * sizeof(uint16_t));
            if (img && pix) {
                unsigned m = c.w < c.h ? c.w : c.h;            /* the centred square */
                unsigned ox = (c.w - m) / 2, oy = (c.h - m) / 2;
                for (int y = 0; y < px; y++) {
                    const uint16_t *row = c.out + (size_t)(oy + (unsigned)y * m / (unsigned)px) * c.w + ox;
                    for (int x = 0; x < px; x++) pix[y * px + x] = row[(unsigned)x * m / (unsigned)px];
                }
                img->header.magic = LV_IMAGE_HEADER_MAGIC;
                img->header.cf = LV_COLOR_FORMAT_RGB565;
                img->header.w = (uint32_t)px;
                img->header.h = (uint32_t)px;
                img->header.stride = (uint32_t)px * 2;
                img->data_size = (uint32_t)px * px * 2;
                img->data = (const uint8_t *)pix;
            } else {
                free(img);
                free(pix);
                img = NULL;
            }
        }
        free(c.out);
    }
    free(work);
    return img;
}

/* ---- the task ------------------------------------------------------------------------ */
typedef struct {
    art_done_fn fn;
    void *user;
    uint32_t key;
    lv_image_dsc_t *img;
} done_t;

static void deliver(void *arg)
{
    done_t *d = arg;
    if (s.run) d->fn(d->key, d->img, d->user);
    else art_free(d->img);
    free(d);
}

static void task(void *arg)
{
    (void)arg;
    while (s.run) {
        req_t r = { 0 };
        t5_mutex_lock(s.mx);
        int best = -1;
        for (int i = 0; i < PENDING_MAX; i++) {
            if (s.q[i].used && (best < 0 || s.q[i].seq > s.q[best].seq)) best = i;
        }
        if (best >= 0) {
            r = s.q[best];
            s.q[best].used = false;
        }
        t5_mutex_unlock(s.mx);
        if (best < 0) {
            t5_delay_ms(30);
            continue;
        }
        uint32_t len = r.len;
        uint8_t *jpg = r.len ? read_range(r.path, r.off, r.len) : NULL;
        if (!jpg) jpg = folder_cover(r.path, &len);
        lv_image_dsc_t *img = jpg ? decode(jpg, len, r.px) : NULL;
        free(jpg);
        done_t *d = malloc(sizeof *d);
        if (!d) {
            free(img ? (void *)img->data : NULL);
            free(img);
            continue;
        }
        *d = (done_t){ r.fn, r.user, r.key, img };
        if (t5_ui_async(deliver, d) != T5_OK) {
            free(img ? (void *)img->data : NULL);
            free(img);
            free(d);
        }
    }
    s.done = true;
}

void art_start(void)
{
    s.mx = t5_mutex_create();
    s.run = true;
    if (!s.mx || t5_task_create("music.art", task, NULL, 16 * 1024, 3, T5_CORE_ANY, NULL) != T5_OK) s.run = false;
}

void art_stop(void)
{
    s.run = false;
    for (int i = 0; i < 100 && !s.done; i++) t5_delay_ms(10);
}

void art_request(uint32_t key, const char *track_path, uint32_t art_off, uint32_t art_len, int px, art_done_fn fn,
                 void *user)
{
    if (!s.run) {
        fn(key, NULL, user);
        return;
    }
    t5_mutex_lock(s.mx);
    int slot = -1, oldest = 0;
    for (int i = 0; i < PENDING_MAX; i++) {
        if (s.q[i].used && s.q[i].key == key && s.q[i].px == px && s.q[i].fn == fn) {
            slot = i;
            break;
        }
        if (slot < 0 && !s.q[i].used) slot = i;
        if (s.q[i].seq < s.q[oldest].seq) oldest = i;
    }
    if (slot < 0) slot = oldest;                           /* full: the oldest goes */
    req_t *r = &s.q[slot];
    r->used = true;
    r->seq = ++s.seq;
    r->key = key;
    r->px = px;
    snprintf(r->path, sizeof r->path, "%s", track_path);
    r->off = art_off;
    r->len = art_len;
    r->fn = fn;
    r->user = user;
    t5_mutex_unlock(s.mx);
}

void art_cancel(art_done_fn fn)
{
    if (!s.mx) return;
    t5_mutex_lock(s.mx);
    for (int i = 0; i < PENDING_MAX; i++) {
        if (s.q[i].fn == fn) s.q[i].used = false;
    }
    t5_mutex_unlock(s.mx);
}

void art_free(lv_image_dsc_t *img)
{
    if (!img) return;
    lv_image_cache_drop(img);
    free((void *)img->data);
    free(img);
}
