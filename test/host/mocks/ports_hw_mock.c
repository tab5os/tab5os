/* SPDX-License-Identifier: Apache-2.0
 * Mock of the t5_ports hardware layer: counts live drivers, simulates an
 * external I2C bus (presence only), a UART that answers each write with a
 * scripted reply, GPIO levels, PWM duty and ADC readings.
 */
#include <stdlib.h>
#include <string.h>
#include "t5_mocks.h"
#include "ports_hw.h"
#include "ports_hw_mock.h"


mock_ports_t mock_ports;

void mock_ports_reset(void)
{
    memset(&mock_ports, 0, sizeof mock_ports);
    mock_ports.rts = -1;
}

/* The next uart write is answered with these bytes. */
void mock_ports_uart_reply(const uint8_t *d, size_t n)
{
    memcpy(mock_ports.reply, d, n);
    mock_ports.nreply = n;
}

struct ports_hw_obj {
    int kind;
};

static t5_err_t mk(int kind, ports_hw_t *out)
{
    if (mock_ports.fail_open != T5_OK) {
        t5_err_t e = mock_ports.fail_open;
        mock_ports.fail_open = T5_OK;
        return e;
    }
    ports_hw_t o = calloc(1, sizeof *o);
    o->kind = kind;
    mock_ports.live[kind]++;
    mock_ports.opens++;
    *out = o;
    return T5_OK;
}

static void rm(ports_hw_t h)
{
    if (h) {
        mock_ports.live[h->kind]--;
        free(h);
    }
}

t5_err_t ports_hw_i2c_open(int sda, int scl, uint32_t hz, ports_hw_t *out) { return mk(0, out); }
void ports_hw_i2c_close(ports_hw_t h) { rm(h); }

t5_err_t ports_hw_i2c_xfer(ports_hw_t h, uint8_t addr, const uint8_t *tx, size_t txlen, uint8_t *rx,
                           size_t rxlen, uint32_t timeout_ms)
{
    if (!mock_ports.i2c_present[addr & 0x7F]) return T5_ERR_NACK;
    memcpy(mock_ports.i2c_last_tx, tx, txlen < 8 ? txlen : 8);
    for (size_t i = 0; i < rxlen; i++) rx[i] = (uint8_t)(addr + i);
    return T5_OK;
}

t5_err_t ports_hw_i2c_probe(ports_hw_t h, uint8_t addr, uint32_t timeout_ms)
{
    return mock_ports.i2c_present[addr & 0x7F] ? T5_OK : T5_ERR_NACK;
}

t5_err_t ports_hw_uart_open(const ports_hw_uart_cfg_t *cfg, ports_hw_t *out)
{
    mock_ports.rts = cfg->rts;
    mock_ports.baud = cfg->baud;
    return mk(1, out);
}
void ports_hw_uart_close(ports_hw_t h) { rm(h); }

int ports_hw_uart_write(ports_hw_t h, const void *data, size_t n)
{
    if (mock_ports.ntx + n > sizeof mock_ports.tx) mock_ports.ntx = 0;
    memcpy(mock_ports.tx + mock_ports.ntx, data, n);
    mock_ports.ntx += n;
    if (mock_ports.nreply) {
        memcpy(mock_ports.rx, mock_ports.reply, mock_ports.nreply);
        mock_ports.nrx = mock_ports.nreply;
        mock_ports.rxpos = 0;
        mock_ports.nreply = 0;
    }
    return (int)n;
}

int ports_hw_uart_read(ports_hw_t h, void *data, size_t n, uint32_t timeout_ms)
{
    size_t avail = mock_ports.nrx - mock_ports.rxpos;
    if (!avail) {
        mock_time_advance_ms(timeout_ms);
        return 0;
    }
    if (n > avail) n = avail;
    memcpy(data, mock_ports.rx + mock_ports.rxpos, n);
    mock_ports.rxpos += n;
    return (int)n;
}

t5_err_t ports_hw_uart_flush(ports_hw_t h)
{
    mock_ports.flushes++;
    mock_ports.nrx = mock_ports.rxpos = 0;
    return T5_OK;
}

t5_err_t ports_hw_uart_wait_tx(ports_hw_t h, uint32_t timeout_ms) { return T5_OK; }

t5_err_t ports_hw_spi_open(int sck, int mosi, int miso, int cs, uint32_t hz, uint8_t mode, ports_hw_t *out)
{
    return mk(2, out);
}
void ports_hw_spi_close(ports_hw_t h) { rm(h); }

t5_err_t ports_hw_spi_transfer(ports_hw_t h, const void *tx, void *rx, size_t n)
{
    if (rx) {
        if (tx) memcpy(rx, tx, n);      /* loopback */
        else memset(rx, 0xFF, n);
    }
    return T5_OK;
}

t5_err_t ports_hw_gpio_config(int pin, bool output, bool pull_up, bool pull_down, bool open_drain)
{
    mock_ports.configured[pin]++;
    mock_ports.out[pin] = output;
    return T5_OK;
}

t5_err_t ports_hw_gpio_set(int pin, bool level)
{
    mock_ports.level[pin] = level;
    return T5_OK;
}

int ports_hw_gpio_get(int pin)
{
    return (pin >= 0 && pin < 64) ? mock_ports.level[pin] : -1;
}

t5_err_t ports_hw_pwm_open(int pin, uint32_t hz, uint8_t bits, ports_hw_t *out) { return mk(3, out); }
void ports_hw_pwm_close(ports_hw_t h) { rm(h); }
t5_err_t ports_hw_pwm_set(ports_hw_t h, uint32_t duty_permille)
{
    mock_ports.duty = duty_permille;
    return T5_OK;
}

t5_err_t ports_hw_adc_open(int pin, int unit, int channel, ports_hw_t *out) { return mk(4, out); }
void ports_hw_adc_close(ports_hw_t h) { rm(h); }
t5_err_t ports_hw_adc_read_mv(ports_hw_t h, int *mv)
{
    *mv = mock_ports.mv;
    return T5_OK;
}

void ports_hw_pin_release(int pin)
{
    if (pin >= 0 && pin < 64) mock_ports.released[pin]++;
}
