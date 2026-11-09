/* Album art: a JPEG cover, embedded in the track (ID3 APIC, FLAC PICTURE) or
 * next to it (cover.jpg, folder.jpg, front.jpg, album.jpg, albumart.jpg),
 * decoded on a task of its own (TJpgDec, scaled 1/2, 1/4 or 1/8 while it
 * decodes) and cropped to a centred square of px pixels, RGB565.
 *
 * Requests are served newest first (a scrolled list wants what's on screen
 * now). The result comes back on the UI task; the image is the caller's to
 * free with art_free. Progressive JPEGs aren't supported by TJpgDec: no art. */
#pragma once
#include <stdint.h>
#include "lvgl.h"

typedef void (*art_done_fn)(uint32_t key, lv_image_dsc_t *img, void *user);   /* img NULL: none */

void art_start(void);
void art_stop(void);
/* key: the caller's (an album, a track); a pending request with the same key
 * and size is replaced. */
void art_request(uint32_t key, const char *track_path, uint32_t art_off, uint32_t art_len, int px, art_done_fn fn,
                 void *user);
/* Drop the pending requests made with fn (a list was rebuilt). */
void art_cancel(art_done_fn fn);
void art_free(lv_image_dsc_t *img);
