#include "esp_check.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "bsp.h"
#include "bsp_pins.h"

static const char *TAG = "bsp_display";
static esp_lcd_panel_handle_t s_panel = NULL;

esp_err_t bsp_display_start(void)
{
    if (s_panel) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Creating RGB LCD panel (%dx%d @ %d Hz)",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_PCLK_HZ);

    esp_lcd_rgb_panel_config_t panel_conf = {
        .clk_src            = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz            = BSP_LCD_PCLK_HZ,
            .h_res              = BSP_LCD_H_RES,
            .v_res              = BSP_LCD_V_RES,
            .hsync_pulse_width  = BSP_LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch   = BSP_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch  = BSP_LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width  = BSP_LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch   = BSP_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch  = BSP_LCD_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = true,
        },
        .data_width         = BSP_LCD_DATA_WIDTH,
        /* Color-format fields differ across IDF versions: 6.0 introduced
         * in_color_format/out_color_format; 5.x uses bits_per_pixel.
         * Guard so the BSP builds on both (5.5 for HSC, 6.0 otherwise). */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .out_color_format   = LCD_COLOR_FMT_RGB565,
#else
        .bits_per_pixel     = BSP_LCD_BIT_PER_PIXEL,
#endif
        .num_fbs            = 2,
        .bounce_buffer_size_px = BSP_LCD_H_RES * BSP_LCD_BOUNCE_BUFFER_HEIGHT,
        .hsync_gpio_num     = BSP_LCD_IO_HSYNC,
        .vsync_gpio_num     = BSP_LCD_IO_VSYNC,
        .de_gpio_num        = BSP_LCD_IO_DE,
        .pclk_gpio_num      = BSP_LCD_IO_PCLK,
        .disp_gpio_num      = BSP_LCD_IO_DISP,
        .data_gpio_nums = {
            BSP_LCD_IO_DATA0,  BSP_LCD_IO_DATA1,  BSP_LCD_IO_DATA2,  BSP_LCD_IO_DATA3,
            BSP_LCD_IO_DATA4,  BSP_LCD_IO_DATA5,  BSP_LCD_IO_DATA6,  BSP_LCD_IO_DATA7,
            BSP_LCD_IO_DATA8,  BSP_LCD_IO_DATA9,  BSP_LCD_IO_DATA10, BSP_LCD_IO_DATA11,
            BSP_LCD_IO_DATA12, BSP_LCD_IO_DATA13, BSP_LCD_IO_DATA14, BSP_LCD_IO_DATA15,
        },
        .flags.fb_in_psram  = true,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_conf, &s_panel),
                        TAG, "new_rgb_panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init");

    ESP_LOGI(TAG, "RGB panel up");
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_display_get_panel(void)
{
    return s_panel;
}
