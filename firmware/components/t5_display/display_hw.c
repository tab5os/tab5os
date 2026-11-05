/* SPDX-License-Identifier: Apache-2.0
 * MIPI-DSI bring-up, following espressif/esp-bsp m5stack_tab5 1.3.2.
 */
#include <stdio.h>
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7123.h"
#include "esp_ldo_regulator.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_pins/pins.h"
#include "display_hw.h"
#include "priv/init_ili9881c.h"
#include "priv/init_st7121.h"
#include "priv/init_st7123.h"

static const char *TAG = "t5.display";

#define DSI_LANES 2
#define DSI_PHY_LDO_CHAN 3
#define DSI_PHY_LDO_MV 2500
#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CH LEDC_CHANNEL_0
/* 80 MHz / 1024 / 4 = 19531 Hz: an integer divider, so every PWM period is
 * the same length. 20 kHz needed a /3.9 fractional divider, whose period
 * jitter showed up as the backlight shimmering between two levels. */
#define BL_CLK LEDC_USE_PLL_DIV_CLK
#define BL_FREQ_HZ 19531
#define BL_RES LEDC_TIMER_10_BIT
#define BL_FULL (1u << 10)   /* duty 1024 = constantly on; 1023 still pulses */

static esp_lcd_dsi_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_ldo_channel_handle_t s_ldo;

static t5_err_t chk(esp_err_t e, const char *what)
{
    if (e != ESP_OK) {
        T5_LOGE(TAG, "%s: %s", what, esp_err_to_name(e));
        return T5_ERR_IO;
    }
    return T5_OK;
}

static t5_err_t backlight_timer(uint32_t hz)
{
    const ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = BL_RES,
        .timer_num = BL_LEDC_TIMER, .freq_hz = hz, .clk_cfg = BL_CLK,
    };
    return chk(ledc_timer_config(&t), "backlight timer");
}

t5_err_t t5_display_hw_backlight_freq(uint32_t hz)
{
    T5_RETURN_ON_ERR(backlight_timer(hz));
    T5_LOGI(TAG, "backlight PWM %u Hz requested, %u Hz actual", (unsigned)hz,
            (unsigned)ledc_get_freq(LEDC_LOW_SPEED_MODE, BL_LEDC_TIMER));
    return T5_OK;
}

static t5_err_t backlight_init(void)
{
    const ledc_channel_config_t c = {
        .gpio_num = T5_PIN_LCD_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = BL_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE, .timer_sel = BL_LEDC_TIMER, .duty = 0, .hpoint = 0,
    };
    T5_RETURN_ON_ERR(backlight_timer(BL_FREQ_HZ));
    return chk(ledc_channel_config(&c), "backlight channel");
}

t5_err_t t5_display_hw_backlight(int percent)
{
    uint32_t duty = percent >= 100 ? BL_FULL : (uint32_t)((BL_FULL * percent) / 100);
    T5_RETURN_ON_ERR(chk(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH, duty), "bl duty"));
    return chk(ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH), "bl update");
}

/* Diagnostics: "<pclk_mhz> <vsync_front_porch>" in this file overrides the
 * ST712x DPI timing at boot (set with `disp timing`, removed with `disp timing reset`). */
#define TIMING_OVERRIDE "/data/display_timing.txt"

static void timing_override(esp_lcd_dpi_panel_config_t *dpi)
{
    FILE *f = fopen(TIMING_OVERRIDE, "r");
    if (!f) return;
    unsigned pclk = 0, vfp = 0;
    if (fscanf(f, "%u %u", &pclk, &vfp) == 2 && pclk >= 40 && pclk <= 90 && vfp >= 4 && vfp <= 400) {
        dpi->dpi_clock_freq_mhz = (float)pclk;
        dpi->video_timing.vsync_front_porch = vfp;
        T5_LOGW(TAG, "timing override from " TIMING_OVERRIDE ": %u MHz, vfp %u", pclk, vfp);
    }
    fclose(f);
}

t5_err_t t5_display_hw_init(t5_panel_t panel, uint16_t **fb_out)
{
    T5_RETURN_ON_ERR(backlight_init());
    T5_RETURN_ON_ERR(t5_display_hw_backlight(0));

    const esp_ldo_channel_config_t ldo = { .chan_id = DSI_PHY_LDO_CHAN, .voltage_mv = DSI_PHY_LDO_MV };
    T5_RETURN_ON_ERR(chk(esp_ldo_acquire_channel(&ldo, &s_ldo), "DSI PHY LDO"));

    const esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = panel == T5_PANEL_ST7121 ? 965 : 1000,
    };
    T5_RETURN_ON_ERR(chk(esp_lcd_new_dsi_bus(&bus, &s_bus), "DSI bus"));
    const esp_lcd_dbi_io_config_t dbi = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    T5_RETURN_ON_ERR(chk(esp_lcd_new_panel_io_dbi(s_bus, &dbi, &s_io), "DBI io"));

    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = { .h_size = T5_DISPLAY_W, .v_size = T5_DISPLAY_H },
    };
    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,          /* reset is on the expander, handled by the probe */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_dev_config_t devc = dev;
    esp_err_t e;
    if (panel == T5_PANEL_ILI9881C_GT911) {
        dpi.dpi_clock_freq_mhz = 60;
        dpi.video_timing.hsync_back_porch = 140; dpi.video_timing.hsync_pulse_width = 40;
        dpi.video_timing.hsync_front_porch = 40; dpi.video_timing.vsync_back_porch = 20;
        dpi.video_timing.vsync_pulse_width = 4; dpi.video_timing.vsync_front_porch = 20;
        ili9881c_vendor_config_t v = {
            .init_cmds = disp_init_data_ili9881c,
            .init_cmds_size = sizeof disp_init_data_ili9881c / sizeof disp_init_data_ili9881c[0],
            .mipi_config = { .dsi_bus = s_bus, .dpi_config = &dpi, .lane_num = DSI_LANES },
        };
        devc.vendor_config = &v;
        e = esp_lcd_new_panel_ili9881c(s_io, &devc, &s_panel);
    } else {
        dpi.dpi_clock_freq_mhz = 70;
        dpi.video_timing.hsync_back_porch = 40; dpi.video_timing.hsync_pulse_width = 2;
        dpi.video_timing.hsync_front_porch = 40;
        if (panel == T5_PANEL_ST7121) {
            dpi.video_timing.vsync_back_porch = 24; dpi.video_timing.vsync_pulse_width = 20;
            dpi.video_timing.vsync_front_porch = 200;
        } else {
            /* The BSP's 70 MHz with a 220-line front porch gives 57.8 Hz, and the
             * panel visibly shimmers (Unit A, worst on mid-grey). The long porch is
             * needed: the ST7123 scans touch during vertical blanking, and at 40
             * lines touch stopped interrupting. So keep 220 and raise the pixel
             * clock: 78 MHz = 64.4 Hz, steady and touch works. */
            dpi.video_timing.vsync_back_porch = 8; dpi.video_timing.vsync_pulse_width = 2;
            dpi.video_timing.vsync_front_porch = 220;
            dpi.dpi_clock_freq_mhz = 78;
        }
        timing_override(&dpi);
        st7123_vendor_config_t v = {
            .init_cmds = panel == T5_PANEL_ST7121 ? disp_init_data_st7121 : disp_init_data_st7123,
            .init_cmds_size = panel == T5_PANEL_ST7121
                ? sizeof disp_init_data_st7121 / sizeof disp_init_data_st7121[0]
                : sizeof disp_init_data_st7123 / sizeof disp_init_data_st7123[0],
            .mipi_config = { .dsi_bus = s_bus, .dpi_config = &dpi },
        };
        devc.vendor_config = &v;
        e = esp_lcd_new_panel_st7123(s_io, &devc, &s_panel);
    }
    T5_RETURN_ON_ERR(chk(e, "panel driver"));
    T5_RETURN_ON_ERR(chk(esp_lcd_panel_reset(s_panel), "panel reset"));
    T5_RETURN_ON_ERR(chk(esp_lcd_panel_init(s_panel), "panel init"));
    T5_RETURN_ON_ERR(chk(esp_lcd_panel_disp_on_off(s_panel, true), "panel on"));

    void *fb = NULL;
    T5_RETURN_ON_ERR(chk(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb), "framebuffer"));
    *fb_out = fb;
    const esp_lcd_video_timing_t *vt = &dpi.video_timing;
    uint32_t htot = vt->h_size + vt->hsync_back_porch + vt->hsync_pulse_width + vt->hsync_front_porch;
    uint32_t vtot = vt->v_size + vt->vsync_back_porch + vt->vsync_pulse_width + vt->vsync_front_porch;
    T5_LOGI(TAG, "DSI up: %d lanes, %d MHz pixel clock, %ux%u total, %.1f Hz refresh, fb %p", DSI_LANES,
            (int)dpi.dpi_clock_freq_mhz, (unsigned)htot, (unsigned)vtot,
            dpi.dpi_clock_freq_mhz * 1e6 / ((double)htot * vtot), fb);
    return T5_OK;
}

t5_err_t t5_display_hw_sleep(bool sleep)
{
    if (sleep) {
        T5_RETURN_ON_ERR(chk(esp_lcd_panel_io_tx_param(s_io, 0x28, NULL, 0), "display off"));
        T5_RETURN_ON_ERR(chk(esp_lcd_panel_io_tx_param(s_io, 0x10, NULL, 0), "sleep in"));
        t5_delay_ms(120);
    } else {
        T5_RETURN_ON_ERR(chk(esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0), "sleep out"));
        t5_delay_ms(120);
        T5_RETURN_ON_ERR(chk(esp_lcd_panel_io_tx_param(s_io, 0x29, NULL, 0), "display on"));
    }
    return T5_OK;
}

t5_err_t t5_display_hw_flush(uint16_t *fb, int y0, int y1)
{
    void *start = fb + (size_t)y0 * T5_DISPLAY_W;
    size_t len = (size_t)(y1 - y0) * T5_DISPLAY_W * 2;
    /* The DPI DMA reads PSRAM directly: push dirty cache lines out. */
    return chk(esp_cache_msync(start, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED),
               "cache sync");
}

t5_err_t t5_display_hw_read(uint8_t cmd, uint8_t *buf, int len)
{
    return chk(esp_lcd_panel_io_rx_param(s_io, cmd, buf, len), "panel read");
}
