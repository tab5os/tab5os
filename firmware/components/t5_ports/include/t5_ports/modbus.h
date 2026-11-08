/* SPDX-License-Identifier: Apache-2.0
 * Modbus RTU master over an rs485 port (plan 5.2). Part of the app SDK.
 *
 *   t5_port_t *bus;
 *   t5_port_uart_cfg_t uc = { .baud = 9600, .parity = 2 };
 *   t5_port_open("rs485", T5_MODE_RS485, &uc, &bus);
 *   t5_modbus_t mb;
 *   t5_modbus_init(&mb, bus);
 *   uint16_t regs[2];
 *   t5_err_t err = t5_modbus_read_holding(&mb, 1, 0x0000, 2, regs);
 *
 * Direction (G34) is driven by the UART in RS-485 half-duplex mode. The
 * master keeps 3.5 character times of silence between frames. A slave's
 * exception response returns an error and leaves the code in mb.exception:
 *   0x01 illegal function     -> T5_ERR_NOT_SUPPORTED
 *   0x02 illegal address,
 *   0x03 illegal value        -> T5_ERR_INVALID_ARG
 *   0x06 slave busy           -> T5_ERR_BUSY
 *   anything else             -> T5_ERR_IO
 * No reply -> T5_ERR_TIMEOUT; bad CRC or a malformed reply -> T5_ERR_IO.
 * Slave 0 is broadcast: writes only, no reply is awaited.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_ports/ports.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_MODBUS_FN_READ_COILS       0x01
#define T5_MODBUS_FN_READ_DISCRETE    0x02
#define T5_MODBUS_FN_READ_HOLDING     0x03
#define T5_MODBUS_FN_READ_INPUT       0x04
#define T5_MODBUS_FN_WRITE_COIL       0x05
#define T5_MODBUS_FN_WRITE_REGISTER   0x06
#define T5_MODBUS_FN_WRITE_REGISTERS  0x10

#define T5_MODBUS_MAX_FRAME  256
#define T5_MODBUS_MAX_READ   125    /* registers per read */
#define T5_MODBUS_MAX_WRITE  123    /* registers per write */
#define T5_MODBUS_MAX_BITS   2000   /* coils per read */

typedef struct {
    t5_port_t *port;
    uint32_t timeout_ms;    /* response timeout, default 300 */
    uint8_t exception;      /* last exception code, 0 if none */
    int64_t last_us;        /* end of the last frame, for the 3.5 char gap */
} t5_modbus_t;

void t5_modbus_init(t5_modbus_t *mb, t5_port_t *port);

/* Registers come back in host order. */
T5_MUST_CHECK t5_err_t t5_modbus_read_holding(t5_modbus_t *mb, uint8_t slave, uint16_t addr, uint16_t count, uint16_t *out);
T5_MUST_CHECK t5_err_t t5_modbus_read_input(t5_modbus_t *mb, uint8_t slave, uint16_t addr, uint16_t count, uint16_t *out);
T5_MUST_CHECK t5_err_t t5_modbus_write_register(t5_modbus_t *mb, uint8_t slave, uint16_t addr, uint16_t value);
T5_MUST_CHECK t5_err_t t5_modbus_write_registers(t5_modbus_t *mb, uint8_t slave, uint16_t addr, const uint16_t *vals,
                                   uint16_t count);
/* Coils: bits packed LSB first, (count + 7) / 8 bytes. */
T5_MUST_CHECK t5_err_t t5_modbus_read_coils(t5_modbus_t *mb, uint8_t slave, uint16_t addr, uint16_t count, uint8_t *bits);
T5_MUST_CHECK t5_err_t t5_modbus_write_coil(t5_modbus_t *mb, uint8_t slave, uint16_t addr, bool on);

/* ---- framing (pure, exposed for tests and tools) ------------------------ */
uint16_t t5_modbus_crc16(const uint8_t *data, size_t len);
/* Request for 0x01..0x04 (addr, count), 0x05/0x06 (addr, value). Returns
 * the frame length with CRC, or 0 if cap is too small. */
size_t t5_modbus_build_req(uint8_t *frame, size_t cap, uint8_t slave, uint8_t fn, uint16_t addr, uint16_t arg);
size_t t5_modbus_build_write_multi(uint8_t *frame, size_t cap, uint8_t slave, uint16_t addr,
                                   const uint16_t *vals, uint16_t count);
/* Length of a normal reply to fn with this count (0x05/0x06/0x10: 8). */
size_t t5_modbus_reply_len(uint8_t fn, uint16_t count);
/* Check a reply: slave, function, CRC, length. On an exception reply sets
 * *exception and returns its mapped error. data and dlen point at the payload
 * after the byte count (reads) or after the function code (writes). */
T5_MUST_CHECK t5_err_t t5_modbus_parse_reply(const uint8_t *rx, size_t n, uint8_t slave, uint8_t fn, uint8_t *exception,
                               const uint8_t **data, size_t *dlen);
T5_MUST_CHECK t5_err_t t5_modbus_exception_err(uint8_t code);
/* 3.5 character times in microseconds (11-bit chars; fixed 1750 us above 19200 baud). */
uint32_t t5_modbus_t35_us(uint32_t baud);

#ifdef __cplusplus
}
#endif
