/* SPDX-License-Identifier: Apache-2.0
 * t5_ports internals: the port table and the pin broker.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_ports/ports.h"

#define PORTS_GPIO_MAX 55       /* ESP32-P4: GPIO0..54 */

/* ---- port table (ports_table.c) ----------------------------------------- */
typedef struct {
    const char *name;
    const char *desc;
    uint8_t npins;
    uint8_t gpio[T5_PORT_MAX_PINS];
    const char *label[T5_PORT_MAX_PINS];
    uint32_t modes;             /* 1u << t5_port_mode_t */
    int8_t i2c[2];              /* pin index: sda, scl */
    int8_t uart[3];             /* pin index: tx, rx, rts (-1 none) */
    int8_t spi[3];              /* pin index: sck, mosi, miso */
    bool shared;                /* routed through t5_i2c, claims nothing */
} port_def_t;

#define PM(m) (1u << (m))

int ports_table_count(void);
const port_def_t *ports_table_get(int i);
const port_def_t *ports_table_find(const char *name);
bool ports_pin_risky(uint8_t gpio);
/* ADC unit (1, 2) and channel for a GPIO; false if it has none. */
bool ports_pin_adc(uint8_t gpio, int *unit, int *channel);
/* Why a risky pin is risky, for error strings. */
const char *ports_pin_risk(uint8_t gpio);

/* ---- broker (broker.c), pure claim bookkeeping, caller holds the lock ---- */
typedef struct {
    bool claimed;
    char owner[T5_PORT_OWNER_LEN];
    const char *port;
    t5_port_mode_t mode;
    const void *handle;
} broker_pin_t;

/* Claim every pin or none. Checks risky pins (T5_ERR_DENIED without
 * T5_PORT_FLAG_RISKY), then conflicts (T5_ERR_BUSY naming the holder). */
t5_err_t broker_claim(const uint8_t *gpio, int n, uint32_t flags, const char *owner, const char *port,
                      t5_port_mode_t mode, const void *handle, char *why, size_t whylen);
/* Free every pin held by handle. Returns how many. */
int broker_release(const void *handle);
const broker_pin_t *broker_pin(uint8_t gpio);
void broker_reset(void);

/* For tests. */
void t5_ports_reset_for_test(void);
