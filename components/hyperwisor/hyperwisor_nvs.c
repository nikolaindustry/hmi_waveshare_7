/**
 * @file hyperwisor_nvs.c
 * @brief NVS persistent storage implementation
 */

#include "hyperwisor_nvs.h"
#include "hyperwisor_core.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "HYPER_NVS";
static nvs_handle_t s_nvs_handle = 0;
static bool s_nvs_initialized = false;

esp_err_t hyperwisor_nvs_init(void)
{
    if (s_nvs_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open(HYPERWISOR_NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    s_nvs_initialized = true;
    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}

esp_err_t hyperwisor_nvs_get_str(const char *key, char *out_buf, size_t buf_len)
{
    if (!s_nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_get_str(s_nvs_handle, key, out_buf, &buf_len);
}

esp_err_t hyperwisor_nvs_set_str(const char *key, const char *value)
{
    if (!s_nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_str(s_nvs_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    return err;
}

esp_err_t hyperwisor_nvs_get_i32(const char *key, int32_t *out_value)
{
    if (!s_nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_get_i32(s_nvs_handle, key, out_value);
}

esp_err_t hyperwisor_nvs_set_i32(const char *key, int32_t value)
{
    if (!s_nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_set_i32(s_nvs_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    return err;
}

esp_err_t hyperwisor_nvs_commit(void)
{
    if (!s_nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_commit(s_nvs_handle);
}
