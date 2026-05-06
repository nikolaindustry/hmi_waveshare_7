#include "bus_config.h"
#include "bsp_pins.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "bus_config";

#define NVS_NS  "bus"
#define NVS_KEY "baud"

static uint32_t s_baud = BSP_RS485_DEFAULT_BAUD;
static bool     s_nvs_ready = false;

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

static bool baud_is_supported(uint32_t baud)
{
    return baud == 9600u || baud == 115200u;
}

esp_err_t bus_config_init(void)
{
    esp_err_t err = ensure_nvs();
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved baud, using default %lu", (unsigned long)s_baud);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t stored = 0;
    err = nvs_get_u32(h, NVS_KEY, &stored);
    nvs_close(h);

    if (err == ESP_OK && baud_is_supported(stored)) {
        s_baud = stored;
        ESP_LOGI(TAG, "loaded baud=%lu", (unsigned long)s_baud);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "key not set, keeping default %lu", (unsigned long)s_baud);
        err = ESP_OK;
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "stored baud %lu unsupported, keeping default %lu",
                 (unsigned long)stored, (unsigned long)s_baud);
    } else {
        ESP_LOGW(TAG, "nvs_get_u32: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

uint32_t bus_config_get_baud(void) { return s_baud; }

esp_err_t bus_config_set_baud(uint32_t baud)
{
    if (!baud_is_supported(baud)) return ESP_ERR_INVALID_ARG;
    if (ensure_nvs() != ESP_OK)   return ESP_FAIL;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u32(h, NVS_KEY, baud);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        s_baud = baud;
        ESP_LOGI(TAG, "saved baud=%lu", (unsigned long)baud);
    } else {
        ESP_LOGE(TAG, "nvs_set_u32: %s", esp_err_to_name(err));
    }
    return err;
}
