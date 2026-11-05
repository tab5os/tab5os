/* SPDX-License-Identifier: Apache-2.0 */
#include "driver/gpio.h"
#include "t5_bsp/board.h"
#include "t5_pins/pins.h"

t5_err_t t5_bsp_board_init(void)
{
    return gpio_sleep_sel_dis(T5_PIN_PWR_KEY) == ESP_OK ? T5_OK : T5_ERR_IO;
}
