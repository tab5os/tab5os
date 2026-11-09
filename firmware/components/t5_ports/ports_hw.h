/* SPDX-License-Identifier: Apache-2.0
 * Hardware layer under t5_ports. Device: ports_hw.c (ESP-IDF drivers).
 * Host tests: test/host/mocks/ports_hw_mock.c.
 *
 * Pins are GPIO numbers; the logic layer has already claimed them and keeps
 * away from the ones that must not be touched (ports_pin_hands_off). Handles
 * are opaque driver objects.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

typedef struct ports_hw_obj *ports_hw_t;

/* I2C master on a controller other than the system bus. */
t5_err_t ports_hw_i2c_open(int sda, int scl, uint32_t hz, ports_hw_t *out);
void ports_hw_i2c_close(ports_hw_t h);
t5_err_t ports_hw_i2c_xfer(ports_hw_t h, uint8_t addr, const uint8_t *tx, size_t txlen, uint8_t *rx,
                           size_t rxlen, uint32_t timeout_ms);
t5_err_t ports_hw_i2c_probe(ports_hw_t h, uint8_t addr, uint32_t timeout_ms);

typedef struct {
    int tx, rx, rts;            /* rts >= 0: RS-485 half duplex, RTS drives DE */
    uint32_t baud;
    uint8_t data_bits, parity, stop_bits;
    size_t rx_buf;
} ports_hw_uart_cfg_t;

t5_err_t ports_hw_uart_open(const ports_hw_uart_cfg_t *cfg, ports_hw_t *out);
void ports_hw_uart_close(ports_hw_t h);
int ports_hw_uart_write(ports_hw_t h, const void *data, size_t n);
int ports_hw_uart_read(ports_hw_t h, void *data, size_t n, uint32_t timeout_ms);
t5_err_t ports_hw_uart_flush(ports_hw_t h);
t5_err_t ports_hw_uart_wait_tx(ports_hw_t h, uint32_t timeout_ms);

t5_err_t ports_hw_spi_open(int sck, int mosi, int miso, int cs, uint32_t hz, uint8_t mode, ports_hw_t *out);
void ports_hw_spi_close(ports_hw_t h);
t5_err_t ports_hw_spi_transfer(ports_hw_t h, const void *tx, void *rx, size_t n);

t5_err_t ports_hw_gpio_config(int pin, bool output, bool pull_up, bool pull_down, bool open_drain);
t5_err_t ports_hw_gpio_set(int pin, bool level);
/* Current input level, or -1 if the pin can't be read. */
int ports_hw_gpio_get(int pin);

t5_err_t ports_hw_pwm_open(int pin, uint32_t hz, uint8_t bits, ports_hw_t *out);
void ports_hw_pwm_close(ports_hw_t h);
t5_err_t ports_hw_pwm_set(ports_hw_t h, uint32_t duty_permille);

/* unit 1 or 2, channel per the SoC table. */
t5_err_t ports_hw_adc_open(int pin, int unit, int channel, ports_hw_t *out);
void ports_hw_adc_close(ports_hw_t h);
t5_err_t ports_hw_adc_read_mv(ports_hw_t h, int *mv);

/* Back to a quiet input (gpio_reset_pin). */
void ports_hw_pin_release(int pin);
