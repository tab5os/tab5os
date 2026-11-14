/* SPDX-License-Identifier: Apache-2.0
 * esp_hosted 3.1 host against the Tab5's factory C6 firmware (1.4.1).
 *
 * The host sizes its SDIO streaming RX staging buffers from what the
 * co-processor advertises at INIT, and then refuses any read bigger than
 * that ("SDIO RX read (2048 bytes) exceeds staging target (1536 bytes)").
 * The factory C6 firmware advertises neither its SDIO mode nor its TX queue
 * depth, so the host settles on one 1536-byte frame, while the C6 streams
 * reads of several frames. The first larger read is skipped, the stream
 * loses sync, and Wi-Fi data and RPCs stop until `net reset` (2026-11-06/07).
 *
 * When the target is a single frame or less, keep the staging buffers
 * dynamic instead (the host's own behaviour when the queue depth is
 * unknown): they grow to the largest read. A co-processor that advertises a
 * real capacity (an updated C6, `t5 c6-stage`) passes through unchanged.
 * The linker wraps the call (CMakeLists.txt).
 */
#include <stdint.h>
#include "esp_log.h"

#define ONE_FRAME 1536u      /* MAX_SDIO_BUFFER_SIZE */

void __real_eh_host_bus_sdio_set_rx_staging_target(uint32_t target_size);

void __wrap_eh_host_bus_sdio_set_rx_staging_target(uint32_t target_size)
{
    if (target_size && target_size <= ONE_FRAME) {
        ESP_LOGI("t5.net", "C6 advertised no SDIO RX capacity: staging buffers stay dynamic");
        target_size = 0;
    }
    __real_eh_host_bus_sdio_set_rx_staging_target(target_size);
}

/* ---- packet buffers in PSRAM ------------------------------------------------
 * Every Wi-Fi frame in either direction gets its own 1536-byte buffer from
 * eh_host_port_dma_alloc(), which esp_hosted takes from internal DMA RAM.
 * A busy download keeps many of them in flight (received frames are then
 * copied for lwIP: see "received frames in PSRAM" below), and once internal
 * RAM runs short (the camera open, ~74 KB free) allocations
 * fail and frames are dropped ("eh_sdio mempool OOM", the stream stalls).
 *
 * The P4's SDMMC host DMAs to and from PSRAM (SOC_SDMMC_PSRAM_DMA_CAPABLE;
 * the driver syncs the cache) as long as the buffer and its length are
 * cache-line aligned, so take them from PSRAM, aligned, and fall back to
 * internal RAM. CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM does the same for the
 * host's aligned buffers (RX staging, TX aggregation). */
#include <stddef.h>
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"

void *__real_eh_host_port_dma_alloc(size_t n);

void *__wrap_eh_host_port_dma_alloc(size_t n)
{
    size_t align = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align) != ESP_OK || align < 4) align = 64;
    size_t sz = (n + align - 1) & ~(align - 1);
    void *p = heap_caps_aligned_alloc(align, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    return p ? p : __real_eh_host_port_dma_alloc(n);
}

/* ---- received frames in PSRAM --------------------------------------------------
 * The SDIO RX task doesn't hand lwIP the DMA buffer above: it copies each
 * Wi-Fi frame into a plain malloc() (eh_host_bus_sdio.c, "TODO: abstract
 * heap_caps_malloc") and frees the DMA buffer. A frame is under
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4 KB), so the copy is internal RAM,
 * and lwIP keeps it (zero copy, esp_pbuf_ref.c) until the app reads the data.
 * Unread TCP data, up to the receive window, and out-of-order segments
 * therefore sat in internal RAM: one socket left unread took free internal
 * RAM from 57 KB to under 4 KB with the 32 KB window (2026-11-14), and Radio
 * was closed for memory after a backlog built up while playing.
 *
 * Every frame reaches lwIP through esp_wifi_remote_channel_rx() (called from
 * esp_hosted's wifi_remote glue, a separate object, so the linker can wrap
 * it): move an internal copy to PSRAM there and free the internal one at
 * once. lwIP frees the buffer through esp_wifi_internal_free_rx_buffer(),
 * which is free(), so either heap works. If PSRAM is short the frame goes on
 * as it is. */
#include <string.h>
#include "esp_memory_utils.h"

esp_err_t __real_esp_wifi_remote_channel_rx(void *h, void *buffer, void *buff_to_free, size_t len);

esp_err_t __wrap_esp_wifi_remote_channel_rx(void *h, void *buffer, void *buff_to_free, size_t len)
{
    if (buffer && buffer == buff_to_free && len && esp_ptr_internal(buffer)) {
        void *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            memcpy(p, buffer, len);
            free(buffer);
            buffer = buff_to_free = p;
        }
    }
    return __real_esp_wifi_remote_channel_rx(h, buffer, buff_to_free, len);
}
