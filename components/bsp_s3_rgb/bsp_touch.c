#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "bsp.h"
#include "bsp_pins.h"

static const char *TAG = "bsp_touch";
static esp_lcd_touch_handle_t s_touch = NULL;

/* GT911 I2C address select:
 *  - INT held LOW at reset release -> 0x5D (primary)
 *  - INT held HIGH at reset release -> 0x14
 * Waveshare board has TP_RST wired through CH422G and uses GPIO4 as a
 * transient OUTPUT during reset to force 0x5D.
 */
static void gt911_force_addr_5d(void)
{
    /* 1. Drive GPIO4 high briefly (Waveshare sequence), then low */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BSP_TOUCH_RESET_IO4),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* Hold touch in reset via CH422G bit TP_RST = 0 */
    bsp_io_expander_set(CH422G_IO_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    /* INT pin held LOW during reset release -> selects 0x5D */
    gpio_set_level(BSP_TOUCH_RESET_IO4, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Release TP reset */
    bsp_io_expander_set(CH422G_IO_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Return GPIO4 to input (no INT used) */
    gpio_reset_pin(BSP_TOUCH_RESET_IO4);
    gpio_set_direction(BSP_TOUCH_RESET_IO4, GPIO_MODE_INPUT);
}

esp_err_t bsp_touch_init(void)
{
    if (s_touch) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initialising GT911 touch");
    gt911_force_addr_5d();

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    /* IDF v6's new i2c_master driver requires scl_speed_hz to be set.
     * The GT911 config macro doesn't set it (was implicit in legacy driver),
     * so override it here. Default GT911 I2C max is 400 kHz. */
    tp_io_cfg.scl_speed_hz = BSP_I2C_FREQ_HZ;
    /* v6 API: pass the i2c_master_bus_handle_t created by bsp_i2c_init() */
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bsp_i2c_get_bus_handle(),
                                 &tp_io_cfg, &tp_io),
        TAG, "new_panel_io_i2c");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_RST,     /* -1 (via CH422G) */
        .int_gpio_num = BSP_TOUCH_INT,     /* -1 (polling) */
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch),
                        TAG, "new_i2c_gt911");

    ESP_LOGI(TAG, "GT911 ready");
    return ESP_OK;
}

esp_lcd_touch_handle_t bsp_touch_get_handle(void)
{
    return s_touch;
}
