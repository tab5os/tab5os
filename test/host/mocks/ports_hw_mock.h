/* SPDX-License-Identifier: Apache-2.0
 * State of the t5_ports hardware mock (ports_hw_mock.c), for test_ports.c.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

/* Kinds of driver object, indexing mock_ports.live. */
typedef enum { MOCK_I2C, MOCK_UART, MOCK_SPI, MOCK_PWM, MOCK_ADC, MOCK_KINDS } mock_kind_t;

typedef struct {
    bool i2c_present[128];
    uint8_t i2c_last_tx[8];
    int live[MOCK_KINDS];       /* driver objects open, by kind */
    int opens;
    t5_err_t fail_open;         /* next driver open returns this */
    int released[64];           /* release count per GPIO */
    int configured[64];
    bool out[64];
    int level[64];
    int rts;
    uint32_t baud;
    uint8_t tx[512];
    size_t ntx;
    uint8_t rx[512];
    size_t nrx, rxpos;
    uint8_t reply[512];
    size_t nreply;
    int flushes;
    uint32_t duty;
    int mv;
} mock_ports_t;

extern mock_ports_t mock_ports;
void mock_ports_reset(void);
/* The next uart write is answered with these bytes. */
void mock_ports_uart_reply(const uint8_t *d, size_t n);
