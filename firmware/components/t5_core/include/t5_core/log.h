/* SPDX-License-Identifier: Apache-2.0
 * Logging. One tag per service: "t5.i2c", "t5.rtc"... so a filter like
 * `esp_log_level_set("t5.*")` style tooling can group them.
 */
#pragma once

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define T5_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define T5_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define T5_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define T5_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
extern int t5_log_host_verbose;
#define T5_LOG_HOST(lvl, tag, fmt, ...) \
    do { if (t5_log_host_verbose) fprintf(stderr, lvl " (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define T5_LOGE(tag, fmt, ...) T5_LOG_HOST("E", tag, fmt, ##__VA_ARGS__)
#define T5_LOGW(tag, fmt, ...) T5_LOG_HOST("W", tag, fmt, ##__VA_ARGS__)
#define T5_LOGI(tag, fmt, ...) T5_LOG_HOST("I", tag, fmt, ##__VA_ARGS__)
#define T5_LOGD(tag, fmt, ...) T5_LOG_HOST("D", tag, fmt, ##__VA_ARGS__)
#endif
