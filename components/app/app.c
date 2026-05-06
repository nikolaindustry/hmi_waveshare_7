#include "esp_log.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "bsp.h"
#include "app.h"
#include "splash.h"
#include "ui/ui_shell.h"

static const char *TAG = "app";

/* Called by splash when it finishes. The LVGL task is holding the port
 * mutex here, so we build straight onto the target screen supplied by
 * the splash animation pipeline. */
static void on_splash_done(lv_obj_t *target_scr)
{
    ui_shell_build(target_scr);
    ESP_LOGI(TAG, "shell UI ready");
}

void app_start(void)
{
    ESP_LOGI(TAG, "LVGL app starting");

    /* owner_name_init() runs in app_main BEFORE bsp_init() so NVS flash
     * reads don't race with the RGB panel's DMA EOF ISR. */

    /* All LVGL calls must be wrapped with the port mutex */
    if (!lvgl_port_lock(-1)) {
        ESP_LOGE(TAG, "LVGL lock timeout");
        return;
    }
    splash_show(3000, on_splash_done);
    lvgl_port_unlock();
}
