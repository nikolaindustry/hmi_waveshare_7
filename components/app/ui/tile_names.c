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

/* Flag bitmask keys. One key per group per flag: the flags are edited
 * and saved as a group, so a key per tile would be 16 writes to say
 * what two u16s say. */
static const char *mom_key (int kind) { return (kind == TILE_KIND_SWITCH) ? "mom_s"  : "mom_r";  }
static const char *home_key(int kind) { return (kind == TILE_KIND_SWITCH) ? "home_s" : "home_r"; }

static void load_flags(nvs_handle_t h, int kind)
{
    int n = 0;
    ctrl_toggle_t *arr = (kind == TILE_KIND_SWITCH)
                             ? ctrl_switches(&n)
                             : ctrl_reading_lights(&n);
    uint16_t mom = 0, home = 0;
    (void)nvs_get_u16(h, mom_key(kind),  &mom);   /* missing -> 0 = all off */
    (void)nvs_get_u16(h, home_key(kind), &home);
    for (int i = 0; i < n && i < 16; i++) {
        arr[i].momentary = (mom  >> i) & 1u;
        arr[i].home      = (home >> i) & 1u;
    }
    if (mom || home) {
        ESP_LOGI(TAG, "%s flags: momentary=0x%02X home=0x%02X",
                 (kind == TILE_KIND_SWITCH) ? "switch" : "reading", mom, home);
    }
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
    load_flags(h, TILE_KIND_READING);
    load_flags(h, TILE_KIND_SWITCH);
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

esp_err_t tile_flags_set(int kind, uint16_t momentary_mask, uint16_t home_mask)
{
    int n = 0;
    ctrl_toggle_t *arr = (kind == TILE_KIND_SWITCH)
                             ? ctrl_switches(&n)
                             : ctrl_reading_lights(&n);
    if (!arr) return ESP_ERR_INVALID_ARG;

    /* Apply to the live array first so the UI is right even if the
     * NVS write fails -- same order as tile_names_set(). */
    for (int i = 0; i < n && i < 16; i++) {
        arr[i].momentary = (momentary_mask >> i) & 1u;
        arr[i].home      = (home_mask      >> i) & 1u;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u16(h, mom_key(kind), momentary_mask);
    if (err == ESP_OK) err = nvs_set_u16(h, home_key(kind), home_mask);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved %s flags: momentary=0x%02X home=0x%02X",
                 (kind == TILE_KIND_SWITCH) ? "switch" : "reading",
                 momentary_mask, home_mask);
    } else {
        ESP_LOGE(TAG, "flag save failed: %s", esp_err_to_name(err));
    }
    return err;
}

int tile_home_count(void)
{
    int total = 0;
    for (int k = 0; k < 2; k++) {
        int n = 0;
        ctrl_toggle_t *arr = (k == TILE_KIND_SWITCH)
                                 ? ctrl_switches(&n)
                                 : ctrl_reading_lights(&n);
        for (int i = 0; i < n; i++) if (arr[i].home) total++;
    }
    return total;
}
