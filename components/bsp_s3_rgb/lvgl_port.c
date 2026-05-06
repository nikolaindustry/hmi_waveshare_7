#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"
#include "bsp.h"
#include "lvgl_port.h"

static const char *TAG = "lvgl_port";

#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_STACK        8192
#define LVGL_TASK_PRIO         2
#define LVGL_TASK_CORE         1
#define LVGL_BUF_LINES         40                  /* lines per draw buf */

static SemaphoreHandle_t s_lvgl_mux = NULL;
static TaskHandle_t      s_lvgl_task = NULL;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;
static lv_disp_t         *s_disp = NULL;

/* ---- Display flush ---- */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

/* ---- Touch read ---- */
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    uint16_t x[1] = {0}, y[1] = {0}, strength[1] = {0};
    uint8_t cnt = 0;

    esp_lcd_touch_read_data(tp);
    bool pressed = esp_lcd_touch_get_coordinates(tp, x, y, strength, &cnt, 1);
    if (pressed && cnt > 0) {
        data->point.x = x[0];
        data->point.y = y[0];
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state   = LV_INDEV_STATE_RELEASED;
    }
}

/* ---- Tick ---- */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/* ---- Handler task ---- */
static void lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task on core %d", xPortGetCoreID());
    uint32_t next_ms = 10;
    for (;;) {
        if (lvgl_port_lock(-1)) {
            next_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (next_ms > 100) next_ms = 100;
        if (next_ms < 2)   next_ms = 2;
        vTaskDelay(pdMS_TO_TICKS(next_ms));
    }
}

/* ---- Public API ---- */
bool lvgl_port_lock(int timeout_ms)
{
    TickType_t t = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, t) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mux);
}

esp_err_t lvgl_port_init(void)
{
    esp_lcd_panel_handle_t panel = bsp_display_get_panel();
    esp_lcd_touch_handle_t touch = bsp_touch_get_handle();
    if (!panel) {
        ESP_LOGE(TAG, "panel not ready");
        return ESP_ERR_INVALID_STATE;
    }

    lv_init();

    /* Draw buffers in PSRAM (2 x H_RES * LINES * bpp) */
    size_t buf_px = BSP_LCD_H_RES * LVGL_BUF_LINES;
    lv_color_t *buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "draw buf alloc failed (%zu bytes each)",
                 buf_px * sizeof(lv_color_t));
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, buf_px);

    /* Display driver */
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res   = BSP_LCD_H_RES;
    s_disp_drv.ver_res   = BSP_LCD_V_RES;
    s_disp_drv.flush_cb  = lvgl_flush_cb;
    s_disp_drv.draw_buf  = &s_draw_buf;
    s_disp_drv.user_data = panel;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    /* Touch indev (only if touch init succeeded) */
    if (touch) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type      = LV_INDEV_TYPE_POINTER;
        s_indev_drv.disp      = s_disp;
        s_indev_drv.read_cb   = lvgl_touch_read_cb;
        s_indev_drv.user_data = touch;
        lv_indev_drv_register(&s_indev_drv);
    } else {
        ESP_LOGW(TAG, "touch not ready, pointer input disabled");
    }

    /* Tick timer */
    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick_cb,
        .name     = "lv_tick",
    };
    esp_timer_handle_t tick = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "timer_create");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_PERIOD_MS * 1000),
                        TAG, "timer_start");

    /* Mutex + handler task */
    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_lvgl_mux);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL,
                            LVGL_TASK_PRIO, &s_lvgl_task, LVGL_TASK_CORE);

    ESP_LOGI(TAG, "LVGL up (draw buf %zu KB x2 in PSRAM)",
             (buf_px * sizeof(lv_color_t)) / 1024);
    return ESP_OK;
}
