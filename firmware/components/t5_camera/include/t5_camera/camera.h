/* SPDX-License-Identifier: Apache-2.0
 * Camera service (plan 7.4, 7.5): the SC202CS (sold as SC2356) on MIPI CSI
 * through esp_video's ISP, a preview converted and scaled by the PPA, JPEG
 * stills from the hardware JPEG encoder and H.264 recording to SD from the
 * hardware H.264 encoder.
 *
 * One owner at a time. Opening needs the camera permission (asked on first
 * use) and shows the camera indicator in the status bar. The sensor is held
 * in reset until the first open; after that a closed camera has its stream
 * off (sensor standby), as esp_video can't be torn down and brought up
 * again. The service closes it on suspend; apps reopen it in on_resume.
 *
 * Frames are captured as YUV420 at 1280x720, 30 fps: the encoder takes them
 * as they are, the preview goes through one PPA pass (YUV420 to RGB565,
 * scale to fit, rotate to stay upright), and stills take a second pass to
 * RGB565 for the JPEG encoder (YUV420 JPEG input needs v3 silicon).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* t5_camera_open: the sensor's 1280x720 mode (a window in the middle of
 * the sensor), for video and apps. */
#define T5_CAMERA_W 1280
#define T5_CAMERA_H 720
/* t5_camera_open_photo: the full sensor width, 10-bit (system apps). It
 * can't record: use t5_camera_open for that. */
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
 *                 lock) and return once it ran */
typedef struct {
    int (*rotation_deg)(void);
    bool (*on_ui_task)(void);
    void *(*lend_ram)(size_t *len);
    void (*reclaim_ram)(void *block);
    void (*ui_call)(void (*fn)(void *arg), void *arg);
} t5_camera_host_t;
void t5_camera_set_host(const t5_camera_host_t *host);

t5_err_t t5_camera_open(t5_camera_t **out);
/* The photo mode (system apps; not in the app ABI). */
t5_err_t t5_camera_open_photo(t5_camera_t **out);
t5_err_t t5_camera_close(t5_camera_t *cam);

/* Live preview inside parent, scaled to fit and kept upright (UI task).
 * Returns the image object; deleting it stops the preview. */
struct _lv_obj_t *t5_camera_preview_create(t5_camera_t *cam, struct _lv_obj_t *parent);
void t5_camera_set_mirror(t5_camera_t *cam, bool mirror);

/* A JPEG still (quality 1..100, 0 = 85). Blocks until written (any task
 * but the UI task). Photos and recordings go only where the calling app
 * may write (T5_ERR_DENIED). */
t5_err_t t5_camera_photo(t5_camera_t *cam, const char *path, int quality);

/* H.264 (Annex B elementary stream) to path; kbps 0 = 4000, lowered while
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

/* Diagnostics: copy the newest preview frame, scaled to w x h RGB565. */
t5_err_t t5_camera_snapshot(uint16_t *out, int w, int h);

#ifdef __cplusplus
}
#endif
