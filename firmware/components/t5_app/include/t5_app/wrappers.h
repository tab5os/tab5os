/* SPDX-License-Identifier: Apache-2.0
 * Tracking wrappers bound in place of plain functions for apps (see abi.h
 * and sdk/abi/api_v1.sym). Each has exactly the signature of the function
 * it replaces.
 */
#pragma once

#include <stddef.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "t5_core/event.h"
#include "t5_core/os.h"
#include "t5_ui/t5_ui.h"
#include "t5_usb/usb.h"
#include <sys/stat.h>
#include <dirent.h>
#include "lwip/sockets.h"

#ifdef __cplusplus
extern "C" {
#endif

void *t5_app_malloc(size_t n);
void *t5_app_calloc(size_t n, size_t sz);
void *t5_app_realloc(void *p, size_t n);
void t5_app_free(void *p);
char *t5_app_strdup(const char *s);

void *t5_app_lv_malloc_zeroed(size_t n);
void *t5_app_lv_realloc(void *p, size_t n);
void *t5_app_lv_reallocf(void *p, size_t n);
char *t5_app_lv_strdup(const char *s);
char *t5_app_lv_strndup(const char *s, size_t max);
void t5_app_lv_label_set_text(lv_obj_t *obj, const char *text);
void t5_app_lv_label_set_text_fmt(lv_obj_t *obj, const char *fmt, ...);
void t5_app_lv_label_set_text_vfmt(lv_obj_t *obj, const char *fmt, va_list args);
lv_draw_buf_t *t5_app_lv_draw_buf_create(uint32_t w, uint32_t h, lv_color_format_t cf, uint32_t stride);
lv_draw_buf_t *t5_app_lv_draw_buf_create_ex(const lv_draw_buf_handlers_t *handlers, uint32_t w, uint32_t h,
                                            lv_color_format_t cf, uint32_t stride);
lv_draw_buf_t *t5_app_lv_draw_buf_dup(const lv_draw_buf_t *b);
lv_draw_buf_t *t5_app_lv_draw_buf_dup_ex(const lv_draw_buf_handlers_t *handlers, const lv_draw_buf_t *b);
lv_draw_buf_t *t5_app_lv_snapshot_take(lv_obj_t *obj, lv_color_format_t cf);
void t5_app_lv_draw_buf_destroy(lv_draw_buf_t *b);
T5_MUST_CHECK t5_err_t t5_app_task_create(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes, int prio, int core,
                            t5_task_t *out);
void t5_app_ui_noop_log_cb(lv_log_print_g_cb_t cb);
T5_MUST_CHECK t5_err_t t5_app_ui_async(t5_ui_fn_t fn, void *arg);
T5_MUST_CHECK t5_err_t t5_app_ui_sync(t5_ui_fn_t fn, void *arg, uint32_t timeout_ms);
lv_result_t t5_app_lv_async_call(lv_async_cb_t cb, void *arg);
lv_result_t t5_app_lv_async_call_cancel(lv_async_cb_t cb, void *arg);
size_t t5_app_fwrite(const void *buf, size_t size, size_t n, FILE *f);
int t5_app_fputs(const char *s, FILE *f);
int t5_app_fprintf(FILE *f, const char *fmt, ...);
int t5_app_stat(const char *path, struct stat *st);
size_t t5_app_fread(void *buf, size_t size, size_t n, FILE *f);
char *t5_app_fgets(char *s, int n, FILE *f);
int t5_app_fgetc(FILE *f);
int t5_app_fflush(FILE *f);
int t5_app_fseek(FILE *f, long off, int whence);
struct dirent *t5_app_readdir(DIR *d);
DIR *t5_app_opendir(const char *path);
int t5_app_closedir(DIR *d);
T5_MUST_CHECK t5_err_t t5_app_storage_write_atomic(const char *path, const void *data, size_t len);
int t5_app_remove(const char *path);
int t5_app_rename(const char *from, const char *to);
int t5_app_mkdir(const char *path, mode_t mode);
T5_MUST_CHECK t5_err_t t5_app_usb_midi_listen(t5_usb_midi_cb_t cb, void *arg);
T5_MUST_CHECK t5_err_t t5_app_usb_uvc_open(uint16_t w, uint16_t h, uint8_t fps, t5_usb_uvc_frame_cb_t cb, void *arg, t5_usb_uvc_t **out);
T5_MUST_CHECK t5_err_t t5_app_usb_uvc_close(t5_usb_uvc_t *u);
/* Not bound to apps: installed at start, polled by the watchdog. */
void t5_app_lv_mem_install(void);
void t5_app_events_check_stuck(void);

BaseType_t t5_app_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const configSTACK_DEPTH_TYPE stack,
                                          void *const arg, UBaseType_t prio, TaskHandle_t *const out,
                                          const BaseType_t core);
void t5_app_vTaskDelete(TaskHandle_t h);
QueueHandle_t t5_app_xQueueGenericCreate(const UBaseType_t len, const UBaseType_t item, const uint8_t type);
QueueHandle_t t5_app_xQueueCreateMutex(const uint8_t type);
QueueHandle_t t5_app_xQueueCreateCountingSemaphore(const UBaseType_t max, const UBaseType_t initial);
void t5_app_vQueueDelete(QueueHandle_t q);
t5_mutex_t t5_app_mutex_create(void);
void t5_app_mutex_delete(t5_mutex_t m);

lv_timer_t *t5_app_lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *user_data);
void t5_app_lv_timer_delete(lv_timer_t *t);
lv_anim_t *t5_app_lv_anim_start(const lv_anim_t *a);

T5_MUST_CHECK t5_err_t t5_app_event_subscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg);
T5_MUST_CHECK t5_err_t t5_app_event_unsubscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg);

FILE *t5_app_fopen(const char *path, const char *mode);
int t5_app_fclose(FILE *f);

int t5_app_lwip_socket(int domain, int type, int protocol);
int t5_app_lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen);
int t5_app_lwip_close(int s);
int t5_app_close(int fd);
int *t5_errno(void);
__attribute__((noreturn)) void t5_app_abort(void);
__attribute__((noreturn)) void t5_app_assert_func(const char *file, int line, const char *func, const char *expr);

#ifdef __cplusplus
}
#endif
