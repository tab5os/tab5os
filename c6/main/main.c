/* SPDX-License-Identifier: Apache-2.0
 * Tab5 OS radio firmware (plan 3.1). The esp_hosted coprocessor does the
 * work; this adds what Tab5 OS needs on top:
 *
 *  - Update safety (plan 3.5): with bootloader rollback on, a new image is
 *    only marked valid once the P4 has reached it over SDIO and its RPC
 *    endpoints are up. If it never gets that far, the next reset (t5_net
 *    power-cycles the C6) boots the previous image.
 *  - Host wake (plan 3.4, bench check 12): esp_hosted's host power-save
 *    feature drives IO2, wired to P4 G14, configured in sdkconfig.defaults.
 */
#include "eh_cp_event.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

static const char *TAG = "t5_c6";

static void on_cp_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id != EH_CP_EVT_PRIVATE_RPC_READY) return;
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "host reached us: marking this image valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void app_main(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "Tab5 OS radio firmware %s", d->version);
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_handler_register(EH_CP_EVENT, ESP_EVENT_ANY_ID, on_cp_event, NULL));
}
