/* SPDX-License-Identifier: Apache-2.0
 * Camera service (plan 7.4, 7.5): the SC202CS (sold as SC2356) on MIPI CSI
 * through esp_video's ISP, a preview converted and scaled by the PPA and
 * shown as a video plane (t5_core/plane.h), JPEG stills from the hardware
 * JPEG encoder, and recording to SD: Motion JPEG in AVI from the same
 * encoder (what the built-in player plays), or H.264 from the hardware
 * H.264 encoder.
 *
 * One owner at a time. Opening needs the camera permission (asked on first
 * use) and shows the camera indicator in the status bar. The sensor is held
 * in reset until the first open; after that a closed camera has its stream
 * off (sensor standby), as esp_video can't be torn down and brought up
 * again. The service closes it on suspend; apps reopen it in on_resume.
 *
 * t5_camera_open captures YUV420 at 1280x720, 30 fps: the H.264 encoder
 * takes it as it is; stills and MJPEG take a PPA pass to RGB565 for the JPEG
 * encoder (YUV420 JPEG input needs v3 silicon). The system Camera's modes
 * capture RGB565 instead, which skips that pass. Either way the preview is
 * the frame itself, scaled, converted and turned upright by the display
 * pipeline on its way to the screen (one PPA pass).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_core/plane.h"

#ifdef __cplusplus
extern "C" {
#endif

/* t5_camera_open: the sensor's 1280x720 mode (a window in the middle of
 * the sensor), for video and apps. */
#define T5_CAMERA_W 1280
#define T5_CAMERA_H 720
/* t5_camera_open_photo: the full sensor width, 10-bit, RGB565 (system
 * apps). It can't record: use t5_camera_open or t5_camera_open_rgb. */
#define T5_CAMERA_PHOTO_W 1600
#define T5_CAMERA_PHOTO_H 900
#define T5_CAMERA_FPS 30

typedef struct t5_camera t5_camera_t;
struct _lv_obj_t;

t5_err_t t5_camera_init(void);
bool t5_camera_active(void);

/* What the camera needs from the UI, installed by the composition root
 * (main), so the service doesn't depend on t5_ui. Any member may be NULL.
 *   rotation_deg  the screen's rotation, clockwise: 0, 90, 180 or 270
 *   on_ui_task    photo and recording wait on capture (recording also on
 *                 the UI task, to lend): they refuse there (T5_ERR_STATE)
 *   lend_ram      before the H.264 encoder starts: the UI's internal RAM
 *                 block (and its size) for the reference buffer, or NULL
 *   reclaim_ram   after it stops: the block back (NULL if the encoder
 *                 kept it)
 *   ui_call       run fn where LVGL is safe (the UI task, or under its
 *                 lock) and return once it ran
 *   plane_show    an image object in parent showing the plane (UI task;
 *                 t5_ui_plane_create): the preview needs it */
typedef struct {
    int (*rotation_deg)(void);
    bool (*on_ui_task)(void);
    void *(*lend_ram)(size_t *len);
    void (*reclaim_ram)(void *block);
    void (*ui_call)(void (*fn)(void *arg), void *arg);
    struct _lv_obj_t *(*plane_show)(struct _lv_obj_t *parent, t5_plane_t *plane);
} t5_camera_host_t;
void t5_camera_set_host(const t5_camera_host_t *host);

t5_err_t t5_camera_open(t5_camera_t **out);
/* The photo mode (system apps; not in the app ABI). */
t5_err_t t5_camera_open_photo(t5_camera_t **out);
/* The video mode captured as RGB565 (system apps; not in the app ABI):
 * faster MJPEG (.avi) recording and photos, but no H.264. */
t5_err_t t5_camera_open_rgb(t5_camera_t **out);
t5_err_t t5_camera_close(t5_camera_t *cam);

/* Live preview inside parent, scaled to fit and kept upright (UI task).
 * Returns the image object; deleting it stops the preview. */
struct _lv_obj_t *t5_camera_preview_create(t5_camera_t *cam, struct _lv_obj_t *parent);
void t5_camera_set_mirror(t5_camera_t *cam, bool mirror);

/* A JPEG still (quality 1..100, 0 = 85). Blocks until written (any task
 * but the UI task). Photos and recordings go only where the calling app
 * may write (T5_ERR_DENIED). */
t5_err_t t5_camera_photo(t5_camera_t *cam, const char *path, int quality);

/* Record to path, in the video mode (t5_camera_open). A path ending in
 * .avi records Motion JPEG in AVI, 1280x720 at the sensor's rate, which the
 * built-in player plays: JPEG quality is lowered while the card falls
 * behind, a frame the card can't take is stored as a repeat of the one
 * before so the timing stays right, and kbps is ignored. Any other path
 * records H.264 (Annex B elementary stream); kbps 0 = 4000, lowered while
 * the card falls behind. Start and stop block, and need the UI task
 * meanwhile: call them from any task but the UI task (T5_ERR_STATE). */
t5_err_t t5_camera_record_start(t5_camera_t *cam, const char *path, uint32_t kbps);
t5_err_t t5_camera_record_stop(t5_camera_t *cam);
bool t5_camera_recording(const t5_camera_t *cam);

typedef struct {
    bool open, streaming, recording;
    uint32_t frames;             /* captured */
    uint32_t dropped;            /* sequence gaps from the capture driver */
    uint32_t preview_frames;
    uint32_t fps;                /* captured, last second */
    uint32_t preview_fps;
    uint32_t rec_frames, rec_dropped;   /* encoded; frames lost to a full write buffer */
    uint64_t rec_bytes;
    uint32_t rec_ms;
    uint32_t encode_us_max;
    uint32_t write_buffer_peak;  /* bytes queued for SD, high-water mark */
    uint32_t photos;
    char sensor[16];
} t5_camera_stats_t;
void t5_camera_stats(t5_camera_stats_t *out);

/* Decode a baseline JPEG (a photo, a UVC camera's MJPEG frame) to RGB565
 * with the hardware decoder. The decoded size is the image's, rounded up to
 * 16; *w and *h get it. out must hold that many pixels (out_bytes). Any
 * task except the UI task's core needs no care: the work runs on core 0. */
t5_err_t t5_jpeg_decode(const uint8_t *jpeg, size_t len, uint16_t *out, size_t out_bytes, uint16_t *w, uint16_t *h);
/* The size a JPEG decodes to (rounded up to 16), without decoding. */
t5_err_t t5_jpeg_size(const uint8_t *jpeg, size_t len, uint16_t *w, uint16_t *h);
/* Its real size (not rounded). */
t5_err_t t5_jpeg_dims(const uint8_t *jpeg, size_t len, uint16_t *w, uint16_t *h);
/* t5_jpeg_decode without its two copies, for video: jpeg and out are
 * buffers from t5_jpeg_alloc (or a plane's), and out_bytes a multiple of
 * 128. */
t5_err_t t5_jpeg_decode_dma(const uint8_t *jpeg, size_t len, uint16_t *out, size_t out_bytes, uint16_t *w,
                            uint16_t *h);
/* PSRAM the JPEG decoder can read and write directly: aligned to 128, the
 * size rounded up to 128 (and zeroed). Free with free(). */
void *t5_jpeg_alloc(size_t bytes);

/* Diagnostics: the next frame as the preview shows it, scaled to w x h
 * RGB565. Any task but the UI task (it waits for the frame). */
t5_err_t t5_camera_snapshot(uint16_t *out, int w, int h);

#ifdef __cplusplus
}
#endif
