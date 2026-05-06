#include "hmi_role.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "hmi_role";

#define NVS_NS  "hmi"
#define NVS_KEY "role"

static hmi_role_t s_role    = HMI_ROLE_PRIMARY;
static bool       s_inited  = false;

esp_err_t hmi_role_init(void)
{
    if (s_inited) return ESP_OK;

    /* owner_name_init() is expected to have already brought NVS up.
     * Guard anyway so either call order works. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs erase+reinit");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s, defaulting to PRIMARY",
                 esp_err_to_name(err));
        s_role   = HMI_ROLE_PRIMARY;
        s_inited = true;
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved role, default PRIMARY");
        s_role   = HMI_ROLE_PRIMARY;
        s_inited = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro: %s", esp_err_to_name(err));
        s_role   = HMI_ROLE_PRIMARY;
        s_inited = true;
        return err;
    }

    uint8_t v = HMI_ROLE_PRIMARY;
    err = nvs_get_u8(h, NVS_KEY, &v);
    nvs_close(h);

    if (err == ESP_OK) {
        s_role = (v == HMI_ROLE_SECONDARY) ? HMI_ROLE_SECONDARY
                                           : HMI_ROLE_PRIMARY;
        ESP_LOGI(TAG, "loaded role=%s", hmi_role_str(s_role));
    } else {
        ESP_LOGI(TAG, "key not set, default PRIMARY");
        s_role = HMI_ROLE_PRIMARY;
    }
    s_inited = true;
    return ESP_OK;
}

hmi_role_t hmi_role_get(void) { return s_role; }

esp_err_t hmi_role_set(hmi_role_t role)
{
    if (!s_inited) hmi_role_init();
    if (role != HMI_ROLE_PRIMARY && role != HMI_ROLE_SECONDARY) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY, (uint8_t)role);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        s_role = role;
        ESP_LOGI(TAG, "saved role=%s", hmi_role_str(role));
    } else {
        ESP_LOGE(TAG, "nvs_set_u8: %s", esp_err_to_name(err));
    }
    return err;
}

const char *hmi_role_str(hmi_role_t role)
{
    switch (role) {
        case HMI_ROLE_PRIMARY:   return "PRIMARY";
        case HMI_ROLE_SECONDARY: return "SECONDARY";
        default:                 return "?";
    }
}
