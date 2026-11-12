/* SPDX-License-Identifier: Apache-2.0
 * Camera service (plan 7.4-7.6). One capture task owns /dev/video0, in
 * one of two sensor modes chosen at open:
 *   t5_camera_open        1280x720 RAW8, a window in the middle of the
 *                         sensor: video (and what apps get)
 *   t5_camera_open_photo  1600x900 RAW10, the full sensor width: photos
 *                         and the Camera app's viewfinder; it can't record
 *                         (the encoder's reference buffer wouldn't fit)
 *
 * Frames come from the ISP as YUV420 (t5_camera_open, for H.264) or
 * RGB565 (the system Camera's modes: the PPA's YUV420 input is slow, ~80 ms
 * a 1600x900 frame, and photos and MJPEG want RGB565 anyway).
 *
 *   DQBUF
 *     -> preview: the frame itself is posted to a video plane
 *        (t5_core/plane.h) with its crop, scale and turn; the display
 *        pipeline scales, converts and turns it into the framebuffer in one
 *        PPA pass. The capture task keeps the buffer until the plane lets
 *        it go (a newer frame was taken), so at most two are lent. Skipped
 *        while a posted frame waits, so the preview rate adapts to the UI.
 *        The PPA's time goes with the pixels it reads (~80 ms for a
 *        1600x900 RGB565 frame, scaled or turned or not), so a preview
 *        under ~60% of the frame's height reads every other row.
 *     -> recording, .avi: hardware JPEG (from the frame itself in RGB565
 *        mode, else after a PPA pass to RGB565), one AVI chunk per sensor
 *        frame into a 4 MB PSRAM ring that a writer task empties to SD (a
 *        frame the ring can't take, or one the driver dropped, becomes an
 *        empty chunk: a repeat).
 *     -> recording, H.264: hardware H.264, NAL units into a 2 MB ring the
 *        same way (a full ring drops the frame and counts it).
 *     -> still: PPA to RGB565 at full size, hardware JPEG, written to SD.
 *   QBUF
 *
 * Memory: four capture buffers (YUV420: 5.5 MB at 720p; RGB565: 7.4 MB at
 * 720p, 11.5 MB at 1600x900) and the write ring; an MJPEG recording adds
 * 0.9 MB of encoder output (2.7 MB from YUV420). All of it is freed on
 * close.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_cam_sensor.h"
#include "esp_sccb_i2c.h"
#include "sc202cs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "t5_bsp/xpin.h"
#include "t5_camera/camera.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/pins.h"
#include "t5_power/power.h"
#include "avi.h"
#include "h264_ram.h"

static const char *TAG = "t5.camera";

#define NBUF 4
#define LENT_MAX 2                   /* frames the preview's plane may hold */
#define RING_BYTES (2 * 1024 * 1024)
#define RING_BYTES_MJPEG (4 * 1024 * 1024)
/* Preview frames at most this often (µs): every frame, or a third while
 * recording (the encoders need the memory bandwidth). */
#define PV_PERIOD_US 30000
#define PV_PERIOD_REC_US 100000
#define CAM_MAGIC 0x43414d31u
#define ALIGN 128
/* Image rotation that shows the sensor upright on a landscape screen, found
 * on the bench. Other screen rotations turn it back the other way (view_deg). */
#define CAM_LANDSCAPE_ROT 0
/* The sensor adds a black level (0x41 of 1023 in its register tables) for
 * the ISP to subtract, but esp_video only uses the ISP's black-level block
 * on P4 revision 3: on revision 1 the pedestal stayed in, as a grey haze
 * over every shadow that white balance then tinted. The sensor's own target
 * is lowered instead (0x3907/0x3908, written after each stream start). */
#define CAM_BLC_TARGET 0x00

struct t5_camera {
    uint32_t magic;
    int fd;
    int w, h;                    /* frame size: the mode it was opened in */
    bool rgb;                    /* RGB565 frames, else YUV420 */
    uint8_t *buf[NBUF];
    uint32_t buflen[NBUF];
    TaskHandle_t task;           /* stays set once created: stop_task waits on exited */
    volatile bool running;       /* cleared by the capture task as it ends */
    SemaphoreHandle_t exited;
    uint32_t serial;             /* which open this is (preview_swap's handle) */
    volatile bool stop;
    bool mirror;
    /* preview: our reference to the plane on screen (under s_mx) */
    t5_plane_t *plane;
    int pw, ph, pk;              /* its size, and the frame's scale (k/16) */
    /* still */
    SemaphoreHandle_t photo_done, ready;
    t5_err_t setup_err;
    /* Work that must run on core 0 (see cam_setup), done by the capture task. */
    t5_err_t (*call_fn)(struct t5_camera *c, void *arg);   /* atomic: see cam_call() */
    void *call_arg;
    t5_err_t call_res;
    SemaphoreHandle_t call_done;
    volatile bool photo_req;
    char photo_path[128];
    int photo_q;
    t5_err_t photo_err;
    /* t5_camera_snapshot */
    volatile bool snap_req;
    uint16_t *snap_out;
    int snap_w, snap_h;
    t5_err_t snap_err;
    SemaphoreHandle_t snap_done;
    /* frames lent to the preview's plane (capture task only) */
    struct v4l2_buffer lent[LENT_MAX];
    uint32_t lent_seq[LENT_MAX];
    t5_plane_t *lent_plane[LENT_MAX];
    int nlent;
    /* recording */
    esp_h264_enc_handle_t enc;
    esp_h264_enc_param_hw_handle_t enc_param;
    uint32_t kbps_target, kbps_now;  /* adaptive: see adapt_bitrate() */
    int64_t kbps_changed_us;
    uint8_t *enc_out;
    uint32_t enc_out_len;
    StreamBufferHandle_t ring;   /* owned by the writer once recording stops */
    volatile bool recording;
    int64_t rec_t0;
    /* MJPEG recording (.avi) */
    bool mjpeg;
    uint16_t *mj_rgb;            /* the frame as RGB565, the encoder's input */
    uint8_t *mj_out;
    size_t mj_out_cap;
    int mj_q, mj_q_max;          /* JPEG quality now, and the most it goes back up to */
    int64_t mj_q_changed_us;
    uint32_t mj_seq;             /* the sensor frame the last chunk stands for */
    bool mj_have_seq;
    int64_t mj_t_first, mj_t_last;
};

static t5_camera_t *s_cam;
static t5_mutex_t s_mx, s_call_mx;
static ppa_client_handle_t s_ppa;   /* stills, MJPEG from YUV420, snapshots: blocking */
static jpeg_encoder_handle_t s_jpeg;
static bool s_video_inited;
static t5_camera_stats_t s_st;
static t5_camera_host_t s_host;

void t5_camera_set_host(const t5_camera_host_t *host)
{
    s_host = host ? *host : (t5_camera_host_t){ 0 };
}

static bool on_ui_task(void)
{
    return s_host.on_ui_task && s_host.on_ui_task();
}

static void encoder_ram_lend(void)
{
    size_t len = 0;
    void *b = s_host.lend_ram ? s_host.lend_ram(&len) : NULL;
    h264_ram_give(b, len);
}

static void encoder_ram_reclaim(void)
{
    void *b = h264_ram_take();
    if (s_host.reclaim_ram) s_host.reclaim_ram(b);
    else heap_caps_free(b);
}

static ppa_srm_rotation_angle_t ppa_angle(int deg)
{
    switch (((deg % 360) + 360) % 360) {
    case 90: return PPA_SRM_ROTATION_ANGLE_90;
    case 180: return PPA_SRM_ROTATION_ANGLE_180;
    case 270: return PPA_SRM_ROTATION_ANGLE_270;
    default: return PPA_SRM_ROTATION_ANGLE_0;
    }
}

/* Clockwise turn that shows the sensor upright on the current screen, as
 * a PPA (counter-clockwise) angle. The sensor is fixed to the board, so as
 * the screen turns clockwise the image must turn back the other way. */
static int view_deg(void)
{
    int rot = s_host.rotation_deg ? s_host.rotation_deg() : 0;
    return ((CAM_LANDSCAPE_ROT + 90 - rot) % 360 + 360) % 360;
}

/* The part of the frame converted at scale k/16. Below full size it is
 * trimmed to multiples of 32 (a few edge rows at most), so the scaled size
 * comes out whole and even for any k: the PPA scales in 1/16 steps. */
static void crop_dims(const t5_camera_t *c, int k, int *iw, int *ih)
{
    *iw = k % 16 ? c->w / 32 * 32 : c->w;
    *ih = k % 16 ? c->h / 32 * 32 : c->h;
}

/* The size of the frame at k/16, turned by deg. */
static void scaled_dims(const t5_camera_t *c, int k, int deg, int *w, int *h)
{
    int iw, ih;
    crop_dims(c, k, &iw, &ih);
    bool sideways = (deg % 180) != 0;
    *w = (sideways ? ih : iw) * k / 16;
    *h = (sideways ? iw : ih) * k / 16;
}

/* A frame -> RGB565 out (ow x oh) at scale k/16, rotated by deg and
 * centred. Blocking. */
static t5_err_t convert(const t5_camera_t *c, const uint8_t *frame, uint16_t *out, int ow, int oh, int k, int deg,
                        bool mirror)
{
    int iw, ih, bw, bh;
    crop_dims(c, k, &iw, &ih);
    scaled_dims(c, k, deg, &bw, &bh);
    if (k < 1 || bw > ow || bh > oh) return T5_ERR_INVALID_ARG;
    float s = (float)k / 16.0f;
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = frame, .pic_w = (uint32_t)c->w, .pic_h = (uint32_t)c->h,
            .block_w = (uint32_t)iw, .block_h = (uint32_t)ih,
            .block_offset_x = (uint32_t)((c->w - iw) / 2 & ~1),
            .block_offset_y = (uint32_t)((c->h - ih) / 2 & ~1),
            .srm_cm = c->rgb ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_YUV420,
        },
        .out = {
            .buffer = out, .buffer_size = (uint32_t)(ow * oh * 2), .pic_w = (uint32_t)ow, .pic_h = (uint32_t)oh,
            .block_offset_x = (uint32_t)((ow - bw) / 2), .block_offset_y = (uint32_t)((oh - bh) / 2),
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = ppa_angle(deg),
        .scale_x = s, .scale_y = s,
        .mirror_x = mirror,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK ? T5_OK : T5_ERR_IO;
}


/* The preview: a frame lent to the plane, described for the display
 * pipeline (the same crop, scale and turn convert() would use). */
static void preview_src(const t5_camera_t *c, const uint8_t *frame, int k, t5_plane_src_t *s)
{
    int iw, ih;
    crop_dims(c, k, &iw, &ih);
    *s = (t5_plane_src_t){
        .buf = frame, .fmt = c->rgb ? T5_PLANE_RGB565 : T5_PLANE_YUV420, .mirror = c->mirror,
        .rot = (uint16_t)view_deg(), .pic_w = (uint16_t)c->w, .pic_h = (uint16_t)c->h,
        .x = (uint16_t)((c->w - iw) / 2 & ~1), .y = (uint16_t)((c->h - ih) / 2 & ~1),
        .w = (uint16_t)iw, .h = (uint16_t)ih, .k = (uint16_t)k,
    };
    /* Every other row, as a picture twice as wide and half as tall: half
     * the reading, and the rows are scaled up by 2k/16 <= 1.25 instead. */
    if (c->rgb && k <= 10 && ih % 4 == 0) {
        s->pic_w = (uint16_t)(c->w * 2);
        s->pic_h = (uint16_t)(c->h / 2);
        s->y /= 2;
        s->h = (uint16_t)(ih / 2);
        s->ky = (uint16_t)(k * 2);
    }
}

/* Lent frames the plane has let go of go back to the driver. */
static void return_lent(t5_camera_t *c, bool all)
{
    for (int i = 0; i < c->nlent;) {
        if (all || t5_plane_released(c->lent_plane[i], c->lent_seq[i])) {
            ioctl(c->fd, VIDIOC_QBUF, &c->lent[i]);
            t5_plane_release(c->lent_plane[i]);
            c->nlent--;
            c->lent[i] = c->lent[c->nlent];
            c->lent_seq[i] = c->lent_seq[c->nlent];
            c->lent_plane[i] = c->lent_plane[c->nlent];
        } else {
            i++;
        }
    }
}

/* Post the frame to the preview if it wants one; true if it was lent. */
static bool preview_post(t5_camera_t *c, const struct v4l2_buffer *b, const uint8_t *frame)
{
    if (c->nlent >= LENT_MAX) return false;
    t5_mutex_lock(s_mx);
    t5_plane_t *p = c->plane;
    uint32_t seq = 0;
    if (p && t5_plane_ready(p)) {
        t5_plane_src_t src;
        preview_src(c, frame, c->pk, &src);
        seq = t5_plane_post(p, &src);
    }
    if (seq) {
        t5_plane_hold(p);
        c->lent[c->nlent] = *b;
        c->lent_seq[c->nlent] = seq;
        c->lent_plane[c->nlent] = p;
        c->nlent++;
    }
    t5_mutex_unlock(s_mx);
    return seq != 0;
}

/* t5_camera_snapshot's frame: a quarter-size copy, then nearest-neighbour
 * to the size asked for. */
static t5_err_t snapshot_frame(t5_camera_t *c, const uint8_t *frame)
{
    int deg = view_deg(), k = 4, tw, th;
    scaled_dims(c, k, deg, &tw, &th);
    uint16_t *tmp = heap_caps_aligned_calloc(ALIGN, 1, ((size_t)tw * th * 2 + ALIGN - 1) & ~(size_t)(ALIGN - 1),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) return T5_ERR_NO_MEM;
    t5_err_t e = convert(c, frame, tmp, tw, th, k, deg, c->mirror);
    if (e == T5_OK) {
        for (int y = 0; y < c->snap_h; y++) {
            for (int x = 0; x < c->snap_w; x++)
                c->snap_out[y * c->snap_w + x] = tmp[(y * th / c->snap_h) * tw + x * tw / c->snap_w];
        }
    }
    heap_caps_free(tmp);
    return e;
}

/* Run a cam_call() waiting for this task. Claimed atomically, so a caller
 * that timed out can take it back if it hasn't started. */
static void run_call(t5_camera_t *c)
{
    t5_err_t (*fn)(t5_camera_t *, void *) = __atomic_exchange_n(&c->call_fn, NULL, __ATOMIC_ACQ_REL);
    if (!fn) return;
    c->call_res = fn(c, c->call_arg);
    xSemaphoreGive(c->call_done);
}

/* The capture task never waits for the UI: it posts a frame to the plane
 * and the UI takes it between its frames.
 * (Posting each frame needed the UI lock, so capture ran at the UI's frame
 * rate, and recording with the preview on got ~8 fps; with the lock held
 * by a caller waiting on capture, it deadlocked.) The screen holds its own
 * reference to the plane, so either side can let go first. */
lv_obj_t *t5_camera_preview_create(t5_camera_t *c, lv_obj_t *parent)
{
    if (!c || c->magic != CAM_MAGIC || !parent || !s_host.plane_show) return NULL;
    lv_obj_update_layout(parent);
    int pw = lv_obj_get_content_width(parent), ph = lv_obj_get_content_height(parent);
    /* The plane is exactly the scaled frame, centred in parent: the
     * largest 1/16 step that fits. */
    int deg = view_deg(), k = 16, w = 0, h = 0;
    for (; k >= 1; k--) {
        scaled_dims(c, k, deg, &w, &h);
        if (w <= pw && h <= ph) break;
    }
    if (k < 1 || w < 16 || h < 16) return NULL;
    t5_plane_t *p = t5_plane_new(w, h);
    if (!p) return NULL;
    lv_obj_t *img = s_host.plane_show(parent, p);
    if (!img) {
        t5_plane_release(p);
        return NULL;
    }
    lv_obj_center(img);
    t5_mutex_lock(s_mx);
    t5_plane_t *old = c->plane;
    c->plane = p;
    c->pw = w;
    c->ph = h;
    c->pk = k;
    t5_mutex_unlock(s_mx);
    t5_plane_release(old);       /* a conversion into it holds its own */
    return img;
}

void t5_camera_set_mirror(t5_camera_t *c, bool mirror)
{
    if (c && c->magic == CAM_MAGIC) c->mirror = mirror;
}

/* The writer owns the ring and the file: on stop it drains what is left
 * (a slow card can be seconds behind) and then frees both, so stopping never
 * waits on the card and never frees them under it. A new recording waits
 * for the previous writer to finish (s_writers). */
typedef struct {
    StreamBufferHandle_t ring;
    FILE *file;
    volatile bool stop;
    /* AVI: the capture task fills these while recording and stops touching
     * them before it sets stop; the writer then finishes the file. */
    bool avi;
    avi_info_t info;
    avi_idx_t *idx;
    uint32_t nidx, idx_cap;
} rec_job_t;

/* Finish an AVI once everything is on the card: the index, then the
 * header again with the real numbers. */
static void avi_finish(rec_job_t *j)
{
    uint8_t buf[16];
    bool ok = true;
    avi_idx1_header(buf, j->nidx);
    ok = fwrite(buf, 1, 8, j->file) == 8;
    for (uint32_t i = 0; ok && i < j->nidx; i++) {
        avi_idx1_entry(buf, &j->idx[i]);
        ok = fwrite(buf, 1, 16, j->file) == 16;
    }
    j->info.idx_entries = ok ? j->nidx : 0;
    uint8_t hdr[AVI_HDR_LEN];
    avi_header(hdr, &j->info);
    if (fseek(j->file, 0, SEEK_SET) != 0 || fwrite(hdr, 1, sizeof hdr, j->file) != sizeof hdr) ok = false;
    if (!ok) T5_LOGE(TAG, "recording: couldn't finish the AVI (index or header)");
    else T5_LOGI(TAG, "AVI: %u frames, %u us each", (unsigned)j->info.frames, (unsigned)j->info.us_per_frame);
}

static volatile int s_writers;
static rec_job_t *s_job;        /* the current recording's; the capture task's */

static void writer_task(void *arg)
{
    rec_job_t *j = arg;
    /* Cache-line aligned and DMA-capable, so the SD driver can DMA straight
     * from it instead of bouncing each sector through internal RAM. */
    uint8_t *chunk = heap_caps_aligned_alloc(64, 64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    int64_t t0 = 0;
    bool failed = !chunk;
    while (chunk) {
        size_t n = xStreamBufferReceive(j->ring, chunk, 64 * 1024, pdMS_TO_TICKS(50));
        if (n && fwrite(chunk, 1, n, j->file) != n) {
            T5_LOGE(TAG, "recording write failed: dropping the rest");
            failed = true;
            break;
        }
        if (!__atomic_load_n(&j->stop, __ATOMIC_ACQUIRE)) continue;
        if (!t0) t0 = esp_timer_get_time();
        if (!n && !xStreamBufferBytesAvailable(j->ring)) break;
    }
    if (failed) {
        /* The capture task may still be filling the ring: wait for stop. */
        while (!__atomic_load_n(&j->stop, __ATOMIC_ACQUIRE)) {
            uint8_t sink[256];
            xStreamBufferReceive(j->ring, sink, sizeof sink, pdMS_TO_TICKS(50));
        }
    }
    if (j->avi && !failed) avi_finish(j);
    if (t0) T5_LOGI(TAG, "recording flushed %d ms after stop", (int)((esp_timer_get_time() - t0) / 1000));
    heap_caps_free(chunk);
    fclose(j->file);
    vStreamBufferDeleteWithCaps(j->ring);
    heap_caps_free(j->idx);
    free(j);
    __atomic_sub_fetch(&s_writers, 1, __ATOMIC_SEQ_CST);
    vTaskDeleteWithCaps(NULL);
}

/* Keep the stream within what the card can write. A slow card (Unit A's
 * writes ~300 KB/s, FAT32 with 512-byte clusters) fell behind a busy scene
 * at 6 Mbit/s: the 2 MB ring filled and 12% of a 10 minute recording was
 * dropped (2026-11-07). Step the bitrate down a quarter while the ring is
 * over 40% full, back up while it stays under 10%. */
#define KBPS_MIN 1500
#define KBPS_DEFAULT 4000              /* t5_camera_record_start's kbps 0 */
static void adapt_bitrate(t5_camera_t *c, size_t queued)
{
    if (!c->enc_param) return;
    int64_t now = esp_timer_get_time();
    uint32_t pct = (uint32_t)(queued * 100 / RING_BYTES), k = c->kbps_now;
    if (pct > 40 && now - c->kbps_changed_us > 1000000 && k > KBPS_MIN) k = k * 3 / 4 < KBPS_MIN ? KBPS_MIN : k * 3 / 4;
    else if (pct < 10 && now - c->kbps_changed_us > 4000000 && k < c->kbps_target)
        k = k + k / 4 > c->kbps_target ? c->kbps_target : k + k / 4;
    if (k == c->kbps_now) return;
    if (esp_h264_enc_set_bitrate(&c->enc_param->base, k * 1000) != ESP_H264_ERR_OK) return;
    T5_LOGI(TAG, "recording: %u -> %u kbit/s (write buffer %u%%)", (unsigned)c->kbps_now, (unsigned)k, (unsigned)pct);
    c->kbps_now = k;
    c->kbps_changed_us = now;
}

static void encode(t5_camera_t *c, uint8_t *frame)
{
    esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = frame, .len = (uint32_t)(c->w * c->h * 3 / 2) } };
    esp_h264_enc_out_frame_t out = { .raw_data = { .buffer = c->enc_out, .len = c->enc_out_len } };
    int64_t t0 = esp_timer_get_time();
    if (esp_h264_enc_process(c->enc, &in, &out) != ESP_H264_ERR_OK) {
        s_st.rec_dropped++;
        return;
    }
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_st.encode_us_max) s_st.encode_us_max = us;
    if (xStreamBufferSpacesAvailable(c->ring) < out.length) {
        s_st.rec_dropped++;
        return;
    }
    xStreamBufferSend(c->ring, out.raw_data.buffer, out.length, 0);
    size_t q = xStreamBufferBytesAvailable(c->ring);
    if (q > s_st.write_buffer_peak) s_st.write_buffer_peak = (uint32_t)q;
    adapt_bitrate(c, q);
    s_st.rec_frames++;
    s_st.rec_bytes += out.length;
    s_st.rec_ms = (uint32_t)((esp_timer_get_time() - c->rec_t0) / 1000);
}

/* ---- Motion JPEG into AVI ------------------------------------------------------
 * One chunk per sensor frame, so the file's frame rate is the sensor's. A
 * frame the driver dropped, or one the ring has no room for, is stored as
 * an empty chunk, which players show as a repeat of the frame before. */
#define MJ_Q_MIN 30
#define MJ_Q_DEFAULT 70
#define MJ_RESERVE (192 * 1024)      /* room wanted before encoding a frame */

static bool mj_push(t5_camera_t *c, const uint8_t *data, uint32_t len)
{
    rec_job_t *j = s_job;
    uint32_t bytes = avi_chunk_bytes(len);
    if (j->info.movi_bytes + bytes > AVI_MAX_MOVI) return false;
    if (j->nidx == j->idx_cap) {
        uint32_t cap = j->idx_cap + 4096;
        avi_idx_t *n = heap_caps_realloc(j->idx, cap * sizeof *n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!n) return false;
        j->idx = n;
        j->idx_cap = cap;
    }
    if (xStreamBufferSpacesAvailable(c->ring) < bytes) return false;
    uint8_t hdr[AVI_CHUNK_HDR];
    avi_chunk_header(hdr, len);
    xStreamBufferSend(c->ring, hdr, sizeof hdr, 0);
    if (len) xStreamBufferSend(c->ring, data, len, 0);
    if (len & 1) {
        uint8_t pad = 0;
        xStreamBufferSend(c->ring, &pad, 1, 0);
    }
    j->idx[j->nidx++] = (avi_idx_t){ 4 + j->info.movi_bytes, len };
    j->info.movi_bytes += bytes;
    j->info.frames++;
    if (len > j->info.max_chunk) j->info.max_chunk = len;
    return true;
}

/* Like adapt_bitrate: quality down 5 while the ring is over 40% full, back
 * up while it stays under 10%. */
static void mj_adapt(t5_camera_t *c, size_t queued)
{
    int64_t now = esp_timer_get_time();
    uint32_t pct = (uint32_t)(queued * 100 / RING_BYTES_MJPEG);
    int q = c->mj_q;
    if (pct > 40 && now - c->mj_q_changed_us > 1000000 && q > MJ_Q_MIN) q = q - 5 < MJ_Q_MIN ? MJ_Q_MIN : q - 5;
    else if (pct < 10 && now - c->mj_q_changed_us > 4000000 && q < c->mj_q_max) q = q + 5 > c->mj_q_max ? c->mj_q_max : q + 5;
    if (q == c->mj_q) return;
    T5_LOGI(TAG, "recording: JPEG quality %d -> %d (write buffer %u%%)", c->mj_q, q, (unsigned)pct);
    c->mj_q = q;
    c->mj_q_changed_us = now;
}

static void encode_mjpeg(t5_camera_t *c, const uint8_t *frame, uint32_t seq)
{
    int64_t now = esp_timer_get_time();
    /* Frames the driver dropped stand in as repeats (a long gap is a stall,
     * not frames to account for). */
    if (c->mj_have_seq && seq > c->mj_seq + 1 && seq - c->mj_seq < 30) {
        for (uint32_t s = c->mj_seq + 1; s < seq; s++) {
            if (mj_push(c, NULL, 0)) s_st.rec_dropped++;
        }
    }
    c->mj_seq = seq;
    c->mj_have_seq = true;
    if (!c->mj_t_first) c->mj_t_first = now;
    c->mj_t_last = now;
    uint32_t want = s_job->info.max_chunk > MJ_RESERVE ? s_job->info.max_chunk : MJ_RESERVE;
    bool ok = false;
    if (xStreamBufferSpacesAvailable(c->ring) >= avi_chunk_bytes(want)) {
        uint32_t len = 0;
        const uint8_t *rgb = c->rgb ? frame : (const uint8_t *)c->mj_rgb;
        bool enc = c->rgb || convert(c, frame, c->mj_rgb, c->w, c->h, 16, 0, false) == T5_OK;
        if (enc) {
            jpeg_encode_cfg_t jc = {
                .height = (uint32_t)c->h, .width = (uint32_t)c->w, .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                .sub_sample = JPEG_DOWN_SAMPLING_YUV420, .image_quality = (uint32_t)c->mj_q,
            };
            enc = jpeg_encoder_process(s_jpeg, &jc, rgb, (uint32_t)(c->w * c->h * 2), c->mj_out,
                                       (uint32_t)c->mj_out_cap, &len) == ESP_OK;
        }
        uint32_t us = (uint32_t)(esp_timer_get_time() - now);
        if (us > s_st.encode_us_max) s_st.encode_us_max = us;
        ok = enc && mj_push(c, c->mj_out, len);
        if (ok) {
            s_st.rec_frames++;
            s_st.rec_bytes += len;
        }
    }
    if (!ok) {
        mj_push(c, NULL, 0);
        s_st.rec_dropped++;
    }
    size_t q = RING_BYTES_MJPEG - xStreamBufferSpacesAvailable(c->ring);
    if (q > s_st.write_buffer_peak) s_st.write_buffer_peak = (uint32_t)q;
    mj_adapt(c, q);
    s_st.rec_ms = (uint32_t)((esp_timer_get_time() - c->rec_t0) / 1000);
}

/* Made on the capture task (core 0), for its interrupt. */
static t5_err_t jpeg_encoder_ready(void)
{
    if (s_jpeg) return T5_OK;
    jpeg_encode_engine_cfg_t ec = { .timeout_ms = 2000 };
    return jpeg_new_encoder_engine(&ec, &s_jpeg) == ESP_OK ? T5_OK : T5_ERR_NOT_SUPPORTED;
}

static t5_err_t take_photo(t5_camera_t *c, const uint8_t *frame)
{
    T5_RETURN_ON_ERR(jpeg_encoder_ready());
    size_t rgb_cap = 0, jpg_cap = 0;
    jpeg_encode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    /* Upright as the screen is held: a portrait screen takes a portrait
     * photo. An RGB565 frame already upright goes to the encoder as it is. */
    int deg = view_deg();
    bool as_is = c->rgb && deg == 0 && !c->mirror;
    uint16_t *rgb = as_is ? NULL : jpeg_alloc_encoder_mem((size_t)c->w * c->h * 2, &in_cfg, &rgb_cap);
    uint8_t *jpg = jpeg_alloc_encoder_mem((size_t)c->w * c->h, &out_cfg, &jpg_cap);
    t5_err_t e = ((rgb || as_is) && jpg) ? T5_OK : T5_ERR_NO_MEM;
    bool sideways = deg % 180 != 0;
    uint32_t pw = (uint32_t)(sideways ? c->h : c->w), ph = (uint32_t)(sideways ? c->w : c->h);
    if (e == T5_OK && !as_is) e = convert(c, frame, rgb, (int)pw, (int)ph, 16, deg, c->mirror);
    uint32_t len = 0;
    if (e == T5_OK) {
        jpeg_encode_cfg_t jc = {
            .height = ph, .width = pw, .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420, .image_quality = (uint32_t)c->photo_q,
        };
        if (jpeg_encoder_process(s_jpeg, &jc, as_is ? frame : (const uint8_t *)rgb, (uint32_t)(c->w * c->h * 2), jpg,
                                 (uint32_t)jpg_cap, &len) != ESP_OK) {
            e = T5_ERR_IO;
        }
    }
    if (e == T5_OK) {
        FILE *f = fopen(c->photo_path, "wb");
        if (!f) e = T5_ERR_IO;
        else {
            if (fwrite(jpg, 1, len, f) != len) e = T5_ERR_IO;
            fclose(f);
        }
    }
    free(rgb);
    free(jpg);
    if (e == T5_OK) s_st.photos++;
    return e;
}

static t5_err_t cam_setup(t5_camera_t *c);

/* c may be freed as soon as exited is given. */
static void capture_exit(t5_camera_t *c)
{
    SemaphoreHandle_t exited = c->exited;
    c->running = false;
    xSemaphoreGive(exited);
    vTaskDelete(NULL);
}

static void capture_task(void *arg)
{
    t5_camera_t *c = arg;
    c->setup_err = cam_setup(c);
    xSemaphoreGive(c->ready);
    if (c->setup_err != T5_OK) capture_exit(c);
    uint32_t last_seq = 0, sec_frames = 0, sec_pv = 0;
    int64_t pv_last = 0;
    bool have_seq = false;
    int64_t sec_t0 = esp_timer_get_time();
    while (!c->stop) {
        return_lent(c, false);
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(c->fd, VIDIOC_DQBUF, &b) != 0) {
            run_call(c);            /* no frame yet: don't keep a caller waiting */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        bool lent = false;
        if (b.flags & V4L2_BUF_FLAG_DONE) {
            s_st.frames++;
            sec_frames++;
            if (have_seq && b.sequence > last_seq + 1) s_st.dropped += b.sequence - last_seq - 1;
            last_seq = b.sequence;
            have_seq = true;
            uint8_t *frame = c->buf[b.index];
            if (c->recording && c->mjpeg) encode_mjpeg(c, frame, b.sequence);
            else if (c->recording && c->enc) encode(c, frame);
            if (c->photo_req) {
                c->photo_err = take_photo(c, frame);
                c->photo_req = false;
                xSemaphoreGive(c->photo_done);
            }
            if (c->snap_req) {
                c->snap_err = snapshot_frame(c, frame);
                c->snap_req = false;
                xSemaphoreGive(c->snap_done);
            }
            /* The preview is skipped while the screen hasn't taken the last
             * frame, so it adapts to the UI. While recording, fewer: the
             * encoders need the memory bandwidth (with H.264 the UI also
             * draws in PSRAM then: its bands are lent). */
            int64_t pv_now = esp_timer_get_time();
            if (pv_now - pv_last >= (c->recording ? PV_PERIOD_REC_US : PV_PERIOD_US)) {
                lent = preview_post(c, &b, frame);
                if (lent) {
                    pv_last = pv_now;
                    s_st.preview_frames++;
                    sec_pv++;
                }
            }
        }
        if (!lent) ioctl(c->fd, VIDIOC_QBUF, &b);
        run_call(c);
        int64_t now = esp_timer_get_time();
        if (now - sec_t0 >= 1000000) {
            s_st.fps = sec_frames;
            s_st.preview_fps = sec_pv;
            sec_frames = sec_pv = 0;
            sec_t0 = now;
        }
    }
    /* Frames still lent stay with the plane: whoever closes ends it (once we
     * are gone: t5_plane_end takes the UI lock, which a closer on the UI
     * task holds while it waits for us), and streaming off takes them back. */
    capture_exit(c);
}

/* The two sensor modes we use. esp_video only knows the Kconfig default,
 * and VIDIOC_S_SENSOR_FMT wants the driver's own descriptor, so they come
 * from a short-lived second handle on the sensor (detect reads its ID and
 * allocates; deleting it only frees). */
static const esp_cam_sensor_format_t *s_fmt_photo, *s_fmt_video;

static void find_formats(void)
{
    sccb_i2c_config_t sc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = T5_I2C_ADDR_SC2356, .scl_speed_hz = T5_SYS_I2C_HZ };
    esp_sccb_io_handle_t io = NULL;
    if (sccb_new_i2c_io((i2c_master_bus_handle_t)t5_i2c_native_bus(), &sc, &io) != ESP_OK) return;
    esp_cam_sensor_config_t cfg = {
        .sccb_handle = io, .reset_pin = -1, .pwdn_pin = -1, .xclk_pin = -1, .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };
    esp_cam_sensor_device_t *dev = sc202cs_detect(&cfg);
    esp_cam_sensor_format_array_t fa = { 0 };
    if (dev && esp_cam_sensor_query_format(dev, &fa) == ESP_OK) {
        for (uint32_t i = 0; i < fa.count; i++) {
            const esp_cam_sensor_format_t *f = &fa.format_array[i];
            if (f->width == T5_CAMERA_PHOTO_W && f->height == T5_CAMERA_PHOTO_H) s_fmt_photo = f;
            if (f->width == T5_CAMERA_W && f->height == T5_CAMERA_H) s_fmt_video = f;
        }
    }
    if (dev) esp_cam_sensor_del_dev(dev);
    esp_sccb_del_i2c_io(io);
    if (!s_fmt_photo || !s_fmt_video) T5_LOGE(TAG, "sensor modes not found (photo %p, video %p)", s_fmt_photo, s_fmt_video);
}

/* esp_video_deinit() leaves the CSI DMA interrupt allocated, so a second
 * esp_video_init() fails ("alloc interrupt failed"). esp_video stays up after
 * the first open; closing stops the stream, which puts the sensor in
 * standby. Until the first open the sensor is held in reset. */
static t5_err_t video_up(void)
{
    if (s_video_inited) return T5_OK;
    T5_RETURN_ON_ERR(t5_xpin_output(T5_XP_CAM_RST, true));     /* out of reset */
    t5_delay_ms(30);
    static esp_video_init_csi_config_t csi;
    memset(&csi, 0, sizeof csi);
    csi.sccb_config.init_sccb = false;
    csi.sccb_config.i2c_handle = (i2c_master_bus_handle_t)t5_i2c_native_bus();
    csi.sccb_config.freq = T5_SYS_I2C_HZ;
    csi.reset_pin = -1;          /* CAM_RST is on the expander, handled above */
    csi.pwdn_pin = -1;
    esp_video_init_config_t cfg = { .csi = &csi };
    if (esp_video_init(&cfg) != ESP_OK) {
        t5_xpin_output(T5_XP_CAM_RST, false);
        return T5_ERR_NOT_FOUND;
    }
    s_video_inited = true;
    find_formats();
    return T5_OK;
}

static void cam_release(void *h)
{
    t5_camera_close(h);
}

/* Stop the capture task and wait for it to exit. It owns c until then: if
 * it doesn't exit, c must not be freed (the task would write into memory
 * already reused by someone else, e.g. the next open's semaphores). */
static bool stop_task(t5_camera_t *c)
{
    c->stop = true;
    if (!c->task) return true;
    int64_t t0 = esp_timer_get_time();
    bool exited = xSemaphoreTake(c->exited, pdMS_TO_TICKS(5000)) == pdTRUE;
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (!exited) {
        T5_LOGE(TAG, "capture task didn't stop in %d ms: leaking it", ms);
        return false;
    }
    if (ms > 200) T5_LOGW(TAG, "capture task took %d ms to stop", ms);
    return true;
}

static void free_cam(t5_camera_t *c)
{
    if (c->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(c->fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < NBUF; i++) {
            if (c->buf[i]) munmap(c->buf[i], c->buflen[i]);
        }
        close(c->fd);
    }
    for (int i = 0; i < c->nlent; i++) t5_plane_release(c->lent_plane[i]);
    t5_plane_release(c->plane);  /* the screen may still hold it */
    if (c->photo_done) vSemaphoreDelete(c->photo_done);
    if (c->snap_done) vSemaphoreDelete(c->snap_done);
    if (c->ready) vSemaphoreDelete(c->ready);
    if (c->call_done) vSemaphoreDelete(c->call_done);
    if (c->exited) vSemaphoreDelete(c->exited);
    c->magic = 0;
    free(c);
}

/* Sensor registers the driver's tables don't set the way we want. */
static void sensor_tune(void)
{
    static t5_i2c_dev_t *dev;
    if (!dev && t5_i2c_open(T5_I2C_ADDR_SC2356, T5_SYS_I2C_HZ, "camera", &dev) != T5_OK) return;
    const uint8_t blc[2] = { 0x00, CAM_BLC_TARGET };   /* 0x3907 (high bits), 0x3908 */
    T5_LOG_ON_ERR(TAG, t5_i2c_write16(dev, 0x3907, blc, sizeof blc));
}

/* Runs on the capture task, pinned to core 0: the CSI controller and its
 * DMA allocate interrupts on the calling core, and core 1 (the UI task's)
 * has none left. */
static t5_err_t cam_setup(t5_camera_t *c)
{
    t5_err_t e = video_up();
    if (e != T5_OK) return e;
    c->fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (c->fd < 0) return T5_ERR_IO;
    const esp_cam_sensor_format_t *fmt = c->w == T5_CAMERA_PHOTO_W ? s_fmt_photo : s_fmt_video;
    if (!fmt || ioctl(c->fd, VIDIOC_S_SENSOR_FMT, fmt) != 0) return T5_ERR_NOT_SUPPORTED;
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    f.fmt.pix.width = (uint32_t)c->w;
    f.fmt.pix.height = (uint32_t)c->h;
    f.fmt.pix.pixelformat = c->rgb ? V4L2_PIX_FMT_RGB565 : V4L2_PIX_FMT_YUV420;
    if (ioctl(c->fd, VIDIOC_S_FMT, &f) != 0) return T5_ERR_NOT_SUPPORTED;
    struct v4l2_requestbuffers rq = { .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(c->fd, VIDIOC_REQBUFS, &rq) != 0) return T5_ERR_NO_MEM;
    for (int i = 0; i < NBUF; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = (uint32_t)i };
        if (ioctl(c->fd, VIDIOC_QUERYBUF, &b) != 0) return T5_ERR_NO_MEM;
        c->buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, b.m.offset);
        c->buflen[i] = b.length;
        if (!c->buf[i] || ioctl(c->fd, VIDIOC_QBUF, &b) != 0) return T5_ERR_NO_MEM;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(c->fd, VIDIOC_STREAMON, &type) != 0) return T5_ERR_IO;
    sensor_tune();
    return T5_OK;
}

static t5_err_t open_mode(t5_camera_t **out, int w, int h, bool rgb)
{
    if (!out) return T5_ERR_INVALID_ARG;
    int pe = t5_core_perm_check(T5_CORE_PERM_CAMERA);
    if (pe != T5_OK) return (t5_err_t)pe;
    t5_mutex_lock(s_mx);
    if (s_cam) {
        t5_mutex_unlock(s_mx);
        return T5_ERR_BUSY;
    }
    t5_camera_t *c = calloc(1, sizeof *c);
    if (!c) {
        t5_mutex_unlock(s_mx);
        return T5_ERR_NO_MEM;
    }
    static uint32_t s_serial;
    c->magic = CAM_MAGIC;
    c->serial = ++s_serial;
    c->fd = -1;
    c->w = w;
    c->h = h;
    c->rgb = rgb;
    c->photo_done = xSemaphoreCreateBinary();
    c->snap_done = xSemaphoreCreateBinary();
    c->ready = xSemaphoreCreateBinary();
    c->call_done = xSemaphoreCreateBinary();
    c->exited = xSemaphoreCreateBinary();
    t5_err_t e = c->photo_done && c->snap_done && c->ready && c->call_done && c->exited ? T5_OK : T5_ERR_NO_MEM;
    c->running = true;
    if (e == T5_OK && xTaskCreatePinnedToCore(capture_task, "camera", 6144, c, 12, &c->task, 0) != pdPASS)
        e = T5_ERR_NO_MEM;
    if (e == T5_OK) {
        if (xSemaphoreTake(c->ready, pdMS_TO_TICKS(5000)) != pdTRUE) e = T5_ERR_TIMEOUT;
        else e = c->setup_err;
    }
    if (e != T5_OK) {
        if (stop_task(c)) free_cam(c);
        t5_mutex_unlock(s_mx);
        T5_LOGE(TAG, "open: %s", t5_err_str(e));
        return e;
    }
    s_cam = c;
    memset(&s_st, 0, sizeof s_st);
    snprintf(s_st.sensor, sizeof s_st.sensor, "SC202CS");
    s_st.open = s_st.streaming = true;
    t5_res_add(T5_RES_OTHER, c, cam_release);   /* under the lock: an exiting app can't be stopped between */
    t5_mutex_unlock(s_mx);
    t5_ev_media_t ev = { .on = true };
    snprintf(ev.owner, sizeof ev.owner, "%s", t5_caller_id());
    t5_event_post(T5_EV_CAMERA_STATE, &ev, sizeof ev);
    T5_LOGI(TAG, "camera open: %dx%d %s", c->w, c->h, c->rgb ? "RGB565" : "YUV420");
    *out = c;
    return T5_OK;
}

t5_err_t t5_camera_open(t5_camera_t **out)
{
    return open_mode(out, T5_CAMERA_W, T5_CAMERA_H, false);
}

t5_err_t t5_camera_open_photo(t5_camera_t **out)
{
    return open_mode(out, T5_CAMERA_PHOTO_W, T5_CAMERA_PHOTO_H, true);
}

t5_err_t t5_camera_open_rgb(t5_camera_t **out)
{
    return open_mode(out, T5_CAMERA_W, T5_CAMERA_H, true);
}

t5_err_t t5_camera_close(t5_camera_t *c)
{
    if (!c || c->magic != CAM_MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(c);
    if (c->recording) t5_camera_record_stop(c);
    /* A snapshot waiting for a frame holds s_call_mx: let it finish. */
    t5_mutex_lock(s_call_mx);
    t5_mutex_unlock(s_call_mx);
    bool stopped = stop_task(c);
    t5_mutex_lock(s_mx);
    if (s_cam == c) s_cam = NULL;
    t5_mutex_unlock(s_mx);
    /* The preview object belongs to the caller's screen: it keeps a copy of
     * the last frame before the capture buffers go. */
    if (stopped) {
        for (int i = 0; i < c->nlent; i++) t5_plane_end(c->lent_plane[i]);
        free_cam(c);
    }
    s_st.open = s_st.streaming = s_st.recording = false;
    t5_ev_media_t ev = { .on = false };
    t5_event_post(T5_EV_CAMERA_STATE, &ev, sizeof ev);
    T5_LOGI(TAG, "camera closed");
    return T5_OK;
}

bool t5_camera_active(void)
{
    return s_cam != NULL;
}

t5_err_t t5_camera_photo(t5_camera_t *c, const char *path, int quality)
{
    if (!c || c->magic != CAM_MAGIC || !path) return T5_ERR_INVALID_ARG;
    if (!t5_core_path_check(path, true)) return T5_ERR_DENIED;
    if (on_ui_task()) return T5_ERR_STATE;      /* it waits for a frame */
    snprintf(c->photo_path, sizeof c->photo_path, "%s", path);
    c->photo_q = quality > 0 && quality <= 100 ? quality : 85;
    xSemaphoreTake(c->photo_done, 0);
    c->photo_req = true;
    if (xSemaphoreTake(c->photo_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
        c->photo_req = false;
        return T5_ERR_TIMEOUT;
    }
    return c->photo_err;
}

typedef struct {
    const char *path;
    uint32_t kbps;
} rec_args_t;

static bool is_avi(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && path[n - 4] == '.' && (path[n - 3] | 0x20) == 'a' && (path[n - 2] | 0x20) == 'v' &&
           (path[n - 1] | 0x20) == 'i';
}

static void mj_free(t5_camera_t *c)
{
    free(c->mj_rgb);
    free(c->mj_out);
    c->mj_rgb = NULL;
    c->mj_out = NULL;
    c->mjpeg = false;
}

/* Start the writer for a job whose file is open (s_writers counted). */
static t5_err_t start_writer(rec_job_t *j)
{
    /* PSRAM stack: internal RAM is short while recording. */
    return xTaskCreatePinnedToCoreWithCaps(writer_task, "cam.write", 4096, j, 6, NULL, 0,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ? T5_OK : T5_ERR_NO_MEM;
}

static t5_err_t rec_start_mjpeg(t5_camera_t *c, const char *path)
{
    T5_RETURN_ON_ERR(jpeg_encoder_ready());
    jpeg_encode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t cap = 0;
    c->mj_rgb = c->rgb ? NULL : jpeg_alloc_encoder_mem((size_t)c->w * c->h * 2, &in_cfg, &cap);
    c->mj_out = jpeg_alloc_encoder_mem((size_t)c->w * c->h, &out_cfg, &c->mj_out_cap);
    rec_job_t *j = calloc(1, sizeof *j);
    if (j) j->ring = xStreamBufferCreateWithCaps(RING_BYTES_MJPEG, 1, MALLOC_CAP_SPIRAM);
    if (j && j->ring) j->file = fopen(path, "wb");
    t5_err_t e = (c->mj_rgb || c->rgb) && c->mj_out && j && j->ring ? T5_OK : T5_ERR_NO_MEM;
    if (e == T5_OK && !j->file) e = T5_ERR_IO;
    if (e == T5_OK) {
        setvbuf(j->file, NULL, _IOFBF, 32 * 1024);
        j->avi = true;
        j->info = (avi_info_t){ .w = (uint16_t)c->w, .h = (uint16_t)c->h };
        uint8_t hdr[AVI_HDR_LEN];
        avi_header(hdr, &j->info);   /* placeholders until the writer finishes */
        if (fwrite(hdr, 1, sizeof hdr, j->file) != sizeof hdr) e = T5_ERR_IO;
    }
    if (e == T5_OK) {
        __atomic_add_fetch(&s_writers, 1, __ATOMIC_SEQ_CST);
        e = start_writer(j);
        if (e != T5_OK) __atomic_sub_fetch(&s_writers, 1, __ATOMIC_SEQ_CST);
    }
    if (e != T5_OK) {
        if (j && j->file) fclose(j->file);
        if (j && j->ring) vStreamBufferDeleteWithCaps(j->ring);
        free(j);
        mj_free(c);
        return e;
    }
    s_job = j;
    c->ring = j->ring;
    c->mjpeg = true;
    c->mj_q = c->mj_q_max = MJ_Q_DEFAULT;
    c->mj_q_changed_us = esp_timer_get_time();
    c->mj_have_seq = false;
    c->mj_t_first = c->mj_t_last = 0;
    return T5_OK;
}

static t5_err_t rec_start_impl(t5_camera_t *c, void *arg)
{
    const char *path = ((rec_args_t *)arg)->path;
    uint32_t kbps = ((rec_args_t *)arg)->kbps ? ((rec_args_t *)arg)->kbps : KBPS_DEFAULT;
    if (c->recording || s_writers) return T5_ERR_BUSY;   /* or the last one still flushing */
    if (is_avi(path)) {
        /* The JPEG encoder takes any size, but the player and the ring are
         * sized for the video mode. */
        if (c->w != T5_CAMERA_W) return T5_ERR_NOT_SUPPORTED;
        T5_RETURN_ON_ERR(rec_start_mjpeg(c, path));
        s_st.rec_frames = s_st.rec_dropped = 0;
        s_st.rec_bytes = 0;
        s_st.encode_us_max = s_st.write_buffer_peak = 0;
        c->rec_t0 = esp_timer_get_time();
        c->recording = true;
        s_st.recording = true;
        T5_LOGI(TAG, "recording to %s (Motion JPEG, quality %d)", path, c->mj_q);
        return T5_OK;
    }
    /* 1600 wide, the encoder's reference buffer (115 KB, internal RAM) is
     * bigger than the 100 KB the UI can lend it: record in the video mode,
     * whose frames are YUV420 (t5_camera_open). */
    if (c->w != T5_CAMERA_W || c->rgb) return T5_ERR_NOT_SUPPORTED;
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = T5_CAMERA_FPS, .fps = T5_CAMERA_FPS,
        .res = { .width = (uint16_t)c->w, .height = (uint16_t)c->h },
        /* qp_max 42 leaves the rate control room to go down when the card is slow. */
        .rc = { .bitrate = kbps * 1000, .qp_min = 25, .qp_max = 42 },
    };
    encoder_ram_lend();
    if (esp_h264_enc_hw_new(&cfg, &c->enc) != ESP_H264_ERR_OK || esp_h264_enc_open(c->enc) != ESP_H264_ERR_OK) {
        if (c->enc) esp_h264_enc_del(c->enc);
        c->enc = NULL;
        encoder_ram_reclaim();
        return T5_ERR_NO_MEM;
    }
    c->enc_param = NULL;
    if (esp_h264_enc_hw_get_param_hd(c->enc, &c->enc_param) != ESP_H264_ERR_OK) c->enc_param = NULL;
    c->kbps_target = c->kbps_now = kbps;
    c->kbps_changed_us = esp_timer_get_time();
    c->enc_out_len = (uint32_t)(c->w * c->h * 3 / 2);
    c->enc_out = esp_h264_aligned_calloc(ALIGN, 1, c->enc_out_len, &c->enc_out_len, MALLOC_CAP_SPIRAM);
    rec_job_t *j = calloc(1, sizeof *j);
    if (j) j->ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (j && j->ring) j->file = fopen(path, "wb");
    if (j && j->file) setvbuf(j->file, NULL, _IOFBF, 32 * 1024);
    __atomic_add_fetch(&s_writers, 1, __ATOMIC_SEQ_CST);
    if (!c->enc_out || !j || !j->file || start_writer(j) != T5_OK) {
        __atomic_sub_fetch(&s_writers, 1, __ATOMIC_SEQ_CST);
        if (j && j->file) fclose(j->file);
        if (j && j->ring) vStreamBufferDeleteWithCaps(j->ring);
        free(j);
        heap_caps_free(c->enc_out);
        esp_h264_enc_close(c->enc);
        esp_h264_enc_del(c->enc);
        c->enc = NULL;
        c->enc_out = NULL;
        encoder_ram_reclaim();
        return T5_ERR_IO;
    }
    s_job = j;
    c->ring = j->ring;
    s_st.rec_frames = s_st.rec_dropped = 0;
    s_st.rec_bytes = 0;
    s_st.encode_us_max = s_st.write_buffer_peak = 0;
    c->rec_t0 = esp_timer_get_time();
    c->recording = true;
    s_st.recording = true;
    T5_LOGI(TAG, "recording to %s at %u kbit/s", path, (unsigned)kbps);
    return T5_OK;
}

static t5_err_t rec_stop_impl(t5_camera_t *c, void *arg)
{
    (void)arg;
    if (!c->recording) return T5_ERR_STATE;
    c->recording = false;        /* we are the capture task: not inside encode() */
    c->ring = NULL;
    if (c->mjpeg) {
        rec_job_t *j = s_job;
        uint32_t n = j->info.frames;
        if (n > 1 && c->mj_t_last > c->mj_t_first)
            j->info.us_per_frame = (uint32_t)((c->mj_t_last - c->mj_t_first) / (n - 1));
        else j->info.us_per_frame = 1000000 / T5_CAMERA_FPS;
        mj_free(c);
        __atomic_store_n(&j->stop, true, __ATOMIC_RELEASE);   /* the writer finishes the AVI */
        s_job = NULL;
        s_st.recording = false;
        T5_LOGI(TAG, "recorded %u frames (%u repeats), %llu bytes, %u ms", (unsigned)s_st.rec_frames,
                (unsigned)s_st.rec_dropped, (unsigned long long)s_st.rec_bytes, (unsigned)s_st.rec_ms);
        return T5_OK;
    }
    __atomic_store_n(&s_job->stop, true, __ATOMIC_RELEASE);   /* the writer drains, closes and frees */
    s_job = NULL;
    esp_h264_enc_close(c->enc);
    esp_h264_enc_del(c->enc);
    c->enc = NULL;
    c->enc_param = NULL;
    heap_caps_free(c->enc_out);
    c->enc_out = NULL;
    encoder_ram_reclaim();
    s_st.recording = false;
    T5_LOGI(TAG, "recorded %u frames (%u dropped), %llu bytes, %u ms", (unsigned)s_st.rec_frames,
            (unsigned)s_st.rec_dropped, (unsigned long long)s_st.rec_bytes, (unsigned)s_st.rec_ms);
    return T5_OK;
}

static t5_err_t cam_call(t5_camera_t *c, t5_err_t (*fn)(t5_camera_t *, void *), void *arg)
{
    if (xTaskGetCurrentTaskHandle() == c->task || !c->running) return fn(c, arg);
    t5_mutex_lock(s_call_mx);                /* one caller at a time; not s_mx, which capture takes */
    xSemaphoreTake(c->call_done, 0);
    c->call_arg = arg;
    __atomic_store_n(&c->call_fn, fn, __ATOMIC_RELEASE);
    t5_err_t e;
    if (xSemaphoreTake(c->call_done, pdMS_TO_TICKS(5000)) == pdTRUE) {
        e = c->call_res;
    } else {
        /* Take it back unless it started: `arg` lives on our stack, so a
         * call left behind ran later on a dead frame and crashed the
         * Camera app's recording (2026-11-07). Started: wait it out. */
        t5_err_t (*expect)(t5_camera_t *, void *) = fn;
        if (__atomic_compare_exchange_n(&c->call_fn, &expect, NULL, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            e = T5_ERR_TIMEOUT;
        } else {
            xSemaphoreTake(c->call_done, portMAX_DELAY);
            e = c->call_res;
        }
    }
    t5_mutex_unlock(s_call_mx);
    return e;
}

t5_err_t t5_camera_record_start(t5_camera_t *c, const char *path, uint32_t kbps)
{
    if (!c || c->magic != CAM_MAGIC || !path) return T5_ERR_INVALID_ARG;
    if (!t5_core_path_check(path, true)) return T5_ERR_DENIED;
    /* Starting lends the encoder the UI's draw buffer, through the UI task:
     * from the UI task that deadlocked (and then crashed). */
    if (on_ui_task()) return T5_ERR_STATE;
    rec_args_t a = { path, kbps };
    return cam_call(c, rec_start_impl, &a);
}

t5_err_t t5_camera_record_stop(t5_camera_t *c)
{
    if (!c || c->magic != CAM_MAGIC) return T5_ERR_INVALID_ARG;
    if (on_ui_task()) return T5_ERR_STATE;    /* takes the draw buffer back, the same way */
    return cam_call(c, rec_stop_impl, NULL);
}

bool t5_camera_recording(const t5_camera_t *c)
{
    return c && c->magic == CAM_MAGIC && c->recording;
}

void t5_camera_stats(t5_camera_stats_t *out)
{
    *out = s_st;
}

t5_err_t t5_camera_snapshot(uint16_t *out, int w, int h)
{
    if (!out || w < 1 || h < 1) return T5_ERR_INVALID_ARG;
    if (on_ui_task()) return T5_ERR_STATE;      /* it waits for a frame */
    t5_mutex_lock(s_call_mx);                   /* one at a time */
    t5_mutex_lock(s_mx);
    t5_camera_t *c = s_cam;
    t5_mutex_unlock(s_mx);
    t5_err_t e = T5_ERR_STATE;
    if (c) {
        c->snap_out = out;
        c->snap_w = w;
        c->snap_h = h;
        xSemaphoreTake(c->snap_done, 0);
        c->snap_req = true;
        e = xSemaphoreTake(c->snap_done, pdMS_TO_TICKS(3000)) == pdTRUE ? c->snap_err : T5_ERR_TIMEOUT;
        c->snap_req = false;
    }
    t5_mutex_unlock(s_call_mx);
    return e;
}

/* JPEG decoding (gallery, UVC). */
typedef struct {
    const uint8_t *jpeg;
    size_t len;
    uint16_t *out;
    size_t out_bytes;
    uint16_t w, h;
    bool direct;                 /* jpeg and out are DMA buffers: no copies */
    t5_err_t err;
    SemaphoreHandle_t done;
} dec_req_t;

static QueueHandle_t s_dec_q;
static jpeg_decoder_handle_t s_dec;
static uint8_t *s_dec_in, *s_dec_out;
static size_t s_dec_in_cap, s_dec_out_cap;

t5_err_t t5_jpeg_size(const uint8_t *jpeg, size_t len, uint16_t *w, uint16_t *h)
{
    jpeg_decode_picture_info_t info;
    if (!jpeg || jpeg_decoder_get_info(jpeg, (uint32_t)len, &info) != ESP_OK) return T5_ERR_NOT_SUPPORTED;
    *w = (uint16_t)((info.width + 15) & ~15u);
    *h = (uint16_t)((info.height + 15) & ~15u);
    return T5_OK;
}

static t5_err_t dec_grow(uint8_t **buf, size_t *cap, size_t need, jpeg_dec_buffer_alloc_direction_t dir)
{
    if (*cap >= need) return T5_OK;
    free(*buf);
    jpeg_decode_memory_alloc_cfg_t c = { .buffer_direction = dir };
    *buf = jpeg_alloc_decoder_mem(need, &c, cap);
    if (!*buf) *cap = 0;
    return *buf ? T5_OK : T5_ERR_NO_MEM;
}

static t5_err_t dec_run(dec_req_t *r)
{
    if (!s_dec) {
        jpeg_decode_engine_cfg_t ec = { .intr_priority = 0, .timeout_ms = 1000 };
        if (jpeg_new_decoder_engine(&ec, &s_dec) != ESP_OK) return T5_ERR_NOT_SUPPORTED;
    }
    T5_RETURN_ON_ERR(t5_jpeg_size(r->jpeg, r->len, &r->w, &r->h));
    size_t need = (size_t)r->w * r->h * 2;
    if (need > r->out_bytes) return T5_ERR_NO_MEM;
    jpeg_decode_cfg_t dc = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565, .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR };
    uint32_t got = 0;
    if (r->direct) {
        return jpeg_decoder_process(s_dec, &dc, r->jpeg, (uint32_t)r->len, (uint8_t *)r->out, (uint32_t)r->out_bytes,
                                    &got) == ESP_OK ? T5_OK : T5_ERR_IO;
    }
    T5_RETURN_ON_ERR(dec_grow(&s_dec_in, &s_dec_in_cap, r->len, JPEG_DEC_ALLOC_INPUT_BUFFER));
    T5_RETURN_ON_ERR(dec_grow(&s_dec_out, &s_dec_out_cap, need, JPEG_DEC_ALLOC_OUTPUT_BUFFER));
    memcpy(s_dec_in, r->jpeg, r->len);
    if (jpeg_decoder_process(s_dec, &dc, s_dec_in, (uint32_t)r->len, s_dec_out, (uint32_t)s_dec_out_cap, &got) != ESP_OK)
        return T5_ERR_IO;
    memcpy(r->out, s_dec_out, need);
    return T5_OK;
}

static void dec_task(void *arg)
{
    (void)arg;
    dec_req_t *r;
    for (;;) {
        if (xQueueReceive(s_dec_q, &r, portMAX_DELAY) != pdTRUE) continue;
        r->err = dec_run(r);
        xSemaphoreGive(r->done);
    }
}

t5_err_t t5_jpeg_dims(const uint8_t *jpeg, size_t len, uint16_t *w, uint16_t *h)
{
    jpeg_decode_picture_info_t info;
    if (!jpeg || jpeg_decoder_get_info(jpeg, (uint32_t)len, &info) != ESP_OK) return T5_ERR_NOT_SUPPORTED;
    *w = (uint16_t)info.width;
    *h = (uint16_t)info.height;
    return T5_OK;
}

void *t5_jpeg_alloc(size_t bytes)
{
    size_t n = (bytes + 127) & ~(size_t)127;
    return heap_caps_aligned_calloc(128, 1, n ? n : 128, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static t5_err_t jpeg_decode_req(const uint8_t *jpeg, size_t len, uint16_t *out, size_t out_bytes, uint16_t *w,
                                uint16_t *h, bool direct);

t5_err_t t5_jpeg_decode_dma(const uint8_t *jpeg, size_t len, uint16_t *out, size_t out_bytes, uint16_t *w, uint16_t *h)
{
    if (((uintptr_t)jpeg & 15) || ((uintptr_t)out & 127) || (out_bytes & 127)) return T5_ERR_INVALID_ARG;
    return jpeg_decode_req(jpeg, len, out, out_bytes, w, h, true);
}

t5_err_t t5_jpeg_decode(const uint8_t *jpeg, size_t len, uint16_t *out, size_t out_bytes, uint16_t *w, uint16_t *h)
{
    return jpeg_decode_req(jpeg, len, out, out_bytes, w, h, false);
}

static t5_err_t jpeg_decode_req(const uint8_t *jpeg, size_t len, uint16_t *out, size_t out_bytes, uint16_t *w,
                                uint16_t *h, bool direct)
{
    if (!jpeg || !out || !s_dec_q) return T5_ERR_INVALID_ARG;
    dec_req_t r = { .jpeg = jpeg, .len = len, .out = out, .out_bytes = out_bytes, .direct = direct,
                    .err = T5_ERR_TIMEOUT, .done = xSemaphoreCreateBinary() };
    if (!r.done) return T5_ERR_NO_MEM;
    dec_req_t *rp = &r;
    t5_err_t e = T5_ERR_TIMEOUT;
    if (xQueueSend(s_dec_q, &rp, pdMS_TO_TICKS(1000)) == pdTRUE && xSemaphoreTake(r.done, pdMS_TO_TICKS(3000)) == pdTRUE)
        e = r.err;
    vSemaphoreDelete(r.done);
    if (e == T5_OK) {
        if (w) *w = r.w;
        if (h) *h = r.h;
    }
    return e;
}

static t5_err_t cam_suspend(void *arg)
{
    (void)arg;
    /* Apps should have closed it in on_suspend; if not, close it here so
     * the sensor is in reset while asleep. */
    if (s_cam) {
        T5_LOGW(TAG, "camera still open at suspend: closing it");
        t5_camera_close(s_cam);
    }
    return T5_OK;
}

t5_err_t t5_camera_init(void)
{
    if (s_mx) return T5_OK;
    s_mx = t5_mutex_create();
    s_call_mx = t5_mutex_create();
    if (!s_mx || !s_call_mx) return T5_ERR_NO_MEM;
    s_dec_q = xQueueCreate(4, sizeof(dec_req_t *));
    if (!s_dec_q || xTaskCreatePinnedToCoreWithCaps(dec_task, "jpeg.dec", 4096, NULL, 6, NULL, 0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return T5_ERR_NO_MEM;
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (ppa_register_client(&pc, &s_ppa) != ESP_OK) return T5_ERR_NO_MEM;
    const t5_suspend_hook_t hook = {
        .name = "camera", .prio = T5_SUSPEND_PRIO_MEDIA, .budget_ms = 300, .suspend = cam_suspend,
    };
    T5_LOG_ON_ERR(TAG, t5_power_register_hook(&hook));
    return T5_OK;
}
