/* SPDX-License-Identifier: Apache-2.0
 * Fix for ESP-IDF v6.1's vStreamBufferGenericDeleteWithCaps().
 *
 * IDF deletes a stream buffer made by xStreamBufferCreateWithCaps() with
 * vSemaphoreDelete(), i.e. vQueueDelete() on a stream buffer. vQueueDelete
 * reads the queue's "statically allocated" flag from whatever byte of the
 * StaticStreamBuffer_t lies there; when it reads 0 it frees the struct,
 * and IDF then frees it again with heap_caps_free(): an intermittent
 * "block already marked as free" assert (mic and camera recordings,
 * 2026-11-07). Here: vStreamBufferDelete(), which leaves static buffers
 * alone, then free both blocks once. The linker wraps the call (CMakeLists).
 */
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/stream_buffer.h"

void __wrap_vStreamBufferGenericDeleteWithCaps(StreamBufferHandle_t sb, BaseType_t is_message)
{
    StaticStreamBuffer_t *st = NULL;
    uint8_t *store = NULL;
    BaseType_t ok = is_message ? xMessageBufferGetStaticBuffers(sb, &store, &st)
                               : xStreamBufferGetStaticBuffers(sb, &store, &st);
    configASSERT(ok == pdTRUE);
    (void)ok;
    vStreamBufferDelete(sb);          /* message buffers are stream buffers */
    heap_caps_free(st);
    heap_caps_free(store);
}
