/* SPDX-License-Identifier: Apache-2.0 */
#include "driver/gpio.h"
#include "t5_bsp/board.h"
#include "t5_bsp/xpin.h"
#include "t5_core/log.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/pins.h"

static const char *TAG = "t5.bsp";

/* G35 is shared with the PMS150G power controller. If its pad config changes
 * in sleep (IDF isolates pads by default), the controller can read a key
 * press and cut power. Keep its active config through sleep. (bench 0.8) */
static t5_err_t pads_init(void)
{
    return gpio_sleep_sel_dis(T5_PIN_PWR_KEY) == ESP_OK ? T5_OK : T5_ERR_IO;
}

static void step(const char *what, t5_err_t e, t5_err_t *first)
{
    if (e == T5_OK) return;
    T5_LOGE(TAG, "bring-up: %s failed: %s", what, t5_err_str(e));
    if (*first == T5_OK) *first = e;
}

t5_err_t t5_bsp_bringup(void)
{
    t5_err_t first = T5_OK;
    step("pads", pads_init(), &first);
    step("i2c", t5_i2c_init(), &first);
    step("expanders", t5_xpin_init(), &first);
    step("board defaults", t5_xpin_apply_boot_defaults(), &first);
    return first;
}
