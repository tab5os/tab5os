/* SPDX-License-Identifier: Apache-2.0
 * t5_ports hardware layer on ESP-IDF drivers.
 *
 * Resources handed out here, all pooled so open/close can repeat forever:
 *   I2C   a second HP controller (i2c_port = -1 picks a free one; the system
 *         bus took the first). Two external I2C ports at once may run out.
 *   UART  UART1..UART4 (UART0 is left alone for the ROM/secondary console).
 *   SPI   SPI2_HOST, one SPI port at a time.
 *   LEDC  timers 1..3 and channels 1..7 (the backlight owns timer 0, channel 0).
 *   ADC   oneshot units, shared by refcount; curve-fitting calibration.
 * Pool bookkeeping runs under the ports service lock (ports.c holds it around
 * open and close).
 */
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "t5_core/log.h"
#include "ports_hw.h"

static const char *TAG = "t5.ports.hw";

static t5_err_t map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return T5_OK;
    case ESP_ERR_TIMEOUT: return T5_ERR_TIMEOUT;
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_NOT_FOUND: return T5_ERR_NACK;
    case ESP_ERR_NO_MEM: return T5_ERR_NO_MEM;
    case ESP_ERR_INVALID_ARG: return T5_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return T5_ERR_BUSY;
    case ESP_ERR_NOT_SUPPORTED: return T5_ERR_NOT_SUPPORTED;
    default: return T5_ERR_IO;
    }
}

typedef enum { OBJ_I2C, OBJ_UART, OBJ_SPI, OBJ_PWM, OBJ_ADC } obj_kind_t;

struct ports_hw_obj {
    obj_kind_t kind;
    union {
        struct {
            i2c_master_bus_handle_t bus;
            i2c_master_dev_handle_t dev;
            uint8_t addr;
            uint32_t hz;
            SemaphoreHandle_t lock;
        } i2c;
        struct {
            uart_port_t num;
        } uart;
        struct {
            spi_device_handle_t dev;
        } spi;
        struct {
            int timer, channel;
            uint8_t bits;
        } pwm;
        struct {
            int unit;
            adc_channel_t ch;
            adc_cali_handle_t cali;
        } adc;
    };
};

static ports_hw_t obj_new(obj_kind_t kind)
{
    ports_hw_t o = calloc(1, sizeof *o);
    if (o) {
        o->kind = kind;
    }
    return o;
}

/* ---- I2C ----------------------------------------------------------------- */
t5_err_t ports_hw_i2c_open(int sda, int scl, uint32_t hz, ports_hw_t *out)
{
    ports_hw_t o = obj_new(OBJ_I2C);
    if (!o || !(o->i2c.lock = xSemaphoreCreateMutex())) {
        free(o);
        return T5_ERR_NO_MEM;
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&cfg, &o->i2c.bus);
    if (e != ESP_OK) {
        T5_LOGW(TAG, "i2c bus on G%d/G%d: %s", sda, scl, esp_err_to_name(e));
        vSemaphoreDelete(o->i2c.lock);
        free(o);
        return e == ESP_ERR_NOT_FOUND ? T5_ERR_BUSY : map_err(e);   /* no free controller */
    }
    o->i2c.hz = hz;
    *out = o;
    return T5_OK;
}

void ports_hw_i2c_close(ports_hw_t h)
{
    if (!h) return;
    if (h->i2c.dev) i2c_master_bus_rm_device(h->i2c.dev);
    i2c_del_master_bus(h->i2c.bus);
    vSemaphoreDelete(h->i2c.lock);
    free(h);
}

t5_err_t ports_hw_i2c_xfer(ports_hw_t h, uint8_t addr, const uint8_t *tx, size_t txlen, uint8_t *rx,
                           size_t rxlen, uint32_t timeout_ms)
{
    xSemaphoreTake(h->i2c.lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (!h->i2c.dev || h->i2c.addr != addr) {
        if (h->i2c.dev) {
            i2c_master_bus_rm_device(h->i2c.dev);
            h->i2c.dev = NULL;
        }
        const i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = h->i2c.hz,
        };
        e = i2c_master_bus_add_device(h->i2c.bus, &dc, &h->i2c.dev);
        h->i2c.addr = addr;
    }
    if (e == ESP_OK) {
        if (txlen && rxlen) {
            e = i2c_master_transmit_receive(h->i2c.dev, tx, txlen, rx, rxlen, (int)timeout_ms);
        } else if (txlen) {
            e = i2c_master_transmit(h->i2c.dev, tx, txlen, (int)timeout_ms);
        } else {
            e = i2c_master_receive(h->i2c.dev, rx, rxlen, (int)timeout_ms);
        }
    }
    xSemaphoreGive(h->i2c.lock);
    return map_err(e);
}

t5_err_t ports_hw_i2c_probe(ports_hw_t h, uint8_t addr, uint32_t timeout_ms)
{
    xSemaphoreTake(h->i2c.lock, portMAX_DELAY);
    esp_err_t e = i2c_master_probe(h->i2c.bus, addr, (int)timeout_ms);
    xSemaphoreGive(h->i2c.lock);
    return map_err(e);
}

/* ---- UART ---------------------------------------------------------------- */
#define UART_FIRST 1
#define UART_LAST  4

t5_err_t ports_hw_uart_open(const ports_hw_uart_cfg_t *c, ports_hw_t *out)
{
    uart_port_t num = UART_NUM_MAX;
    for (int u = UART_FIRST; u <= UART_LAST; u++) {
        if (!uart_is_driver_installed((uart_port_t)u)) {
            num = (uart_port_t)u;
            break;
        }
    }
    if (num == UART_NUM_MAX) {
        return T5_ERR_BUSY;
    }
    ports_hw_t o = obj_new(OBJ_UART);
    if (!o) {
        return T5_ERR_NO_MEM;
    }
    static const uart_word_length_t bits[] = { UART_DATA_5_BITS, UART_DATA_6_BITS, UART_DATA_7_BITS, UART_DATA_8_BITS };
    static const uart_parity_t par[] = { UART_PARITY_DISABLE, UART_PARITY_ODD, UART_PARITY_EVEN };
    const uart_config_t uc = {
        .baud_rate = (int)c->baud,
        .data_bits = bits[c->data_bits - 5],
        .parity = par[c->parity],
        .stop_bits = c->stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    size_t rxbuf = c->rx_buf < 256 ? 256 : c->rx_buf;
    esp_err_t e = uart_driver_install(num, (int)rxbuf, 0, 0, NULL, 0);
    if (e == ESP_OK) e = uart_param_config(num, &uc);
    if (e == ESP_OK) e = uart_set_pin(num, c->tx, c->rx, c->rts >= 0 ? c->rts : UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e == ESP_OK && c->rts >= 0) e = uart_set_mode(num, UART_MODE_RS485_HALF_DUPLEX);
    if (e != ESP_OK) {
        T5_LOGW(TAG, "uart%d: %s", num, esp_err_to_name(e));
        if (uart_is_driver_installed(num)) uart_driver_delete(num);
        free(o);
        return map_err(e);
    }
    o->uart.num = num;
    *out = o;
    return T5_OK;
}

void ports_hw_uart_close(ports_hw_t h)
{
    if (!h) return;
    uart_driver_delete(h->uart.num);
    free(h);
}

int ports_hw_uart_write(ports_hw_t h, const void *data, size_t n)
{
    int r = uart_write_bytes(h->uart.num, data, n);
    return r < 0 ? T5_ERR_IO : r;
}

int ports_hw_uart_read(ports_hw_t h, void *data, size_t n, uint32_t timeout_ms)
{
    int r = uart_read_bytes(h->uart.num, data, (uint32_t)n, pdMS_TO_TICKS(timeout_ms));
    return r < 0 ? T5_ERR_IO : r;
}

t5_err_t ports_hw_uart_flush(ports_hw_t h)
{
    return map_err(uart_flush_input(h->uart.num));
}

t5_err_t ports_hw_uart_wait_tx(ports_hw_t h, uint32_t timeout_ms)
{
    return map_err(uart_wait_tx_done(h->uart.num, pdMS_TO_TICKS(timeout_ms)));
}

/* ---- SPI ----------------------------------------------------------------- */
#define SPI_HOST_PORTS SPI2_HOST
#define SPI_MAX_XFER   4096

t5_err_t ports_hw_spi_open(int sck, int mosi, int miso, int cs, uint32_t hz, uint8_t mode, ports_hw_t *out)
{
    ports_hw_t o = obj_new(OBJ_SPI);
    if (!o) {
        return T5_ERR_NO_MEM;
    }
    const spi_bus_config_t bc = {
        .mosi_io_num = mosi, .miso_io_num = miso, .sclk_io_num = sck,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = SPI_MAX_XFER,
    };
    esp_err_t e = spi_bus_initialize(SPI_HOST_PORTS, &bc, SPI_DMA_CH_AUTO);
    if (e == ESP_OK) {
        const spi_device_interface_config_t dc = {
            .clock_speed_hz = (int)hz, .mode = mode, .spics_io_num = cs, .queue_size = 1,
        };
        e = spi_bus_add_device(SPI_HOST_PORTS, &dc, &o->spi.dev);
        if (e != ESP_OK) spi_bus_free(SPI_HOST_PORTS);
    }
    if (e != ESP_OK) {
        T5_LOGW(TAG, "spi: %s", esp_err_to_name(e));
        free(o);
        return map_err(e);
    }
    *out = o;
    return T5_OK;
}

void ports_hw_spi_close(ports_hw_t h)
{
    if (!h) return;
    spi_bus_remove_device(h->spi.dev);
    spi_bus_free(SPI_HOST_PORTS);
    free(h);
}

t5_err_t ports_hw_spi_transfer(ports_hw_t h, const void *tx, void *rx, size_t n)
{
    const uint8_t *t = tx;
    uint8_t *r = rx;
    while (n) {
        size_t chunk = n > SPI_MAX_XFER ? SPI_MAX_XFER : n;
        spi_transaction_t tr = {
            .length = chunk * 8, .rxlength = rx ? chunk * 8 : 0, .tx_buffer = t, .rx_buffer = r,
        };
        T5_RETURN_ON_ERR(map_err(spi_device_polling_transmit(h->spi.dev, &tr)));
        n -= chunk;
        if (t) t += chunk;
        if (r) r += chunk;
    }
    return T5_OK;
}

/* ---- GPIO ---------------------------------------------------------------- */
t5_err_t ports_hw_gpio_config(int pin, bool output, bool pull_up, bool pull_down, bool open_drain)
{
    const gpio_config_t gc = {
        .pin_bit_mask = 1ULL << pin,
        .mode = output ? (open_drain ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT) : GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return map_err(gpio_config(&gc));
}

t5_err_t ports_hw_gpio_set(int pin, bool level)
{
    return map_err(gpio_set_level((gpio_num_t)pin, level));
}

int ports_hw_gpio_get(int pin)
{
    if (!GPIO_IS_VALID_GPIO(pin)) {
        return -1;
    }
    return gpio_get_level((gpio_num_t)pin);
}

void ports_hw_pin_release(int pin)
{
    if (!GPIO_IS_VALID_GPIO(pin)) {
        return;
    }
    gpio_reset_pin((gpio_num_t)pin);
}

/* ---- PWM (LEDC) ---------------------------------------------------------- */
#define LEDC_T_FIRST 1
#define LEDC_T_COUNT 3      /* timers 1..3 */
#define LEDC_C_FIRST 1
#define LEDC_C_COUNT 7      /* channels 1..7 */

static struct { int users; uint32_t hz; uint8_t bits; } s_timers[LEDC_T_COUNT];
static bool s_channels[LEDC_C_COUNT];

t5_err_t ports_hw_pwm_open(int pin, uint32_t hz, uint8_t bits, ports_hw_t *out)
{
    int t = -1, c = -1;
    for (int i = 0; i < LEDC_T_COUNT && t < 0; i++) {
        if (s_timers[i].users && s_timers[i].hz == hz && s_timers[i].bits == bits) t = i;
    }
    for (int i = 0; i < LEDC_T_COUNT && t < 0; i++) {
        if (!s_timers[i].users) t = i;
    }
    for (int i = 0; i < LEDC_C_COUNT && c < 0; i++) {
        if (!s_channels[i]) c = i;
    }
    if (t < 0 || c < 0) {
        return T5_ERR_BUSY;
    }
    ports_hw_t o = obj_new(OBJ_PWM);
    if (!o) {
        return T5_ERR_NO_MEM;
    }
    esp_err_t e = ESP_OK;
    if (!s_timers[t].users) {
        const ledc_timer_config_t tc = {
            .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = (ledc_timer_bit_t)bits,
            .timer_num = (ledc_timer_t)(LEDC_T_FIRST + t), .freq_hz = hz, .clk_cfg = LEDC_AUTO_CLK,
        };
        e = ledc_timer_config(&tc);
    }
    if (e == ESP_OK) {
        const ledc_channel_config_t cc = {
            .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = (ledc_channel_t)(LEDC_C_FIRST + c),
            .intr_type = LEDC_INTR_DISABLE, .timer_sel = (ledc_timer_t)(LEDC_T_FIRST + t), .duty = 0, .hpoint = 0,
        };
        e = ledc_channel_config(&cc);
    }
    if (e != ESP_OK) {
        T5_LOGW(TAG, "pwm on G%d: %s", pin, esp_err_to_name(e));
        free(o);
        return map_err(e);
    }
    s_timers[t].users++;
    s_timers[t].hz = hz;
    s_timers[t].bits = bits;
    s_channels[c] = true;
    o->pwm.timer = t;
    o->pwm.channel = c;
    o->pwm.bits = bits;
    *out = o;
    return T5_OK;
}

void ports_hw_pwm_close(ports_hw_t h)
{
    if (!h) return;
    ledc_channel_t ch = (ledc_channel_t)(LEDC_C_FIRST + h->pwm.channel);
    ledc_stop(LEDC_LOW_SPEED_MODE, ch, 0);
    const ledc_channel_config_t cc = { .speed_mode = LEDC_LOW_SPEED_MODE, .channel = ch, .deconfigure = true };
    ledc_channel_config(&cc);
    s_channels[h->pwm.channel] = false;
    if (--s_timers[h->pwm.timer].users == 0) {
        ledc_timer_t tn = (ledc_timer_t)(LEDC_T_FIRST + h->pwm.timer);
        ledc_timer_pause(LEDC_LOW_SPEED_MODE, tn);
        const ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = tn, .deconfigure = true };
        ledc_timer_config(&tc);
    }
    free(h);
}

t5_err_t ports_hw_pwm_set(ports_hw_t h, uint32_t duty_permille)
{
    ledc_channel_t ch = (ledc_channel_t)(LEDC_C_FIRST + h->pwm.channel);
    uint32_t duty = (uint32_t)(((uint64_t)duty_permille << h->pwm.bits) / 1000);
    esp_err_t e = ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    if (e == ESP_OK) e = ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
    return map_err(e);
}

/* ---- ADC ----------------------------------------------------------------- */
static struct { adc_oneshot_unit_handle_t h; int users; } s_adc[2];

t5_err_t ports_hw_adc_open(int pin, int unit, int channel, ports_hw_t *out)
{
    if (unit < 1 || unit > 2) {
        return T5_ERR_INVALID_ARG;
    }
    ports_hw_t o = obj_new(OBJ_ADC);
    if (!o) {
        return T5_ERR_NO_MEM;
    }
    adc_unit_t uid = unit == 1 ? ADC_UNIT_1 : ADC_UNIT_2;
    esp_err_t e = ESP_OK;
    if (!s_adc[unit - 1].h) {
        const adc_oneshot_unit_init_cfg_t uc = { .unit_id = uid, .ulp_mode = ADC_ULP_MODE_DISABLE };
        e = adc_oneshot_new_unit(&uc, &s_adc[unit - 1].h);
    }
    if (e == ESP_OK) {
        const adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        e = adc_oneshot_config_channel(s_adc[unit - 1].h, (adc_channel_t)channel, &cc);
    }
    if (e != ESP_OK) {
        T5_LOGW(TAG, "adc G%d: %s", pin, esp_err_to_name(e));
        if (s_adc[unit - 1].h && !s_adc[unit - 1].users) {
            adc_oneshot_del_unit(s_adc[unit - 1].h);
            s_adc[unit - 1].h = NULL;
        }
        free(o);
        return map_err(e);
    }
    s_adc[unit - 1].users++;
    o->adc.unit = unit;
    o->adc.ch = (adc_channel_t)channel;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = uid, .chan = (adc_channel_t)channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &o->adc.cali) != ESP_OK) {
        o->adc.cali = NULL;
        T5_LOGW(TAG, "adc G%d: no calibration, readings are approximate", pin);
    }
#endif
    *out = o;
    return T5_OK;
}

void ports_hw_adc_close(ports_hw_t h)
{
    if (!h) return;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (h->adc.cali) adc_cali_delete_scheme_curve_fitting(h->adc.cali);
#endif
    int u = h->adc.unit - 1;
    if (--s_adc[u].users == 0) {
        adc_oneshot_del_unit(s_adc[u].h);
        s_adc[u].h = NULL;
    }
    free(h);
}

t5_err_t ports_hw_adc_read_mv(ports_hw_t h, int *mv)
{
    adc_oneshot_unit_handle_t u = s_adc[h->adc.unit - 1].h;
    if (h->adc.cali) {
        return map_err(adc_oneshot_get_calibrated_result(u, h->adc.cali, h->adc.ch, mv));
    }
    int raw;
    T5_RETURN_ON_ERR(map_err(adc_oneshot_read(u, h->adc.ch, &raw)));
    *mv = raw * 3300 / 4095;    /* uncalibrated, 12 dB attenuation, 12-bit */
    return T5_OK;
}
