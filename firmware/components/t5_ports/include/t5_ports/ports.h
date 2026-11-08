/* SPDX-License-Identifier: Apache-2.0
 * External ports (plan 5.1). Part of the app SDK.
 *
 * Apps address connectors by name, never by pin number:
 *
 *   grove       HY2.0-4P             I2C, UART, GPIO, PWM, ADC
 *   mbus.spi    M5-Bus SPI           SPI                      (pins shared with stamp)
 *   mbus.uart   M5-Bus UART          UART                     (G37/G38: risky)
 *   mbus.debug  M5-Bus debug UART    UART                     (G6 shared with stamp)
 *   mbus.i2c    M5-Bus I2C           I2C, through the system bus; internal addresses refused
 *   mbus.g2 ... M5-Bus single pins   GPIO, PWM, ADC where the pin has a channel
 *   ext         2.54 mm 10-pin       GPIO, UART, I2C, PWM, ADC
 *   rs485       HC1.25-6P            RS-485 half duplex (see modbus.h)
 *   stamp       Stamp pads           SPI, UART, GPIO          (pins shared with mbus.spi)
 *
 * Rules:
 *  - Claims are per pin and exclusive. Opening a port claims the pins the
 *    mode needs (GPIO, PWM and ADC claim one pin, cfg->pin_index). A conflict
 *    returns T5_ERR_BUSY and t5_port_open_ex() explains it, for example
 *    "G18 held by com.x.app (mbus.spi)". Ports that share pins (stamp,
 *    mbus.spi, mbus.debug) conflict naturally.
 *  - Risky pins (G35 power key, G37/G38 strapping pins) need
 *    T5_PORT_FLAG_RISKY, else T5_ERR_DENIED. G35 is input-only.
 *  - Handles opened by an app are released when it exits or crashes.
 *  - mbus.i2c is the internal bus where it reaches the M5-Bus. It is shared,
 *    claims no pins, and refuses the internal chips' addresses.
 *  - Switched 5 V for grove, ext and the M5-Bus is a separate power rail
 *    (t5_rail_acquire(T5_RAIL_EXT5V)); opening a port doesn't power it.
 *
 * Handles are not thread-safe against t5_port_close(): don't close a handle
 * another task is using.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_MODE_I2C,
    T5_MODE_UART,
    T5_MODE_SPI,
    T5_MODE_GPIO,
    T5_MODE_PWM,
    T5_MODE_ADC,
    T5_MODE_RS485,
    T5_MODE_COUNT,
} t5_port_mode_t;

typedef struct t5_port t5_port_t;

/* cfg for each mode. NULL cfg means the defaults noted here. */
typedef struct {
    uint32_t hz;                /* default 100000 */
} t5_port_i2c_cfg_t;

typedef struct {
    uint32_t baud;              /* default 115200 */
    uint8_t data_bits;          /* 5..8, default 8 */
    uint8_t parity;             /* 0 none, 1 odd, 2 even */
    uint8_t stop_bits;          /* 1 or 2, default 1 */
    size_t rx_buf;              /* receive ring buffer, default 1024 */
} t5_port_uart_cfg_t;           /* also used by T5_MODE_RS485 */

typedef struct {
    uint32_t hz;                /* default 1000000 */
    uint8_t mode;               /* SPI mode 0..3 */
    int cs_pin_index;           /* hardware CS on this port pin, -1 none (stamp: 0 = G33) */
} t5_port_spi_cfg_t;

typedef struct {
    uint8_t pin_index;          /* pin within the port; 0 for single-pin ports */
    bool output;
    bool pull_up, pull_down, open_drain;
} t5_port_gpio_cfg_t;

typedef struct {
    uint8_t pin_index;
    uint32_t hz;                /* default 1000 */
    uint8_t resolution_bits;    /* default 10 */
} t5_port_pwm_cfg_t;

typedef struct {
    uint8_t pin_index;
} t5_port_adc_cfg_t;

#define T5_PORT_FLAG_RISKY 0x1u /* allow strapping and power-key pins */

#define T5_PORT_WHY_LEN 64      /* enough for any error string */

/* Open a port. cfg points to the struct for mode (or NULL for defaults). */
T5_MUST_CHECK t5_err_t t5_port_open(const char *name, t5_port_mode_t mode, const void *cfg, t5_port_t **out);
/* As t5_port_open, with flags and a reason on failure (why may be NULL). */
T5_MUST_CHECK t5_err_t t5_port_open_ex(const char *name, t5_port_mode_t mode, const void *cfg, uint32_t flags,
                         t5_port_t **out, char *why, size_t whylen);
/* Release the pins and the driver. NULL or an already closed handle is ignored. */
T5_MUST_CHECK t5_err_t t5_port_close(t5_port_t *p);

const char *t5_port_name(const t5_port_t *p);
t5_port_mode_t t5_port_mode(const t5_port_t *p);

/* ---- I2C (7-bit addresses) ---------------------------------------------- */
T5_MUST_CHECK t5_err_t t5_port_i2c_read_reg(t5_port_t *p, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);
T5_MUST_CHECK t5_err_t t5_port_i2c_write_reg(t5_port_t *p, uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len);
/* Write tx then read rx with a repeated start. Either side may be empty. */
T5_MUST_CHECK t5_err_t t5_port_i2c_xfer(t5_port_t *p, uint8_t addr, const uint8_t *tx, size_t txlen, uint8_t *rx,
                          size_t rxlen);
/* T5_OK if something ACKs, T5_ERR_NACK if not. */
T5_MUST_CHECK t5_err_t t5_port_i2c_probe(t5_port_t *p, uint8_t addr);
/* Probe 0x08..0x77 (reserved ones skipped on mbus.i2c). Returns the count found. */
int t5_port_i2c_scan(t5_port_t *p, uint8_t *found, size_t max);

/* ---- UART and RS-485 ---------------------------------------------------- */
/* Return bytes written/read, or a negative t5_err_t. */
int t5_port_uart_write(t5_port_t *p, const void *data, size_t n);
int t5_port_uart_read(t5_port_t *p, void *data, size_t n, uint32_t timeout_ms);
/* Drop anything received and not read yet. */
T5_MUST_CHECK t5_err_t t5_port_uart_flush(t5_port_t *p);
/* Wait until everything written has left the wire (RS-485: before turnaround). */
T5_MUST_CHECK t5_err_t t5_port_uart_wait_tx(t5_port_t *p, uint32_t timeout_ms);
uint32_t t5_port_uart_baud(const t5_port_t *p);

/* ---- SPI (full duplex; tx or rx may be NULL) ---------------------------- */
T5_MUST_CHECK t5_err_t t5_port_spi_transfer(t5_port_t *p, const void *tx, void *rx, size_t n);

/* ---- GPIO --------------------------------------------------------------- */
T5_MUST_CHECK t5_err_t t5_port_gpio_set(t5_port_t *p, bool level);
T5_MUST_CHECK t5_err_t t5_port_gpio_get(t5_port_t *p, bool *level);

/* ---- PWM ---------------------------------------------------------------- */
T5_MUST_CHECK t5_err_t t5_port_pwm_set(t5_port_t *p, uint32_t duty_permille);

/* ---- ADC (calibrated, 0..~3300 mV) -------------------------------------- */
T5_MUST_CHECK t5_err_t t5_port_adc_read_mv(t5_port_t *p, int *mv);

/* ---- Enumeration (Port Inspector, console) ------------------------------ */
#define T5_PORT_MAX_PINS 6
#define T5_PORT_OWNER_LEN 24

typedef struct {
    const char *name;
    const char *desc;
    uint8_t npins;
    uint8_t gpio[T5_PORT_MAX_PINS];
    const char *label[T5_PORT_MAX_PINS];  /* "SDA", "MOSI", "CS"... */
    uint32_t modes;             /* bit (1u << t5_port_mode_t) per supported mode */
    uint8_t risky_mask;         /* bit per pin index */
    uint8_t adc_mask;           /* bit per pin index with an ADC channel */
    bool shared;                /* mbus.i2c: system bus, no pin claims */
} t5_port_info_t;

typedef struct {
    uint8_t gpio;
    bool claimed;
    bool risky;
    char owner[T5_PORT_OWNER_LEN];  /* "" if free, "system" for the shared bus */
    const char *port;           /* port holding it, NULL if free */
    t5_port_mode_t mode;        /* valid if port != NULL */
    int level;                  /* 0, 1, or -1 if unreadable */
} t5_pin_info_t;

/* Must run once at boot before any other task opens a port (idempotent). */
T5_MUST_CHECK t5_err_t t5_ports_init(void);

int t5_ports_count(void);
T5_MUST_CHECK t5_err_t t5_ports_info(int i, t5_port_info_t *out);
T5_MUST_CHECK t5_err_t t5_ports_find(const char *name, t5_port_info_t *out);
/* Every GPIO used by any port, ascending. Returns the count (may exceed max). */
int t5_ports_pins(uint8_t *gpio, int max);
/* Owner, port, mode and level of one GPIO. T5_ERR_NOT_FOUND if no port uses it. */
T5_MUST_CHECK t5_err_t t5_ports_pin_info(uint8_t gpio, t5_pin_info_t *out);

const char *t5_port_mode_name(t5_port_mode_t mode);   /* "i2c", "uart"... */
/* Parse a mode name; returns T5_MODE_COUNT if unknown. */
t5_port_mode_t t5_port_mode_parse(const char *s);

#ifdef __cplusplus
}
#endif
