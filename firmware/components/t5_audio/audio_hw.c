/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "t5_bsp/xpin.h"
#include "t5_core/log.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/pins.h"
#include "t5_power/rail.h"
#include "audio_hw.h"

static const char *TAG = "t5.audio";

#define AUDIO_HW_CLK_SRC I2S_CLK_SRC_APLL

/* esp_codec_dev register access through the bus broker, so codec traffic is
 * serialised with every other device and counted in `i2c stats`. */
typedef struct {
    audio_codec_ctrl_if_t base;
    t5_i2c_dev_t *dev;
} t5_ctrl_t;

static int c_open(const audio_codec_ctrl_if_t *c, void *cfg, int sz)
{
    (void)c; (void)cfg; (void)sz;
    return ESP_CODEC_DEV_OK;
}

static bool c_is_open(const audio_codec_ctrl_if_t *c)
{
    return ((const t5_ctrl_t *)c)->dev != NULL;
}

static int c_read(const audio_codec_ctrl_if_t *c, int reg, int reg_len, void *data, int len)
{
    const t5_ctrl_t *t = (const t5_ctrl_t *)c;
    if (reg_len != 1) return ESP_CODEC_DEV_NOT_SUPPORT;
    return t5_i2c_read(t->dev, (uint8_t)reg, data, (size_t)len) == T5_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_READ_FAIL;
}

static int c_write(const audio_codec_ctrl_if_t *c, int reg, int reg_len, void *data, int len)
{
    const t5_ctrl_t *t = (const t5_ctrl_t *)c;
    if (reg_len != 1) return ESP_CODEC_DEV_NOT_SUPPORT;
    return t5_i2c_write(t->dev, (uint8_t)reg, data, (size_t)len) == T5_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static int c_close(const audio_codec_ctrl_if_t *c)
{
    (void)c;
    return ESP_CODEC_DEV_OK;
}

static t5_ctrl_t s_dac_ctrl, s_adc_ctrl;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_data_if_t *s_data;
static esp_codec_dev_handle_t s_out, s_in;
static bool s_out_open, s_in_open;
static t5_rail_hold_t *s_spk;

static void ctrl_init(t5_ctrl_t *t, t5_i2c_dev_t *dev)
{
    t->base.open = c_open;
    t->base.is_open = c_is_open;
    t->base.read_reg = c_read;
    t->base.write_reg = c_write;
    t->base.close = c_close;
    t->dev = dev;
}

static esp_codec_dev_sample_info_t fs(void)
{
    esp_codec_dev_sample_info_t f = {
        .bits_per_sample = 16, .channel = T5_AUDIO_CH, .sample_rate = T5_AUDIO_RATE, .mclk_multiple = 256,
    };
    return f;
}

t5_err_t audio_hw_init(bool *dac_ok, bool *adc_ok)
{
    *dac_ok = *adc_ok = false;
    t5_i2c_dev_t *dac = NULL, *adc = NULL;
    if (t5_i2c_probe(T5_I2C_ADDR_ES8388) == T5_OK &&
        t5_i2c_open(T5_I2C_ADDR_ES8388, T5_SYS_I2C_HZ, "es8388", &dac) == T5_OK) {
        ctrl_init(&s_dac_ctrl, dac);
    }
    if (t5_i2c_probe(T5_I2C_ADDR_ES7210) == T5_OK &&
        t5_i2c_open(T5_I2C_ADDR_ES7210, T5_SYS_I2C_HZ, "es7210", &adc) == T5_OK) {
        ctrl_init(&s_adc_ctrl, adc);
    }
    if (!dac && !adc) return T5_ERR_NOT_FOUND;

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = AUDIO_HW_DMA_DESC;
    cc.dma_frame_num = AUDIO_HW_DMA_FRAMES;
    cc.auto_clear = true;            /* silence, not old samples, on underrun */
    if (i2s_new_channel(&cc, &s_tx, &s_rx) != ESP_OK) return T5_ERR_IO;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(T5_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = T5_PIN_I2S_MCLK, .bclk = T5_PIN_I2S_SCLK, .ws = T5_PIN_I2S_LRCK,
            .dout = T5_PIN_I2S_DOUT, .din = T5_PIN_I2S_DIN,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    /* MCLK from the APLL (24.576 MHz / 2), not the default. Before P4 v3 the
     * default is the 40 MHz XTAL, and 12.288 MHz from it is a 3.255 fractional
     * divide: MCLK periods alternate between 75 and 100 ns, and the ES8388's
     * DAC turns that jitter into hiss under anything playing. */
    std.clk_cfg.clk_src = AUDIO_HW_CLK_SRC;
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK || i2s_channel_init_std_mode(s_rx, &std) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK || i2s_channel_enable(s_rx) != ESP_OK) {
        return T5_ERR_IO;
    }
    /* esp_codec_dev sets the clock again on every open: the same source. */
    audio_codec_i2s_cfg_t ic = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx,
                                 .clk_src = AUDIO_HW_CLK_SRC };
    s_data = audio_codec_new_i2s_data(&ic);
    if (!s_data) return T5_ERR_NO_MEM;

    if (dac) {
        es8388_codec_cfg_t c = {
            .ctrl_if = &s_dac_ctrl.base,
            .gpio_if = audio_codec_new_gpio(),
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
            .pa_pin = -1,                     /* the amp is SPK_EN on the expander: a rail */
            .master_mode = false,
            .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
        };
        const audio_codec_if_t *ci = es8388_codec_new(&c);
        if (ci) {
            esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = ci, .data_if = s_data };
            s_out = esp_codec_dev_new(&dc);
        }
        if (s_out) {
            esp_codec_set_disable_when_closed(s_out, true);   /* closed = powered down by register */
            *dac_ok = true;
        }
    }
    if (adc) {
        es7210_codec_cfg_t c = {
            .ctrl_if = &s_adc_ctrl.base,
            .master_mode = false,
            .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,   /* the two mics, as L and R */
        };
        const audio_codec_if_t *ci = es7210_codec_new(&c);
        if (ci) {
            esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = ci, .data_if = s_data };
            s_in = esp_codec_dev_new(&dc);
        }
        if (s_in) {
            esp_codec_set_disable_when_closed(s_in, true);
            *adc_ok = true;
        }
    }
    /* Both codecs start powered down: open and close once to put them in a
     * known state (the factory firmware may have left them running). */
    if (s_out) {
        esp_codec_dev_sample_info_t f = fs();
        if (esp_codec_dev_open(s_out, &f) == ESP_CODEC_DEV_OK) esp_codec_dev_close(s_out);
    }
    if (s_in) {
        esp_codec_dev_sample_info_t f = fs();
        if (esp_codec_dev_open(s_in, &f) == ESP_CODEC_DEV_OK) esp_codec_dev_close(s_in);
    }
    T5_LOGI(TAG, "ES8388 %s, ES7210 %s", *dac_ok ? "ok" : "missing", *adc_ok ? "ok" : "missing");
    return T5_OK;
}

t5_err_t audio_hw_out_open(int volume, bool muted)
{
    if (!s_out) return T5_ERR_NOT_FOUND;
    if (s_out_open) return T5_OK;
    esp_codec_dev_sample_info_t f = fs();
    if (esp_codec_dev_open(s_out, &f) != ESP_CODEC_DEV_OK) return T5_ERR_IO;
    s_out_open = true;
    esp_codec_dev_set_out_vol(s_out, volume);
    esp_codec_dev_set_out_mute(s_out, muted);
    return T5_OK;
}

void audio_hw_out_close(void)
{
    if (!s_out_open) return;
    audio_hw_speaker(false);
    esp_codec_dev_close(s_out);
    s_out_open = false;
}

t5_err_t audio_hw_write(const int16_t *pcm, size_t bytes)
{
    if (!s_out_open) return T5_ERR_STATE;
    return esp_codec_dev_write(s_out, (void *)pcm, (int)bytes) == ESP_CODEC_DEV_OK ? T5_OK : T5_ERR_IO;
}

t5_err_t audio_hw_in_open(float gain_db)
{
    if (!s_in) return T5_ERR_NOT_FOUND;
    if (s_in_open) return T5_OK;
    esp_codec_dev_sample_info_t f = fs();
    if (esp_codec_dev_open(s_in, &f) != ESP_CODEC_DEV_OK) return T5_ERR_IO;
    s_in_open = true;
    esp_codec_dev_set_in_gain(s_in, gain_db);
    return T5_OK;
}

void audio_hw_in_close(void)
{
    if (!s_in_open) return;
    esp_codec_dev_close(s_in);
    s_in_open = false;
}

t5_err_t audio_hw_read(int16_t *pcm, size_t bytes)
{
    if (!s_in_open) return T5_ERR_STATE;
    return esp_codec_dev_read(s_in, pcm, (int)bytes) == ESP_CODEC_DEV_OK ? T5_OK : T5_ERR_IO;
}

void audio_hw_set_volume(int pct)
{
    if (s_out_open) esp_codec_dev_set_out_vol(s_out, pct);
}

void audio_hw_set_mute(bool on)
{
    if (s_out_open) esp_codec_dev_set_out_mute(s_out, on);
}

void audio_hw_set_in_gain(float db)
{
    if (s_in_open) esp_codec_dev_set_in_gain(s_in, db);
}

bool audio_hw_headphones(void)
{
    bool level = false;
    /* HP_DET reads high with a plug in (confirmed on Unit A, 2026-11-09). */
    if (t5_xpin_read(T5_XP_HP_DET, &level) != T5_OK) return false;
    return level;
}

t5_err_t audio_hw_speaker(bool on)
{
    if (on && !s_spk) return t5_rail_acquire_system(T5_RAIL_SPK, "audio", 0, &s_spk);
    if (!on && s_spk) {
        t5_err_t e = t5_rail_release(s_spk);
        s_spk = NULL;
        return e;
    }
    return T5_OK;
}
