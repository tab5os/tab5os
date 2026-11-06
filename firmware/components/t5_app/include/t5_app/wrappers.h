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
#include "t5_ports/ports.h"

#ifdef __cplusplus
extern "C" {
#endif

void *t5_app_malloc(size_t n);
void *t5_app_calloc(size_t n, size_t sz);
void *t5_app_realloc(void *p, size_t n);
void t5_app_free(void *p);
char *t5_app_strdup(const char *s);

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

t5_err_t t5_app_event_subscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg);
t5_err_t t5_app_event_unsubscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg);

FILE *t5_app_fopen(const char *path, const char *mode);
int t5_app_fclose(FILE *f);

t5_err_t t5_app_port_open(const char *name, t5_port_mode_t mode, const void *cfg, t5_port_t **out);
t5_err_t t5_app_port_open_ex(const char *name, t5_port_mode_t mode, const void *cfg, uint32_t flags, t5_port_t **out,
                             char *why, size_t whylen);

#ifdef __cplusplus
}
#endif
