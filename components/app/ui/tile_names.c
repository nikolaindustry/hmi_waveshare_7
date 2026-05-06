#include "tile_names.h"
#include "ctrl_state.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "tile_names";

#define NVS_NS "tiles"

/* Build the 2-char NVS key for a tile. */
static void tile_key(int kind, int idx, char out[4])
{
    out[0] = (kind == TILE_KIND_SWITCH) ? 's' : 'r';
    out[1] = (char)('0' + idx);
    out[2] = '\0';
}

/* Resolve (kind, idx) to the live ctrl_state slot so we can mutate
 * its name[] buffer directly. Returns NULL on out-of-range. */
static ctrl_toggle_t *resolve(int kind, int idx)
{
    int n = 0;
    ctrl_toggle_t *arr = (kind == TILE_KIND_SWITCH)
                             ? ctrl_switches(&n)
                             : ctrl_reading_lights(&n);
    if (idx < 0 || idx >= n) return NULL;
    return &arr[idx];
}

static void load_group(nvs_handle_t h, int kind)
{
    int n = 0;
    ctrl_toggle_t *arr = (kind == TILE_KIND_SWITCH)
                             ? ctrl_switches(&n)
                             : ctrl_reading_lights(&n);
    for (int i = 0; i < n; i++) {
        char key[4];
        tile_key(kind, i, key);
        size_t len = sizeof(arr[i].name);
        esp_err_t err = nvs_get_str(h, key, arr[i].name, &len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "loaded %s = \"%s\"", key, arr[i].name);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_get_str %s: %s", key, esp_err_to_name(err));
        }
        /* key missing -> keep compile-time default in the array */
    }
}

esp_err_t tile_names_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* namespace does not exist yet -> all defaults */
        ESP_LOGI(TAG, "no saved tile names, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro: %s", esp_err_to_name(err));
        return err;
    }
    load_group(h, TILE_KIND_READING);
    load_group(h, TILE_KIND_SWITCH);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t tile_names_set(int kind, int idx, const char *name)
{
    if (!name || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    ctrl_toggle_t *slot = resolve(kind, idx);
    if (!slot) return ESP_ERR_INVALID_ARG;

    /* copy-truncate into the ctrl_state buffer first so the UI
     * picks up the new label even if NVS write fails */
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return err;
    }
    char key[4];
    tile_key(kind, idx, key);
    err = nvs_set_str(h, key, slot->name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved %s = \"%s\"", key, slot->name);
    } else {
        ESP_LOGE(TAG, "nvs_set_str %s: %s", key, esp_err_to_name(err));
    }
    return err;
}
