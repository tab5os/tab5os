/* SPDX-License-Identifier: Apache-2.0
 * t5_i2c hardware layer on the ESP-IDF i2c_master driver.
 */
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "t5_i2c_hw.h"
#include "t5_pins/pins.h"

static i2c_master_bus_handle_t s_bus;

static t5_err_t map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return T5_OK;
    case ESP_ERR_TIMEOUT: return T5_ERR_TIMEOUT;
    case ESP_ERR_INVALID_RESPONSE:   /* NACK during a transfer */
    case ESP_ERR_NOT_FOUND:          /* NACK on probe */
        return T5_ERR_NACK;
    case ESP_ERR_NO_MEM: return T5_ERR_NO_MEM;
    default: return T5_ERR_IO;       /* ESP_ERR_INVALID_STATE: FSM wedged, etc. */
    }
}

t5_err_t t5_i2c_hw_init(void)
{
    if (s_bus) {
        return T5_OK;
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = T5_PIN_SYS_I2C_SDA,
        .scl_io_num = T5_PIN_SYS_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    /* t5_i2c keeps its own per-device counters; the driver's NACK logs are noise. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    return map_err(i2c_new_master_bus(&cfg, &s_bus));
}

t5_err_t t5_i2c_hw_add(uint8_t addr, uint32_t hz, t5_i2c_hw_dev_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = hz,
    };
    i2c_master_dev_handle_t h;
    t5_err_t err = map_err(i2c_master_bus_add_device(s_bus, &cfg, &h));
    if (err == T5_OK) {
        *out = (t5_i2c_hw_dev_t)h;
    }
    return err;
}

void t5_i2c_hw_remove(t5_i2c_hw_dev_t dev)
{
    i2c_master_bus_rm_device((i2c_master_dev_handle_t)dev);
}

t5_err_t t5_i2c_hw_xfer(t5_i2c_hw_dev_t dev, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen,
                        uint32_t timeout_ms)
{
    i2c_master_dev_handle_t h = (i2c_master_dev_handle_t)dev;
    esp_err_t e;
    if (txlen && rxlen) {
        e = i2c_master_transmit_receive(h, tx, txlen, rx, rxlen, timeout_ms);
    } else if (txlen) {
        e = i2c_master_transmit(h, tx, txlen, timeout_ms);
    } else {
        e = i2c_master_receive(h, rx, rxlen, timeout_ms);
    }
    return map_err(e);
}

t5_err_t t5_i2c_hw_probe(uint8_t addr, uint32_t timeout_ms)
{
    return map_err(i2c_master_probe(s_bus, addr, timeout_ms));
}

t5_err_t t5_i2c_hw_recover(void)
{
    /* Controller FSM reset, then hardware bus clear: 9 SCL pulses and a STOP. */
    return map_err(i2c_master_bus_reset(s_bus));
}
