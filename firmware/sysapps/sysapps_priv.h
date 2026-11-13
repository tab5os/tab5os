/* SPDX-License-Identifier: Apache-2.0
 * Shared helpers for the system apps: a page stack with the MD2 app bar
 * (back pops, or exits at the root), a once-a-second refresh of the page in
 * front, and small formatting helpers.
 *
 * Built-in apps link to the OS directly, so the ABI's tracking wrappers
 * don't see their timers, tasks or event handlers: each app deletes its
 * own in on_destroy (sa_nav_destroy does the timer). Their screen and the
 * objects under it are freed by the runtime as for any app.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "t5/app.h"
#include "t5/ui.h"
#include "t5_gui/t5ui.h"
#include "t5_pkg/manifest.h"

#define SA_MAX_DEPTH 8

typedef struct sa_nav sa_nav_t;
typedef void (*sa_refresh_fn_t)(sa_nav_t *nav, void *arg);
typedef void (*sa_free_fn_t)(void *arg);

typedef struct {
    t5ui_obj_t *page, *body;
    sa_refresh_fn_t refresh;
    sa_free_fn_t free_arg;
    void *arg;
} sa_level_t;

struct sa_nav {
    t5_app_t *app;
    t5ui_obj_t *root;
    sa_level_t lv[SA_MAX_DEPTH];
    int depth;
    t5ui_timer_t *timer;
    void (*root_back)(void);      /* back on the last page: pop it and call this, instead of exiting */
};

void sa_nav_init(sa_nav_t *nav, t5_app_t *app);
void sa_nav_destroy(sa_nav_t *nav);
/* A new page on top; returns its scrolling body. */
t5ui_obj_t *sa_push(sa_nav_t *nav, const char *title);
void sa_pop(sa_nav_t *nav);
void sa_pop_to_root(sa_nav_t *nav);
/* Refresh for the page in front: now, every second, and when it comes back
 * to the front. arg is freed with free_arg when the page goes. */
void sa_set_refresh(sa_nav_t *nav, sa_refresh_fn_t fn, void *arg, sa_free_fn_t free_arg);
/* The front page (a T5UI_MD_PAGE: T5UI_P_TITLE, T5UI_MD_APPBAR_ACTION
 * children go to its app bar) and its scrolling body. */
t5ui_obj_t *sa_bar(sa_nav_t *nav);
t5ui_obj_t *sa_body(sa_nav_t *nav);

/* Helpers */
/* Coding standard rule 7: a failed action the user asked for shows a
 * snackbar, "Couldn't <what> (<error>)". */
void sa_report(t5_err_t e, const char *what);
void sa_fmt_bytes(char *out, size_t n, uint64_t bytes);
void sa_fmt_duration(char *out, size_t n, uint32_t seconds);
void sa_set_text(t5ui_obj_t *obj, const char *text);     /* T5UI_P_TEXT, only if it changed */
t5ui_obj_t *sa_note(t5ui_obj_t *parent, const char *text);   /* body text with margins */
t5ui_obj_t *sa_button_row(t5ui_obj_t *parent);               /* a row for buttons */

/* Full-screen photo (.jpg) viewer over an app's screen (photo.c). One at
 * a time; on_close runs when it is closed (by its close button or
 * sa_photo_close). Apps close it in on_destroy. Pinch, double tap or
 * drag to zoom and pan. */
bool sa_photo_can_open(const char *path);
bool sa_photo_open(t5ui_obj_t *screen, const char *path, void (*on_close)(void));
bool sa_photo_is_open(void);
void sa_photo_close(void);

/* Full-screen video player (video.c), the same way: Motion JPEG in AVI,
 * with PCM sound if it has any. */
bool sa_video_can_open(const char *path);
bool sa_video_open(t5ui_obj_t *screen, const char *path, void (*on_close)(void));
bool sa_video_is_open(void);
void sa_video_close(void);

/* Case-insensitive suffix match. */
bool sa_ends_with(const char *s, const char *suffix);

/* PPA work for the viewers (ppa.c), blocking. Client 0 for the UI task,
 * 1 for the video player's task. Pictures are RGB565 in PSRAM. */
#define SA_PPA_CLIENTS 2
typedef struct {
    void *px;
    int w, h;
    size_t bytes;
} sa_pic_t;
/* n pixels at k/16, as the PPA rounds them. */
int sa_ppa_scaled(int n, int k);
/* The block (bx, by, bw, bh) of src, scaled by k/16, into dst at (ox, oy). */
t5_err_t sa_ppa_scale(int client, const sa_pic_t *src, int bx, int by, int bw, int bh, int k, const sa_pic_t *dst,
                      int ox, int oy);
/* Black. */
t5_err_t sa_ppa_fill(int client, const sa_pic_t *dst, int x, int y, int w, int h);

/* The apps */
#include "t5_app/apps.h"
#include "t5_app/registry.h"
extern const t5_builtin_app_t sysapp_settings;
extern const t5_builtin_app_t sysapp_files;
extern const t5_builtin_app_t sysapp_appmanager;
extern const t5_builtin_app_t sysapp_power;
extern const t5_builtin_app_t sysapp_ports;
extern const t5_builtin_app_t sysapp_camera;
extern const t5_builtin_app_t sysapp_terminal;
extern const t5_builtin_app_t sysapp_editor;
extern const t5_builtin_app_t sysapp_videos;
/* The ports a script run in Terminal or Editor may open: those in
 * t5_ports/ports_table.c except mbus.debug and the single M5-Bus GPIOs. A
 * hand copy of the table's names: keep the two in step. */
extern const char *const sa_all_ports[];
