/* SPDX-License-Identifier: Apache-2.0
 * System I2C bus service (G31/G32).
 *
 * One bus, one owner. Services open a device handle per chip; every transfer
 * goes through here so the bus can be locked, counted, traced and recovered.
 *
 *  - Recovery: on a timeout or bus error the controller is reset and the bus
 *    cleared (9 SCL pulses + STOP), then the transfer is retried once.
 *  - Fault isolation: after T5_I2C_FAULT_THRESHOLD consecutive failures a
 *    device is marked faulted, T5_EV_I2C_DEVICE_FAULT is posted, and calls fail
 *    fast (one real attempt every T5_I2C_FAULT_RETRY_MS) so a dead chip can't
 *    stall everyone else. A success clears the fault.
 *  - Reserved addresses: internal chips. The Phase 5 port broker refuses them
 *    for app handles on any bus that reaches them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_I2C_TIMEOUT_MS        50
#define T5_I2C_FAULT_THRESHOLD   8
#define T5_I2C_FAULT_RETRY_MS    2000
#define T5_I2C_TRACE_LEN         128
#define T5_I2C_MAX_DEVICES       24

typedef struct t5_i2c_dev t5_i2c_dev_t;

t5_err_t t5_i2c_init(void);

/* Open a handle. name is for diagnostics ("rtc", "touch"); must be static. */
t5_err_t t5_i2c_open(uint8_t addr, uint32_t hz, const char *name, t5_i2c_dev_t **out);
void t5_i2c_close(t5_i2c_dev_t *dev);
uint8_t t5_i2c_addr(const t5_i2c_dev_t *dev);

/* Write tx then read rx with a repeated start. Either side may be empty. */
t5_err_t t5_i2c_xfer(t5_i2c_dev_t *dev, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen);

/* 8-bit register helpers. */
t5_err_t t5_i2c_read(t5_i2c_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len);
t5_err_t t5_i2c_write(t5_i2c_dev_t *dev, uint8_t reg, const uint8_t *buf, size_t len);
t5_err_t t5_i2c_read_u8(t5_i2c_dev_t *dev, uint8_t reg, uint8_t *val);
t5_err_t t5_i2c_write_u8(t5_i2c_dev_t *dev, uint8_t reg, uint8_t val);
/* Read-modify-write under the bus lock: reg = (reg & ~mask) | (val & mask). */
t5_err_t t5_i2c_update_u8(t5_i2c_dev_t *dev, uint8_t reg, uint8_t mask, uint8_t val);

/* 16-bit big-endian register address (touch controllers). */
t5_err_t t5_i2c_read16(t5_i2c_dev_t *dev, uint16_t reg, uint8_t *buf, size_t len);
t5_err_t t5_i2c_write16(t5_i2c_dev_t *dev, uint16_t reg, const uint8_t *buf, size_t len);

/* Does anything ACK at addr? Doesn't need a handle. */
t5_err_t t5_i2c_probe(uint8_t addr);

/* Hold the bus across several transfers (e.g. a multi-step init). Recursive. */
t5_err_t t5_i2c_lock(uint32_t timeout_ms);
void t5_i2c_unlock(void);

/* Force a bus recovery now (console, stress test). */
t5_err_t t5_i2c_recover(void);

/* Called after every recovery, with the bus lock held, so state that a
 * glitch may have reset (expander outputs) can be re-applied. */
typedef void (*t5_i2c_recover_cb_t)(void *arg);
t5_err_t t5_i2c_on_recover(t5_i2c_recover_cb_t cb, void *arg);

/* Internal chips. Apps never get a handle to these on the system bus. */
bool t5_i2c_addr_reserved(uint8_t addr);

/* ---- Diagnostics -------------------------------------------------------- */
typedef struct {
    uint8_t addr;
    const char *name;
    uint32_t xfers;
    uint32_t errors;
    uint32_t nacks;
    uint32_t timeouts;
    uint32_t consecutive;
    bool faulted;
    int32_t last_err;
} t5_i2c_dev_stats_t;

typedef struct {
    uint32_t xfers;
    uint32_t errors;
    uint32_t recoveries;
    uint32_t recovery_failures;
    uint32_t lock_timeouts;
} t5_i2c_bus_stats_t;

void t5_i2c_bus_stats(t5_i2c_bus_stats_t *out);
/* Fills up to max entries, returns the number of open devices. */
int t5_i2c_dev_stats(t5_i2c_dev_stats_t *out, int max);
/* Clear a fault by hand (e.g. after re-powering the chip). */
void t5_i2c_clear_fault(t5_i2c_dev_t *dev);

typedef struct {
    uint32_t t_ms;
    uint8_t addr;
    uint8_t txlen;     /* clipped to 255 */
    uint8_t rxlen;
    uint8_t first;     /* first tx byte, usually the register */
    int8_t err;        /* t5_err_t */
} t5_i2c_trace_t;

void t5_i2c_trace_enable(bool on);
/* Copies the newest entries, oldest first. Returns the count. */
int t5_i2c_trace_read(t5_i2c_trace_t *out, int max);

#ifdef __cplusplus
}
#endif
