/* SPDX-License-Identifier: Apache-2.0
 * Modbus RTU master (plan 5.2). Framing is pure; transactions go through the
 * port UART calls, so host tests drive both with the mock UART.
 */
#include <string.h>
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_ports/modbus.h"

static const char *TAG = "t5.modbus";

#define DEFAULT_TIMEOUT_MS 300

uint16_t t5_modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

static size_t put_crc(uint8_t *f, size_t n)
{
    uint16_t crc = t5_modbus_crc16(f, n);
    f[n] = (uint8_t)(crc & 0xFF);
    f[n + 1] = (uint8_t)(crc >> 8);
    return n + 2;
}

size_t t5_modbus_build_req(uint8_t *frame, size_t cap, uint8_t slave, uint8_t fn, uint16_t addr, uint16_t arg)
{
    if (!frame || cap < 8) {
        return 0;
    }
    frame[0] = slave;
    frame[1] = fn;
    frame[2] = (uint8_t)(addr >> 8);
    frame[3] = (uint8_t)addr;
    frame[4] = (uint8_t)(arg >> 8);
    frame[5] = (uint8_t)arg;
    return put_crc(frame, 6);
}

size_t t5_modbus_build_write_multi(uint8_t *frame, size_t cap, uint8_t slave, uint16_t addr,
                                   const uint16_t *vals, uint16_t count)
{
    size_t n = 7 + 2u * count + 2;
    if (!frame || !vals || !count || count > T5_MODBUS_MAX_WRITE || cap < n) {
        return 0;
    }
    frame[0] = slave;
    frame[1] = T5_MODBUS_FN_WRITE_REGISTERS;
    frame[2] = (uint8_t)(addr >> 8);
    frame[3] = (uint8_t)addr;
    frame[4] = (uint8_t)(count >> 8);
    frame[5] = (uint8_t)count;
    frame[6] = (uint8_t)(2 * count);
    for (uint16_t i = 0; i < count; i++) {
        frame[7 + 2 * i] = (uint8_t)(vals[i] >> 8);
        frame[8 + 2 * i] = (uint8_t)vals[i];
    }
    return put_crc(frame, 7 + 2u * count);
}

size_t t5_modbus_reply_len(uint8_t fn, uint16_t count)
{
    switch (fn) {
    case T5_MODBUS_FN_READ_COILS:
    case T5_MODBUS_FN_READ_DISCRETE:
        return 5 + (count + 7u) / 8;
    case T5_MODBUS_FN_READ_HOLDING:
    case T5_MODBUS_FN_READ_INPUT:
        return 5 + 2u * count;
    case T5_MODBUS_FN_WRITE_COIL:
    case T5_MODBUS_FN_WRITE_REGISTER:
    case T5_MODBUS_FN_WRITE_REGISTERS:
        return 8;
    default:
        return 0;
    }
}

t5_err_t t5_modbus_exception_err(uint8_t code)
{
    switch (code) {
    case 0x01: return T5_ERR_NOT_SUPPORTED;
    case 0x02:
    case 0x03: return T5_ERR_INVALID_ARG;
    case 0x06: return T5_ERR_BUSY;
    default: return T5_ERR_IO;
    }
}

t5_err_t t5_modbus_parse_reply(const uint8_t *rx, size_t n, uint8_t slave, uint8_t fn, uint8_t *exception,
                               const uint8_t **data, size_t *dlen)
{
    if (exception) *exception = 0;
    if (!rx || n < 5) {
        return T5_ERR_IO;
    }
    uint16_t crc = (uint16_t)(rx[n - 2] | (rx[n - 1] << 8));
    if (crc != t5_modbus_crc16(rx, n - 2) || rx[0] != slave) {
        return T5_ERR_IO;
    }
    if (rx[1] == (fn | 0x80)) {
        if (n != 5) {
            return T5_ERR_IO;
        }
        if (exception) *exception = rx[2];
        return t5_modbus_exception_err(rx[2]);
    }
    if (rx[1] != fn) {
        return T5_ERR_IO;
    }
    const uint8_t *d;
    size_t dl;
    switch (fn) {
    case T5_MODBUS_FN_READ_COILS:
    case T5_MODBUS_FN_READ_DISCRETE:
    case T5_MODBUS_FN_READ_HOLDING:
    case T5_MODBUS_FN_READ_INPUT:
        if (n != 5u + rx[2]) {
            return T5_ERR_IO;
        }
        d = rx + 3;
        dl = rx[2];
        break;
    case T5_MODBUS_FN_WRITE_COIL:
    case T5_MODBUS_FN_WRITE_REGISTER:
    case T5_MODBUS_FN_WRITE_REGISTERS:
        if (n != 8) {
            return T5_ERR_IO;
        }
        d = rx + 2;
        dl = 4;
        break;
    default:
        return T5_ERR_NOT_SUPPORTED;
    }
    if (data) *data = d;
    if (dlen) *dlen = dl;
    return T5_OK;
}

uint32_t t5_modbus_t35_us(uint32_t baud)
{
    if (!baud) {
        return 0;
    }
    if (baud > 19200) {
        return 1750;
    }
    return (uint32_t)((35ull * 11 * 1000000) / (10ull * baud));
}

void t5_modbus_init(t5_modbus_t *mb, t5_port_t *port)
{
    if (mb) {
        *mb = (t5_modbus_t){ .port = port, .timeout_ms = DEFAULT_TIMEOUT_MS };
    }
}

/* Read exactly n bytes, or fewer if timeout_ms passes. */
static size_t read_n(t5_port_t *p, uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    uint32_t start = t5_time_ms();
    while (got < n) {
        uint32_t spent = t5_time_ms() - start;
        if (spent > timeout_ms) {
            break;
        }
        int r = t5_port_uart_read(p, buf + got, n - got, timeout_ms - spent);
        if (r < 0) {
            break;
        }
        if (r == 0 && t5_time_ms() - start >= timeout_ms) {
            break;
        }
        got += (size_t)r;
        if (r == 0) {
            t5_delay_ms(1);
        }
    }
    return got;
}

/* Send req, wait for a reply of expect bytes (or a 5-byte exception). */
static t5_err_t transact(t5_modbus_t *mb, const uint8_t *req, size_t reqlen, uint8_t *rx, size_t expect,
                         const uint8_t **data, size_t *dlen)
{
    if (!mb || !mb->port) {
        return T5_ERR_INVALID_ARG;
    }
    uint32_t baud = t5_port_uart_baud(mb->port);
    if (!baud) {
        return T5_ERR_INVALID_ARG;
    }
    mb->exception = 0;
    uint32_t t35 = t5_modbus_t35_us(baud);
    uint32_t char_us = (uint32_t)(11ull * 1000000 / baud);

    int64_t idle = t5_time_us() - mb->last_us;
    if (mb->last_us && idle < (int64_t)t35) {
        t5_delay_ms((uint32_t)((t35 - idle + 999) / 1000));
    }
    T5_IGNORE(t5_port_uart_flush(mb->port));   /* best effort: stale bytes fail the CRC anyway */
    int w = t5_port_uart_write(mb->port, req, reqlen);
    if (w != (int)reqlen) {
        return w < 0 ? (t5_err_t)w : T5_ERR_IO;
    }
    /* Reading before the frame has left the wire would turn RS-485 around too early. */
    T5_RETURN_ON_ERR(t5_port_uart_wait_tx(mb->port, 20 + (uint32_t)(reqlen * char_us / 1000)));
    mb->last_us = t5_time_us();
    if (req[0] == 0) {
        /* Broadcast: no reply. Give slaves the turnaround delay. */
        t5_delay_ms(100);
        mb->last_us = t5_time_us();
        return T5_OK;
    }

    size_t got = read_n(mb->port, rx, 5, mb->timeout_ms ? mb->timeout_ms : DEFAULT_TIMEOUT_MS);
    if (got == 0) {
        return T5_ERR_TIMEOUT;
    }
    if (got == 5 && !(rx[1] & 0x80) && expect > 5) {
        uint32_t rest_ms = 20 + (uint32_t)(((expect - 5) * char_us + t35) / 1000);
        got += read_n(mb->port, rx + 5, expect - 5, rest_ms);
    }
    mb->last_us = t5_time_us();
    t5_err_t err = t5_modbus_parse_reply(rx, got, req[0], req[1], &mb->exception, data, dlen);
    if (err != T5_OK) {
        T5_LOGD(TAG, "slave %u fn 0x%02x: %s (exc %u, %u bytes)", req[0], req[1], t5_err_str(err),
                mb->exception, (unsigned)got);
    }
    return err;
}

static t5_err_t read_regs(t5_modbus_t *mb, uint8_t fn, uint8_t slave, uint16_t addr, uint16_t count,
                          uint16_t *out)
{
    if (!out || !count || count > T5_MODBUS_MAX_READ || slave == 0) {
        return T5_ERR_INVALID_ARG;
    }
    uint8_t req[8], rx[T5_MODBUS_MAX_FRAME];
    size_t n = t5_modbus_build_req(req, sizeof req, slave, fn, addr, count);
    const uint8_t *d;
    size_t dl;
    T5_RETURN_ON_ERR(transact(mb, req, n, rx, t5_modbus_reply_len(fn, count), &d, &dl));
    if (dl != 2u * count) {
        return T5_ERR_IO;
    }
    for (uint16_t i = 0; i < count; i++) {
        out[i] = (uint16_t)((d[2 * i] << 8) | d[2 * i + 1]);
    }
    return T5_OK;
}

t5_err_t t5_modbus_read_holding(t5_modbus_t *mb, uint8_t slave, uint16_t addr, uint16_t count, uint16_t *out)
{
    return read_regs(mb, T5_MODBUS_FN_READ_HOLDING, slave, addr, count, out);
}

t5_err_t t5_modbus_read_input(t5_modbus_t *mb, uint8_t slave, uint16_t addr, uint16_t count, uint16_t *out)
{
    return read_regs(mb, T5_MODBUS_FN_READ_INPUT, slave, addr, count, out);
}

t5_err_t t5_modbus_read_coils(t5_modbus_t *mb, uint8_t slave, uint16_t addr, uint16_t count, uint8_t *bits)
{
    if (!bits || !count || count > T5_MODBUS_MAX_BITS || slave == 0) {
        return T5_ERR_INVALID_ARG;
    }
    uint8_t req[8], rx[T5_MODBUS_MAX_FRAME];
    size_t n = t5_modbus_build_req(req, sizeof req, slave, T5_MODBUS_FN_READ_COILS, addr, count);
    const uint8_t *d;
    size_t dl;
    T5_RETURN_ON_ERR(transact(mb, req, n, rx, t5_modbus_reply_len(T5_MODBUS_FN_READ_COILS, count), &d, &dl));
    if (dl != (count + 7u) / 8) {
        return T5_ERR_IO;
    }
    memcpy(bits, d, dl);
    return T5_OK;
}

/* Single writes echo the request. */
static t5_err_t write_single(t5_modbus_t *mb, uint8_t fn, uint8_t slave, uint16_t addr, uint16_t value)
{
    uint8_t req[8], rx[T5_MODBUS_MAX_FRAME];
    size_t n = t5_modbus_build_req(req, sizeof req, slave, fn, addr, value);
    const uint8_t *d = NULL;
    size_t dl = 0;
    T5_RETURN_ON_ERR(transact(mb, req, n, rx, 8, &d, &dl));
    /* The echo of address and value/count; a broadcast (slave 0) has no reply. */
    if (slave != 0 && (!d || dl < 4 || memcmp(d, req + 2, 4) != 0)) {
        return T5_ERR_IO;
    }
    return T5_OK;
}

t5_err_t t5_modbus_write_register(t5_modbus_t *mb, uint8_t slave, uint16_t addr, uint16_t value)
{
    return write_single(mb, T5_MODBUS_FN_WRITE_REGISTER, slave, addr, value);
}

t5_err_t t5_modbus_write_coil(t5_modbus_t *mb, uint8_t slave, uint16_t addr, bool on)
{
    return write_single(mb, T5_MODBUS_FN_WRITE_COIL, slave, addr, on ? 0xFF00 : 0x0000);
}

t5_err_t t5_modbus_write_registers(t5_modbus_t *mb, uint8_t slave, uint16_t addr, const uint16_t *vals,
                                   uint16_t count)
{
    uint8_t req[T5_MODBUS_MAX_FRAME], rx[T5_MODBUS_MAX_FRAME];
    size_t n = t5_modbus_build_write_multi(req, sizeof req, slave, addr, vals, count);
    if (!n) {
        return T5_ERR_INVALID_ARG;
    }
    const uint8_t *d = NULL;
    size_t dl = 0;
    T5_RETURN_ON_ERR(transact(mb, req, n, rx, 8, &d, &dl));
    /* The echo of address and value/count; a broadcast (slave 0) has no reply. */
    if (slave != 0 && (!d || dl < 4 || memcmp(d, req + 2, 4) != 0)) {
        return T5_ERR_IO;
    }
    return T5_OK;
}
