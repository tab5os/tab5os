/* SPDX-License-Identifier: Apache-2.0
 * The log ring (logring.h): esp_log's vprintf hook copies each line here
 * before printing it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "t5_core/logring.h"

#define RING T5_LOG_RING_BYTES
EXT_RAM_BSS_ATTR static char s_ring[RING];
static uint64_t s_head;          /* bytes ever written */
static vprintf_like_t s_prev;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int capture(const char *fmt, va_list ap)
{
    char line[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > 0) {
        if (n > (int)sizeof line - 1) n = sizeof line - 1;
        taskENTER_CRITICAL(&s_mux);
        for (int i = 0; i < n; i++) s_ring[(s_head + (uint64_t)i) % RING] = line[i];
        s_head += (uint64_t)n;
        taskEXIT_CRITICAL(&s_mux);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void t5_log_capture_start(void)
{
    if (!s_prev) s_prev = esp_log_set_vprintf(capture);
}

uint64_t t5_log_head(void)
{
    taskENTER_CRITICAL(&s_mux);
    uint64_t h = s_head;
    taskEXIT_CRITICAL(&s_mux);
    return h;
}

size_t t5_log_read(uint64_t *pos, char *buf, size_t n)
{
    taskENTER_CRITICAL(&s_mux);
    uint64_t head = s_head;
    uint64_t oldest = head > RING ? head - RING : 0;
    if (*pos < oldest) *pos = oldest;
    if (*pos > head) *pos = head;
    size_t avail = (size_t)(head - *pos);
    if (avail > n) avail = n;
    for (size_t i = 0; i < avail; i++) buf[i] = s_ring[(*pos + i) % RING];
    *pos += avail;
    taskEXIT_CRITICAL(&s_mux);
    return avail;
}
