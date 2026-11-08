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
 * Received frames stay in those buffers until lwIP and the reading app are
 * done with them, so a busy download pins tens of KB of internal RAM, and
 * once internal RAM runs short (the camera open, ~74 KB free) allocations
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
