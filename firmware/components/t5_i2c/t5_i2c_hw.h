/* SPDX-License-Identifier: Apache-2.0
 * Hardware layer under t5_i2c. Device: t5_i2c_hw.c (ESP-IDF i2c_master).
 * Host tests: test/host/mocks/t5_i2c_hw_mock.c (simulated chips, fault injection).
 * Return values: T5_OK, T5_ERR_NACK, T5_ERR_TIMEOUT, T5_ERR_IO.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

typedef struct t5_i2c_hw_dev *t5_i2c_hw_dev_t;

t5_err_t t5_i2c_hw_init(void);
t5_err_t t5_i2c_hw_add(uint8_t addr, uint32_t hz, t5_i2c_hw_dev_t *out);
void t5_i2c_hw_remove(t5_i2c_hw_dev_t dev);
t5_err_t t5_i2c_hw_xfer(t5_i2c_hw_dev_t dev, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen,
                        uint32_t timeout_ms);
t5_err_t t5_i2c_hw_probe(uint8_t addr, uint32_t timeout_ms);
/* Reset the controller and clear the bus. */
t5_err_t t5_i2c_hw_recover(void);
