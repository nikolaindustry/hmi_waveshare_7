#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp.h"
#include "lvgl_port.h"

static const char *TAG = "bsp";

esp_err_t bsp_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(),          TAG, "i2c");
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(),  TAG, "ch422g");

    /* Short delay then switch backlight OFF-and-DISP-high via expander
     * (panel powers up but stays dark until LVGL is ready). */
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(bsp_display_start(),     TAG, "display");

    /* Touch is non-fatal: if GT911 is absent/misbehaving we still want the
     * panel and LVGL up so the user sees something on screen. */
    esp_err_t terr = bsp_touch_init();
    if (terr != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed (%s) - continuing without touch",
                 esp_err_to_name(terr));
    }

    ESP_RETURN_ON_ERROR(lvgl_port_init(),        TAG, "lvgl");

    /* Now that LVGL has painted at least the clear colour, turn on BL */
    bsp_display_backlight(true);

    ESP_LOGI(TAG, "BSP ready%s", (terr == ESP_OK) ? "" : " (no touch)");
    return ESP_OK;
}
