/* SPDX-License-Identifier: Apache-2.0
 * Phase 5.1-5.3: port broker, port I/O over the hw mock, Modbus RTU framing
 * and transactions, module profiles and suggestions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "t5_mocks.h"
#include "ports_hw_mock.h"
#include "ports_priv.h"
#include "t5_core/res.h"
#include "t5_ports/access.h"
#include "t5_ports/modbus.h"
#include "t5_ports/ports.h"
#include "t5_ports/profiles.h"

/* Two fake apps; contexts start with the header like the real app runtime's. */
static t5_app_ctx_hdr_t s_app_a = { T5_APP_CTX_MAGIC, "com.x.app", "App" };
static t5_app_ctx_hdr_t s_app_b = { T5_APP_CTX_MAGIC, "com.y.other", "Other" };

/* Resource hooks: record adds/removes. */
static struct {
    int adds, removes;
    void *app;
    t5_res_kind_t kind;
    void *handle;
    t5_res_release_fn_t release;
} s_res;

static void res_add(void *app, t5_res_kind_t kind, void *handle, t5_res_release_fn_t release)
{
    s_res.adds++;
    s_res.app = app;
    s_res.kind = kind;
    s_res.handle = handle;
    s_res.release = release;
}

static void res_remove(void *app, void *handle)
{
    s_res.removes++;
}

static const t5_res_hooks_t s_hooks = { res_add, res_remove };

static t5_ev_port_t s_last_ev;
static void on_port_ev(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    memcpy(&s_last_ev, data, sizeof s_last_ev);
}

void setUp(void)
{
    mock_reset_all();
    mock_ports_reset();
    t5_ports_reset_for_test();
    memset(&s_res, 0, sizeof s_res);
    memset(&s_last_ev, 0, sizeof s_last_ev);
    t5_res_set_hooks(&s_hooks);
    t5_app_ctx_set(NULL);
    t5_event_subscribe(T5_EV_PORT_CLAIMED, on_port_ev, NULL);
    t5_event_subscribe(T5_EV_PORT_RELEASED, on_port_ev, NULL);
}

void tearDown(void)
{
    t5_res_set_hooks(NULL);
    t5_app_ctx_set(NULL);
}

static void test_table_sane(void)
{
    t5_port_info_t pi;
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_find("grove", &pi));
    TEST_ASSERT_EQUAL_HEX8(0x3, pi.adc_mask);           /* G53/G54 are ADC2 ch4/5 */
    TEST_ASSERT_TRUE(pi.modes & (1u << T5_MODE_ADC));
    /* G18/G19 have ADC channels, but stamp isn't a general-purpose port. */
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_find("stamp", &pi));
    TEST_ASSERT_NOT_EQUAL(0, pi.adc_mask);
    TEST_ASSERT_FALSE(pi.modes & (1u << T5_MODE_ADC));
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_find("mbus.uart", &pi));
    TEST_ASSERT_EQUAL_HEX8(0x3, pi.risky_mask);
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_find("mbus.g16", &pi));
    TEST_ASSERT_TRUE(pi.modes & (1u << T5_MODE_ADC));
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_find("mbus.g2", &pi));
    TEST_ASSERT_FALSE(pi.modes & (1u << T5_MODE_ADC));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_ports_find("nope", &pi));
    /* Every port's pins are unique within the port and valid GPIOs. */
    for (int i = 0; i < t5_ports_count(); i++) {
        TEST_ASSERT_EQUAL(T5_OK, t5_ports_info(i, &pi));
        for (int a = 0; a < pi.npins; a++) {
            TEST_ASSERT_LESS_THAN(55, pi.gpio[a]);
            for (int b = a + 1; b < pi.npins; b++) TEST_ASSERT_NOT_EQUAL(pi.gpio[a], pi.gpio[b]);
        }
    }
    uint8_t pins[64];
    int n = t5_ports_pins(pins, 64);
    TEST_ASSERT_GREATER_THAN(25, n);
    for (int i = 1; i < n; i++) TEST_ASSERT_LESS_THAN(pins[i], pins[i - 1]);
    TEST_ASSERT_EQUAL(T5_MODE_RS485, t5_port_mode_parse("rs485"));
    TEST_ASSERT_EQUAL(T5_MODE_COUNT, t5_port_mode_parse("can"));
}

static void test_conflict_names_owner_and_port(void)
{
    t5_port_t *a, *b;
    char why[T5_PORT_WHY_LEN];
    t5_app_ctx_set(&s_app_a);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("mbus.spi", T5_MODE_SPI, NULL, &a));
    TEST_ASSERT_EQUAL_STRING("com.x.app", s_last_ev.owner);
    TEST_ASSERT_EQUAL_STRING("mbus.spi", s_last_ev.port);

    /* stamp SPI uses G5/G18/G19 too: the shared-pin group conflicts by design. */
    t5_app_ctx_set(&s_app_b);
    t5_port_spi_cfg_t sc = { .hz = 1000000, .cs_pin_index = 0 };
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_port_open_ex("stamp", T5_MODE_SPI, &sc, 0, &b, why, sizeof why));
    TEST_ASSERT_NULL(b);
    TEST_ASSERT_EQUAL_STRING("G5 held by com.x.app (mbus.spi)", why);

    /* stamp GPIO on a pin mbus.spi doesn't use (G46) is fine; on G18 it isn't. */
    t5_port_gpio_cfg_t g = { .pin_index = 5 };
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("stamp", T5_MODE_GPIO, &g, &b));
    t5_port_t *c;
    g.pin_index = 2;
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_port_open_ex("stamp", T5_MODE_GPIO, &g, 0, &c, why, sizeof why));
    TEST_ASSERT_EQUAL_STRING("G18 held by com.x.app (mbus.spi)", why);

    /* The same app can't claim twice either. */
    t5_app_ctx_set(&s_app_a);
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_port_open("mbus.spi", T5_MODE_SPI, NULL, &c));

    TEST_ASSERT_EQUAL(T5_OK, t5_port_close(a));
    t5_app_ctx_set(&s_app_b);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_close(b));
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("stamp", T5_MODE_SPI, &sc, &b));
    TEST_ASSERT_EQUAL(4, mock_ports.released[5] + mock_ports.released[18] + mock_ports.released[19] +
                         mock_ports.released[46]);   /* G5, G18, G19 by mbus.spi, G46 by the GPIO */
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(b));
}

static void test_debug_uart_vs_stamp_and_singles(void)
{
    t5_port_t *dbg, *s, *g;
    char why[T5_PORT_WHY_LEN];
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("mbus.debug", T5_MODE_UART, NULL, &dbg));
    /* stamp UART uses G6 (mbus.debug TX). System caller is named "system". */
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_port_open_ex("stamp", T5_MODE_UART, NULL, 0, &s, why, sizeof why));
    TEST_ASSERT_EQUAL_STRING("G6 held by system (mbus.debug)", why);
    /* Independent single-pin ports don't interfere. */
    t5_port_gpio_cfg_t out = { .output = true };
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("mbus.g2", T5_MODE_GPIO, &out, &g));
    TEST_ASSERT_EQUAL(T5_OK, t5_port_gpio_set(g, true));
    bool lv = false;
    TEST_ASSERT_EQUAL(T5_OK, t5_port_gpio_get(g, &lv));
    TEST_ASSERT_TRUE(lv);
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(g));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(dbg));
}

static void test_ext_pins_split_by_mode(void)
{
    t5_port_t *i2c, *uart, *gpio;
    char why[T5_PORT_WHY_LEN];
    /* ext I2C (G49/G50) and ext UART (G0/G1) can coexist; GPIO on G49 can't. */
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("ext", T5_MODE_I2C, NULL, &i2c));
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("ext", T5_MODE_UART, NULL, &uart));
    t5_port_gpio_cfg_t g = { .pin_index = 2 };
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_port_open_ex("ext", T5_MODE_GPIO, &g, 0, &gpio, why, sizeof why));
    TEST_ASSERT_EQUAL_STRING("G49 held by system (ext)", why);
    g.pin_index = 9;
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_port_open("ext", T5_MODE_GPIO, &g, &gpio));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_port_open_ex("ext", T5_MODE_SPI, NULL, 0, &gpio, why, sizeof why));
    TEST_ASSERT_EQUAL_STRING("ext has no spi mode", why);
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_port_open("usb", T5_MODE_GPIO, NULL, &gpio));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(i2c));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(uart));
    TEST_ASSERT_EQUAL(0, mock_ports.live[MOCK_I2C] + mock_ports.live[MOCK_UART]);
}

static void test_risky_pins(void)
{
    t5_port_t *p;
    char why[T5_PORT_WHY_LEN];
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_port_open_ex("mbus.uart", T5_MODE_UART, NULL, 0, &p, why, sizeof why));
    TEST_ASSERT_NOT_NULL(strstr(why, "G37"));
    TEST_ASSERT_NOT_NULL(strstr(why, "T5_PORT_FLAG_RISKY"));
    TEST_ASSERT_EQUAL(0, mock_ports.opens);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open_ex("mbus.uart", T5_MODE_UART, NULL, T5_PORT_FLAG_RISKY, &p, NULL, 0));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));

    /* G35: flag needed, input only, never reconfigured or reset. */
    t5_port_gpio_cfg_t in = { 0 };
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_port_open("mbus.g35", T5_MODE_GPIO, &in, &p));
    t5_port_gpio_cfg_t out = { .output = true };
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_port_open_ex("mbus.g35", T5_MODE_GPIO, &out, T5_PORT_FLAG_RISKY, &p, why, sizeof why));
    TEST_ASSERT_NOT_NULL(strstr(why, "input only"));
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open_ex("mbus.g35", T5_MODE_GPIO, &in, T5_PORT_FLAG_RISKY, &p, NULL, 0));
    TEST_ASSERT_EQUAL(T5_ERR_STATE, t5_port_gpio_set(p, true));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
    TEST_ASSERT_EQUAL(0, mock_ports.configured[35]);
    TEST_ASSERT_EQUAL(0, mock_ports.released[35]);

    /* Risky is checked before conflicts: a held risky pin still says DENIED without the flag. */
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open_ex("mbus.uart", T5_MODE_UART, NULL, T5_PORT_FLAG_RISKY, &p, NULL, 0));
    t5_port_t *q;
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_port_open("mbus.uart", T5_MODE_UART, NULL, &q));
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_port_open_ex("mbus.uart", T5_MODE_UART, NULL, T5_PORT_FLAG_RISKY, &q, NULL, 0));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
}

static bool only_grove(const char *port, char *why, size_t whylen)
{
    if (!strcmp(port, "grove")) return true;
    snprintf(why, whylen, "not %s", port);
    return false;
}

static void test_access_check(void)
{
    t5_port_t *p;
    char why[T5_PORT_WHY_LEN];
    t5_ports_set_access_check(only_grove);
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_port_open_ex("ext", T5_MODE_UART, NULL, 0, &p, why, sizeof why));
    TEST_ASSERT_EQUAL_STRING("not ext", why);
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL(0, mock_ports.opens);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("grove", T5_MODE_UART, NULL, &p));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
    t5_ports_set_access_check(NULL);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("ext", T5_MODE_UART, NULL, &p));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
}

static void test_release_and_res_hooks(void)
{
    t5_port_t *p;
    t5_app_ctx_set(&s_app_a);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("grove", T5_MODE_I2C, NULL, &p));
    TEST_ASSERT_EQUAL(1, s_res.adds);
    TEST_ASSERT_EQUAL(T5_RES_PORT, s_res.kind);
    TEST_ASSERT_EQUAL_PTR(p, s_res.handle);
    TEST_ASSERT_EQUAL_PTR(&s_app_a, s_res.app);
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_PORT_CLAIMED));

    t5_pin_info_t pi;
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_pin_info(53, &pi));
    TEST_ASSERT_TRUE(pi.claimed);
    TEST_ASSERT_EQUAL_STRING("com.x.app", pi.owner);
    TEST_ASSERT_EQUAL_STRING("grove", pi.port);
    TEST_ASSERT_EQUAL(T5_MODE_I2C, pi.mode);

    /* App exit: the runtime calls the release fn from a system task. */
    t5_app_ctx_set(NULL);
    s_res.release(s_res.handle);
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_PORT_RELEASED));
    TEST_ASSERT_EQUAL(0, mock_ports.live[MOCK_I2C]);
    TEST_ASSERT_EQUAL(1, mock_ports.released[53]);
    TEST_ASSERT_EQUAL(1, mock_ports.released[54]);
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_pin_info(53, &pi));
    TEST_ASSERT_FALSE(pi.claimed);
    TEST_ASSERT_NULL(pi.port);
    /* Double close is harmless; ops on a closed handle fail. */
    TEST_ASSERT_EQUAL(T5_OK, t5_port_close(p));
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_PORT_RELEASED));
    uint8_t b;
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_port_i2c_read_reg(p, 0x44, 0, &b, 1));

    /* Normal close by the app removes the record. */
    t5_app_ctx_set(&s_app_a);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("grove", T5_MODE_I2C, NULL, &p));
    TEST_ASSERT_EQUAL(T5_OK, t5_port_close(p));
    TEST_ASSERT_EQUAL(1, s_res.removes);
}

static void test_driver_failure_rolls_back(void)
{
    t5_port_t *p;
    char why[T5_PORT_WHY_LEN];
    mock_ports.fail_open = T5_ERR_BUSY;
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_port_open_ex("rs485", T5_MODE_RS485, NULL, 0, &p, why, sizeof why));
    TEST_ASSERT_NOT_NULL(strstr(why, "driver failed"));
    t5_pin_info_t pi;
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_pin_info(20, &pi));
    TEST_ASSERT_FALSE(pi.claimed);
    TEST_ASSERT_EQUAL(0, mock_events_count(T5_EV_PORT_CLAIMED));
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("rs485", T5_MODE_RS485, NULL, &p));
    TEST_ASSERT_EQUAL(34, mock_ports.rts);       /* DIR on G34 as RTS */
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
}

static void test_handle_pool_reusable(void)
{
    /* Open/close many times: no leak of handles, pins or drivers. */
    for (int i = 0; i < 500; i++) {
        t5_port_t *p;
        t5_port_pwm_cfg_t pc = { .hz = 500 };
        TEST_ASSERT_EQUAL(T5_OK, t5_port_open("mbus.g47", T5_MODE_PWM, &pc, &p));
        TEST_ASSERT_EQUAL(T5_OK, t5_port_pwm_set(p, 250));
        TEST_ASSERT_EQUAL(T5_OK, t5_port_close(p));
    }
    TEST_ASSERT_EQUAL(0, mock_ports.live[MOCK_PWM]);
    TEST_ASSERT_EQUAL(250, mock_ports.duty);
}

static void test_adc_and_modes(void)
{
    t5_port_t *p;
    t5_port_adc_cfg_t ac = { 0 };
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_port_open("mbus.g2", T5_MODE_ADC, &ac, &p));
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("mbus.g16", T5_MODE_ADC, &ac, &p));
    mock_ports.mv = 1234;
    int mv = 0;
    TEST_ASSERT_EQUAL(T5_OK, t5_port_adc_read_mv(p, &mv));
    TEST_ASSERT_EQUAL(1234, mv);
    /* Wrong-mode calls are rejected. */
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_port_pwm_set(p, 10));
    TEST_ASSERT_LESS_THAN(0, t5_port_uart_write(p, "x", 1));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
    /* ext ADC only on its ADC-capable pins (G49/G50). */
    ac.pin_index = 0;
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_port_open("ext", T5_MODE_ADC, &ac, &p));
    ac.pin_index = 3;
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("ext", T5_MODE_ADC, &ac, &p));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
}

static void test_external_i2c(void)
{
    t5_port_t *p;
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("grove", T5_MODE_I2C, NULL, &p));
    mock_ports.i2c_present[0x44] = true;     /* external: reserved list doesn't apply */
    mock_ports.i2c_present[0x70] = true;
    uint8_t buf[2];
    TEST_ASSERT_EQUAL(T5_OK, t5_port_i2c_read_reg(p, 0x44, 0x07, buf, 2));
    TEST_ASSERT_EQUAL_HEX8(0x07, mock_ports.i2c_last_tx[0]);
    TEST_ASSERT_EQUAL_HEX8(0x44, buf[0]);
    uint8_t w[2] = { 1, 2 };
    TEST_ASSERT_EQUAL(T5_OK, t5_port_i2c_write_reg(p, 0x70, 0x10, w, 2));
    TEST_ASSERT_EQUAL_HEX8(0x10, mock_ports.i2c_last_tx[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, mock_ports.i2c_last_tx[2]);
    TEST_ASSERT_EQUAL(T5_ERR_NACK, t5_port_i2c_probe(p, 0x29));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_port_i2c_probe(p, 0x02));
    uint8_t found[8];
    TEST_ASSERT_EQUAL(2, t5_port_i2c_scan(p, found, 8));
    TEST_ASSERT_EQUAL_HEX8(0x44, found[0]);
    TEST_ASSERT_EQUAL_HEX8(0x70, found[1]);
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
}

static void test_mbus_i2c_shared_and_reserved(void)
{
    t5_port_t *a, *b;
    mock_i2c_dev_t *d = mock_i2c_add(0x50, false);
    d->regs[3] = 0xAB;
    mock_i2c_add(0x44, false);              /* EXP1: reserved */
    t5_app_ctx_set(&s_app_a);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("mbus.i2c", T5_MODE_I2C, NULL, &a));
    t5_app_ctx_set(&s_app_b);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("mbus.i2c", T5_MODE_I2C, NULL, &b));   /* shared bus */
    uint8_t v = 0;
    TEST_ASSERT_EQUAL(T5_OK, t5_port_i2c_read_reg(b, 0x50, 3, &v, 1));
    TEST_ASSERT_EQUAL_HEX8(0xAB, v);
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_port_i2c_read_reg(b, 0x44, 0, &v, 1));
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_port_i2c_write_reg(b, 0x68, 0, &v, 1));
    TEST_ASSERT_EQUAL(T5_ERR_DENIED, t5_port_i2c_probe(b, 0x44));
    uint8_t found[8];
    int n = t5_port_i2c_scan(a, found, 8);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX8(0x50, found[0]);
    /* More addresses than the per-handle cache: older t5_i2c handles are recycled. */
    for (uint8_t addr = 0x60; addr < 0x68; addr++) mock_i2c_add(addr, false);
    for (uint8_t addr = 0x60; addr < 0x68; addr++) {
        TEST_ASSERT_EQUAL(T5_OK, t5_port_i2c_read_reg(a, addr, 0, &v, 1));
    }
    t5_pin_info_t pi;
    TEST_ASSERT_EQUAL(T5_OK, t5_ports_pin_info(31, &pi));
    TEST_ASSERT_FALSE(pi.claimed);
    TEST_ASSERT_EQUAL_STRING("system", pi.owner);
    TEST_ASSERT_EQUAL_STRING("mbus.i2c", pi.port);
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(a));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(b));
    t5_i2c_dev_stats_t st[T5_I2C_MAX_DEVICES];
    TEST_ASSERT_EQUAL(0, t5_i2c_dev_stats(st, T5_I2C_MAX_DEVICES));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_ports_pin_info(22, &pi));   /* backlight: not a port pin */
}

static void test_spi_and_uart_io(void)
{
    t5_port_t *p;
    t5_port_spi_cfg_t sc = { .hz = 8000000, .mode = 0, .cs_pin_index = -1 };
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("mbus.spi", T5_MODE_SPI, &sc, &p));
    uint8_t tx[4] = { 1, 2, 3, 4 }, rx[4] = { 0 };
    TEST_ASSERT_EQUAL(T5_OK, t5_port_spi_transfer(p, tx, rx, 4));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tx, rx, 4);
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
    sc.cs_pin_index = 0;    /* mbus.spi has no CS pin; index 0 is MOSI */
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_port_open("mbus.spi", T5_MODE_SPI, &sc, &p));

    t5_port_uart_cfg_t uc = { .baud = 9600 };
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("grove", T5_MODE_UART, &uc, &p));
    TEST_ASSERT_EQUAL(9600, t5_port_uart_baud(p));
    TEST_ASSERT_EQUAL(-1, mock_ports.rts);
    TEST_ASSERT_EQUAL(5, t5_port_uart_write(p, "hello", 5));
    TEST_ASSERT_EQUAL_MEMORY("hello", mock_ports.tx, 5);
    char buf[4];
    TEST_ASSERT_EQUAL(0, t5_port_uart_read(p, buf, 4, 10));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
    uc.data_bits = 9;
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_port_open("grove", T5_MODE_UART, &uc, &p));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_ports_pin_info(53, NULL));
}

/* ---- Modbus ------------------------------------------------------------------ */

static void test_modbus_crc_and_frames(void)
{
    /* Classic example: 01 03 00 00 00 0A -> CRC C5 CD */
    uint8_t f[8];
    TEST_ASSERT_EQUAL(8, t5_modbus_build_req(f, sizeof f, 1, 0x03, 0x0000, 10));
    const uint8_t want[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, 8);
    TEST_ASSERT_EQUAL_HEX16(0x4B37, t5_modbus_crc16((const uint8_t *)"123456789", 9));
    /* 11 06 00 01 00 03 -> 9A 9B */
    t5_modbus_build_req(f, sizeof f, 0x11, 0x06, 0x0001, 0x0003);
    TEST_ASSERT_EQUAL_HEX8(0x9A, f[6]);
    TEST_ASSERT_EQUAL_HEX8(0x9B, f[7]);
    TEST_ASSERT_EQUAL(0, t5_modbus_build_req(f, 4, 1, 3, 0, 1));

    uint8_t m[32];
    uint16_t vals[2] = { 0x000A, 0x0102 };
    size_t n = t5_modbus_build_write_multi(m, sizeof m, 0x11, 0x0001, vals, 2);
    TEST_ASSERT_EQUAL(13, n);
    const uint8_t wm[11] = { 0x11, 0x10, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x0A, 0x01, 0x02 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(wm, m, 11);
    TEST_ASSERT_EQUAL_HEX8(0xC6, m[11]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, m[12]);

    TEST_ASSERT_EQUAL(9, t5_modbus_reply_len(0x03, 2));
    TEST_ASSERT_EQUAL(6, t5_modbus_reply_len(0x01, 3));
    TEST_ASSERT_EQUAL(8, t5_modbus_reply_len(0x10, 2));
    TEST_ASSERT_EQUAL(4010, t5_modbus_t35_us(9600));
    TEST_ASSERT_EQUAL(1750, t5_modbus_t35_us(115200));
}

static size_t with_crc(uint8_t *f, size_t n)
{
    uint16_t c = t5_modbus_crc16(f, n);
    f[n] = (uint8_t)c;
    f[n + 1] = (uint8_t)(c >> 8);
    return n + 2;
}

static void test_modbus_parse(void)
{
    uint8_t r[16] = { 0x01, 0x03, 0x04, 0x00, 0x2A, 0x01, 0x00 };
    size_t n = with_crc(r, 7);
    const uint8_t *d;
    size_t dl;
    uint8_t exc;
    TEST_ASSERT_EQUAL(T5_OK, t5_modbus_parse_reply(r, n, 1, 0x03, &exc, &d, &dl));
    TEST_ASSERT_EQUAL(4, dl);
    TEST_ASSERT_EQUAL_HEX8(0x2A, d[1]);
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_modbus_parse_reply(r, n, 2, 0x03, &exc, &d, &dl));   /* wrong slave */
    r[4] ^= 1;
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_modbus_parse_reply(r, n, 1, 0x03, &exc, &d, &dl));   /* bad CRC */

    uint8_t e[8] = { 0x01, 0x83, 0x02 };
    n = with_crc(e, 3);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_modbus_parse_reply(e, n, 1, 0x03, &exc, &d, &dl));
    TEST_ASSERT_EQUAL(2, exc);
    e[2] = 0x01;
    n = with_crc(e, 3);
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_modbus_parse_reply(e, n, 1, 0x03, &exc, NULL, NULL));
    e[2] = 0x06;
    n = with_crc(e, 3);
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_modbus_parse_reply(e, n, 1, 0x03, &exc, NULL, NULL));
    e[2] = 0x04;
    n = with_crc(e, 3);
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_modbus_parse_reply(e, n, 1, 0x03, &exc, NULL, NULL));
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_modbus_parse_reply(e, 3, 1, 0x03, &exc, NULL, NULL));   /* short */
}

static void test_modbus_transactions(void)
{
    t5_port_t *p;
    t5_port_uart_cfg_t uc = { .baud = 9600, .parity = 2 };
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("rs485", T5_MODE_RS485, &uc, &p));
    t5_modbus_t mb;
    t5_modbus_init(&mb, p);

    uint8_t r[16] = { 0x07, 0x03, 0x04, 0x12, 0x34, 0xAB, 0xCD };
    mock_ports_uart_reply(r, with_crc(r, 7));
    uint16_t regs[2];
    TEST_ASSERT_EQUAL(T5_OK, t5_modbus_read_holding(&mb, 7, 0x0010, 2, regs));
    TEST_ASSERT_EQUAL_HEX16(0x1234, regs[0]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, regs[1]);
    TEST_ASSERT_EQUAL(8, mock_ports.ntx);
    TEST_ASSERT_EQUAL_HEX8(0x07, mock_ports.tx[0]);
    TEST_ASSERT_EQUAL_HEX8(0x10, mock_ports.tx[3]);
    TEST_ASSERT_GREATER_THAN(0, mock_ports.flushes);

    /* The next request waits out 3.5 char times (4 ms at 9600) after the reply. */
    uint32_t t0 = t5_time_ms();
    uint8_t ex[8] = { 0x07, 0x84, 0x02 };
    mock_ports_uart_reply(ex, with_crc(ex, 3));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_modbus_read_input(&mb, 7, 0x9999, 1, regs));
    TEST_ASSERT_EQUAL(2, mb.exception);
    TEST_ASSERT_GREATER_OR_EQUAL(t0 + 4, t5_time_ms());

    /* Write single: echoed. */
    mock_ports.ntx = 0;
    uint8_t w[8];
    t5_modbus_build_req(w, sizeof w, 7, 0x06, 0x0001, 0x0102);
    mock_ports_uart_reply(w, 8);
    TEST_ASSERT_EQUAL(T5_OK, t5_modbus_write_register(&mb, 7, 0x0001, 0x0102));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(w, mock_ports.tx, 8);

    /* Write multiple: reply echoes address and count. */
    uint8_t wr[8] = { 0x07, 0x10, 0x00, 0x20, 0x00, 0x02 };
    mock_ports_uart_reply(wr, with_crc(wr, 6));
    uint16_t vals[2] = { 1, 2 };
    TEST_ASSERT_EQUAL(T5_OK, t5_modbus_write_registers(&mb, 7, 0x0020, vals, 2));

    /* Coils. */
    uint8_t cr[8] = { 0x07, 0x01, 0x02, 0x05, 0x01 };
    mock_ports_uart_reply(cr, with_crc(cr, 5));
    uint8_t bits[2];
    TEST_ASSERT_EQUAL(T5_OK, t5_modbus_read_coils(&mb, 7, 0, 10, bits));
    TEST_ASSERT_EQUAL_HEX8(0x05, bits[0]);
    t5_modbus_build_req(w, sizeof w, 7, 0x05, 3, 0xFF00);
    mock_ports_uart_reply(w, 8);
    TEST_ASSERT_EQUAL(T5_OK, t5_modbus_write_coil(&mb, 7, 3, true));

    /* No reply: timeout. Corrupt reply: IO. */
    TEST_ASSERT_EQUAL(T5_ERR_TIMEOUT, t5_modbus_read_holding(&mb, 7, 0, 2, regs));
    r[3] ^= 0xFF;
    mock_ports_uart_reply(r, 9);
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_modbus_read_holding(&mb, 7, 0x0010, 2, regs));
    /* Broadcast write: no reply awaited. */
    TEST_ASSERT_EQUAL(T5_OK, t5_modbus_write_register(&mb, 0, 1, 1));
    /* Bad args. */
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_modbus_read_holding(&mb, 7, 0, 126, regs));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_modbus_read_holding(&mb, 0, 0, 1, regs));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_modbus_read_holding(&mb, 7, 0, 1, regs));
}

/* ---- Profiles ------------------------------------------------------------------ */

static void test_profile_parse(void)
{
    const char *js =
        "[{\"name\":\"My Sensor\", \"ports\":[\"grove\"], \"bus\":\"i2c\", \"i2c\":[\"0x44\", 112],\n"
        "  \"power\":[\"ext5v\",\"hvin\"], \"driver\":\"sht3x\", \"notes\":\"tab\\tq\\\"\\u00e9\",\n"
        "  \"extra\": {\"nested\": [1, 2.5e3, true, null, \"x\"]}},\n"
        " {\"name\":\"Relay\", \"port\":\"mbus.g2\", \"bus\":\"gpio\"}]";
    t5_profile_t p[4];
    TEST_ASSERT_EQUAL(2, t5_profile_parse(js, strlen(js), p, 4));
    TEST_ASSERT_EQUAL_STRING("My Sensor", p[0].name);
    TEST_ASSERT_EQUAL(1, p[0].nports);
    TEST_ASSERT_EQUAL_STRING("grove", p[0].ports[0]);
    TEST_ASSERT_EQUAL(T5_MODE_I2C, p[0].bus);
    TEST_ASSERT_EQUAL(2, p[0].naddrs);
    TEST_ASSERT_EQUAL_HEX8(0x44, p[0].addrs[0]);
    TEST_ASSERT_EQUAL_HEX8(0x70, p[0].addrs[1]);
    TEST_ASSERT_EQUAL(T5_PROFILE_POWER_EXT5V | T5_PROFILE_POWER_HVIN, p[0].power);
    TEST_ASSERT_EQUAL_STRING("tab\tq\"?", p[0].notes);
    TEST_ASSERT_EQUAL(T5_MODE_GPIO, p[1].bus);
    TEST_ASSERT_EQUAL_STRING("mbus.g2", p[1].ports[0]);

    const char *bad[] = {
        "{\"name\":\"x\"", "{\"ports\":[\"grove\"]}", "{\"name\":\"x\",\"bus\":\"can\"}",
        "{\"name\":\"x\",\"i2c\":[300]}", "{\"name\":\"x\"} trailing", "[{\"name\":\"x\"},]", "",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_LESS_THAN_MESSAGE(0, t5_profile_parse(bad[i], strlen(bad[i]), p, 4), bad[i]);
    }
    const char *one = "  {\"name\":\"Solo\"}  ";
    TEST_ASSERT_EQUAL(1, t5_profile_parse(one, strlen(one), p, 4));
}

static void test_profile_load_dir(void)
{
    char dir[] = "/tmp/t5prof.XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    char path[96];
    snprintf(path, sizeof path, "%s/a.json", dir);
    FILE *f = fopen(path, "w");
    fputs("{\"name\":\"Custom ENV\",\"ports\":[\"grove\"],\"i2c\":[\"0x44\",\"0x70\"]}", f);
    fclose(f);
    snprintf(path, sizeof path, "%s/broken.json", dir);
    f = fopen(path, "w");
    fputs("{nope", f);
    fclose(f);
    snprintf(path, sizeof path, "%s/readme.txt", dir);
    f = fopen(path, "w");
    fputs("{\"name\":\"ignored\"}", f);
    fclose(f);
    int base = t5_profiles_count();
    TEST_ASSERT_EQUAL(1, t5_profiles_load_dir(dir));
    TEST_ASSERT_EQUAL(base + 1, t5_profiles_count());
    const t5_profile_t *p = t5_profiles_get(base);
    TEST_ASSERT_EQUAL_STRING("Custom ENV", p->name);
    TEST_ASSERT_FALSE(p->builtin);
    TEST_ASSERT_EQUAL(0, t5_profiles_load_dir("/nonexistent/dir"));

    /* User profile ties with built-in ENV III on everything else: ranks first. */
    uint8_t found[2] = { 0x44, 0x70 };
    t5_profile_match_t m[8];
    int n = t5_profiles_suggest("grove", found, 2, m, 8);
    TEST_ASSERT_GREATER_OR_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("Custom ENV", m[0].profile->name);
    TEST_ASSERT_EQUAL_STRING("ENV III", m[1].profile->name);
    char cmd[64];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    (void)!system(cmd);
}

static void test_profile_suggest_ranking(void)
{
    uint8_t found[] = { 0x44, 0x70, 0x29 };
    t5_profile_match_t m[16];
    int n = t5_profiles_suggest("grove", found, 3, m, 16);
    TEST_ASSERT_GREATER_OR_EQUAL(6, n);
    /* Complete two-address match beats complete singles, which beat partials. */
    TEST_ASSERT_EQUAL_STRING("ENV III", m[0].profile->name);
    TEST_ASSERT_TRUE(m[0].complete);
    TEST_ASSERT_EQUAL(2, m[0].matched);
    TEST_ASSERT_TRUE(m[0].port_ok);
    bool seen_partial = false;
    for (int i = 0; i < n; i++) {
        if (!m[i].complete) seen_partial = true;
        else TEST_ASSERT_FALSE_MESSAGE(seen_partial, "complete after partial");
        TEST_ASSERT_GREATER_THAN(0, m[i].matched);
    }
    TEST_ASSERT_TRUE(seen_partial);     /* ENV II / ENV IV: 0x44 only */
    /* Singles at 0x29 (ToF, Color) and 0x70 (PaHUB) are all there. */
    int singles = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = m[i].profile->name;
        singles += !strcmp(nm, "ToF") || !strcmp(nm, "Color") || !strcmp(nm, "PaHUB");
    }
    TEST_ASSERT_EQUAL(3, singles);
    /* max limits the output; nothing found gives nothing. */
    TEST_ASSERT_EQUAL(1, t5_profiles_suggest("grove", found, 3, m, 1));
    TEST_ASSERT_EQUAL(0, t5_profiles_suggest("grove", NULL, 0, m, 16));
    /* A port the profiles don't list still matches, with port_ok false. */
    n = t5_profiles_suggest("mbus.i2c", found, 2, m, 16);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_FALSE(m[0].port_ok);
}

static void test_profile_scan(void)
{
    mock_ports.i2c_present[0x29] = true;
    uint8_t found[8];
    TEST_ASSERT_EQUAL(1, t5_profiles_scan("grove", found, 8));
    TEST_ASSERT_EQUAL_HEX8(0x29, found[0]);
    TEST_ASSERT_EQUAL(0, mock_ports.live[MOCK_I2C]);           /* closed again */
    /* Busy port: scan refuses rather than disturbing the owner. */
    t5_port_t *p;
    t5_app_ctx_set(&s_app_a);
    TEST_ASSERT_EQUAL(T5_OK, t5_port_open("grove", T5_MODE_UART, NULL, &p));
    t5_app_ctx_set(NULL);
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_profiles_scan("grove", found, 8));
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_port_close(p));
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_profiles_scan("rs485", found, 8));
    /* Internal bus only when named, never its reserved addresses. */
    mock_i2c_add(0x44, false);
    mock_i2c_add(0x5A, false);
    TEST_ASSERT_EQUAL(1, t5_profiles_scan("mbus.i2c", found, 8));
    TEST_ASSERT_EQUAL_HEX8(0x5A, found[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_table_sane);
    RUN_TEST(test_conflict_names_owner_and_port);
    RUN_TEST(test_debug_uart_vs_stamp_and_singles);
    RUN_TEST(test_ext_pins_split_by_mode);
    RUN_TEST(test_risky_pins);
    RUN_TEST(test_access_check);
    RUN_TEST(test_release_and_res_hooks);
    RUN_TEST(test_driver_failure_rolls_back);
    RUN_TEST(test_handle_pool_reusable);
    RUN_TEST(test_adc_and_modes);
    RUN_TEST(test_external_i2c);
    RUN_TEST(test_mbus_i2c_shared_and_reserved);
    RUN_TEST(test_spi_and_uart_io);
    RUN_TEST(test_modbus_crc_and_frames);
    RUN_TEST(test_modbus_parse);
    RUN_TEST(test_modbus_transactions);
    RUN_TEST(test_profile_parse);
    RUN_TEST(test_profile_load_dir);
    RUN_TEST(test_profile_suggest_ranking);
    RUN_TEST(test_profile_scan);
    return UNITY_END();
}
