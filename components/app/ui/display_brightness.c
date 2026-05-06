#include "display_brightness.h"
#include "bsp.h"                 /* bsp_display_backlight() */
#include "lvgl_port.h"            /* lvgl_port_lock/unlock (recursive) */
#include "lvgl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG     = "disp_bri";
static const char *NVS_NS  = "disp";
static const char *NVS_KEY = "bri";

static uint8_t s_level        = 100;   /* 0..100, default full */
static bool    s_nvs_ready    = false;

/* ---- helpers ---- */

static uint8_t clamp_level(int v)
{
    if (v < 0)   return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

static esp_err_t ensure_nvs(void)
{
    if (s_nvs_ready) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs erase+reinit");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        s_nvs_ready = true;
    } else {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Paint the dimming overlay on lv_layer_sys() and switch the backlight.
 * For level == 0 the backlight is turned off; for 1..100 we darken by
 * (100-level)% via opaque-black compositing on the always-top sys layer.
 *
 * MUST hold the LVGL port mutex while touching lv_* state. The mutex is
 * recursive (xSemaphoreCreateRecursiveMutex in lvgl_port.c), so calling
 * this from inside an LVGL event callback (which already holds the lock)
 * is safe. */
static void apply_level(uint8_t level)
{
    if (level == 0) {
        /* Fully dark. Painting opaque black under the backlight-off makes
         * a glitch-free transition even if the backlight takes a few ms. */
        if (lvgl_port_lock(-1)) {
            lv_obj_t *sys = lv_layer_sys();
            if (sys) {
                lv_obj_set_style_bg_color(sys, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_opa  (sys, LV_OPA_COVER, 0);
            }
            lvgl_port_unlock();
        }
        bsp_display_backlight(false);
        return;
    }

    if (lvgl_port_lock(-1)) {
        lv_obj_t *sys = lv_layer_sys();
        if (sys) {
            uint32_t opa = ((100u - (uint32_t)level) * 255u) / 100u;
            lv_obj_set_style_bg_color(sys, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa  (sys, (lv_opa_t)opa, 0);
        }
        lvgl_port_unlock();
    }
    bsp_display_backlight(true);
}

/* ---- API ---- */

void display_brightness_init(void)
{
    if (ensure_nvs() != ESP_OK) {
        s_level = 100;
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved brightness, using default %u", s_level);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro: %s", esp_err_to_name(err));
        return;
    }

    int32_t stored = 100;
    err = nvs_get_i32(h, NVS_KEY, &stored);
    nvs_close(h);

    if (err == ESP_OK) {
        s_level = clamp_level((int)stored);
        ESP_LOGI(TAG, "loaded brightness=%u", s_level);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "key not set, keeping default %u", s_level);
    } else {
        ESP_LOGW(TAG, "nvs_get_i32: %s", esp_err_to_name(err));
    }
}

void display_brightness_apply(void)
{
    apply_level(s_level);
    ESP_LOGI(TAG, "applied brightness=%u", s_level);
}

uint8_t display_brightness_get(void)
{
    return s_level;
}

void display_brightness_preview(uint8_t level_0_100)
{
    uint8_t clamped = clamp_level((int)level_0_100);
    s_level = clamped;
    apply_level(clamped);
}

esp_err_t display_brightness_set(uint8_t level_0_100)
{
    uint8_t clamped = clamp_level((int)level_0_100);
    s_level = clamped;
    apply_level(clamped);

    if (ensure_nvs() != ESP_OK) return ESP_FAIL;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(h, NVS_KEY, (int32_t)clamped);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved brightness=%u", clamped);
    } else {
        ESP_LOGE(TAG, "nvs save failed: %s", esp_err_to_name(err));
    }
    return err;
}
