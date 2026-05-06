#include "owner_name.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "owner_name";

#define NVS_NS  "cfg"
#define NVS_KEY "owner"

static char             s_name[OWNER_NAME_MAX] = "Owner";
static owner_name_cb_t  s_cb = NULL;
static bool             s_nvs_ready = false;

static esp_err_t ensure_nvs(void)
{
    if (s_nvs_ready) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* partition corrupt or schema changed -> wipe and retry */
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

esp_err_t owner_name_init(void)
{
    esp_err_t err = ensure_nvs();
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* namespace does not exist yet -> keep default */
        ESP_LOGI(TAG, "no saved owner name, using default \"%s\"", s_name);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_name);
    err = nvs_get_str(h, NVS_KEY, s_name, &len);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded owner=\"%s\"", s_name);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "key not set, keeping default");
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "nvs_get_str: %s", esp_err_to_name(err));
    }
    return err;
}

const char *owner_name_get(void) { return s_name; }

esp_err_t owner_name_set(const char *new_name)
{
    if (!new_name) return ESP_ERR_INVALID_ARG;

    /* copy-truncate into our buffer */
    strncpy(s_name, new_name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    if (ensure_nvs() != ESP_OK) return ESP_FAIL;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, NVS_KEY, s_name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved owner=\"%s\"", s_name);
        if (s_cb) s_cb(s_name);
    } else {
        ESP_LOGE(TAG, "nvs_set_str: %s", esp_err_to_name(err));
    }
    return err;
}

void owner_name_listen(owner_name_cb_t cb) { s_cb = cb; }
