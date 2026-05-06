/* =====================================================================
 *  hyperwisor_app.c
 *
 *  Application glue between the display-agnostic `hyperwisor` component
 *  and our domain objects (ctrl_state, bmp280, hvac_controller, ac_state,
 *  modbus_task). Lives in the `app` component so it can depend on those
 *  modules without causing a circular dependency with the library.
 * ===================================================================== */

#include "hyperwisor_app.h"
#include "hyperwisor.h"                 /* umbrella: core + ws + cmd + widget + nvs */

#include "ctrl_state.h"
#include "ac_state.h"
#include "bmp280.h"
#include "hvac_controller.h"
#include "hvac_config.h"
#include "modbus_task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "HYPER_APP";

#define MAX_USER_RELAYS   8     /* coils 0..7 are user-controllable; 8..15 HVAC-owned */
#define MAX_WIDGET_ID_LEN 64

/* RGB slave register map. Must stay in sync with slave_rgb_arduino.ino,
 * ui_tab_control.c and hmi_sync.c. The Arduino slave exposes
 *   regs 0..5   = ROOF  (R, G, B, MODE, SPEED, BRIGHTNESS)
 *   regs 6..11  = FLOOR (same layout)
 * We only write the first 4 registers per zone ([R, G, B, MODE]); the
 * slave keeps its defaults for SPEED/BRIGHTNESS. */
#define CLOUD_RGB_SLAVE_ADDR        0x20
#define CLOUD_RGB_START_REG_ROOF    0
#define CLOUD_RGB_START_REG_FLOOR   6

/* ---- Cached widget bindings (loaded from NVS in init, updated by "config" cmd) ---- */
static char s_target_id[MAX_WIDGET_ID_LEN]            = {0};
static char s_relay_widget[MAX_USER_RELAYS][MAX_WIDGET_ID_LEN] = {{0}};
static char s_w_temp[MAX_WIDGET_ID_LEN]               = {0};
static char s_w_hpa[MAX_WIDGET_ID_LEN]                = {0};
static char s_w_hvac_mode[MAX_WIDGET_ID_LEN]          = {0};
static char s_w_hvac_fan[MAX_WIDGET_ID_LEN]           = {0};
static char s_w_hvac_clutch[MAX_WIDGET_ID_LEN]        = {0};
static char s_w_hvac_cur[MAX_WIDGET_ID_LEN]           = {0};
static char s_w_hvac_set[MAX_WIDGET_ID_LEN]           = {0};

static nvs_handle_t s_app_nvs = 0;
static bool s_nvs_open = false;

/* ==================== NVS helpers ==================== */

static void nvs_ensure_open(void)
{
    if (s_nvs_open) return;
    esp_err_t err = nvs_open(HYPERWISOR_APP_NVS_NS, NVS_READWRITE, &s_app_nvs);
    if (err == ESP_OK) {
        s_nvs_open = true;
    } else {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", HYPERWISOR_APP_NVS_NS, esp_err_to_name(err));
    }
}

static void nvs_load_str(const char *key, char *dst, size_t dst_len)
{
    if (!s_nvs_open) return;
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(s_app_nvs, key, dst, &len);
    if (err != ESP_OK) {
        dst[0] = '\0';
    }
}

static void nvs_save_str(const char *key, const char *value)
{
    if (!s_nvs_open || !key || !value) return;
    nvs_set_str(s_app_nvs, key, value);
    nvs_commit(s_app_nvs);
}

static void load_cached_bindings(void)
{
    nvs_ensure_open();
    nvs_load_str(HYPERWISOR_APP_KEY_TARGET,   s_target_id,    sizeof(s_target_id));
    nvs_load_str(HYPERWISOR_APP_KEY_W_TEMP,   s_w_temp,       sizeof(s_w_temp));
    nvs_load_str(HYPERWISOR_APP_KEY_W_HPA,    s_w_hpa,        sizeof(s_w_hpa));
    nvs_load_str(HYPERWISOR_APP_KEY_W_MODE,   s_w_hvac_mode,  sizeof(s_w_hvac_mode));
    nvs_load_str(HYPERWISOR_APP_KEY_W_FAN,    s_w_hvac_fan,   sizeof(s_w_hvac_fan));
    nvs_load_str(HYPERWISOR_APP_KEY_W_CLUTCH, s_w_hvac_clutch, sizeof(s_w_hvac_clutch));
    nvs_load_str(HYPERWISOR_APP_KEY_W_CUR,    s_w_hvac_cur,   sizeof(s_w_hvac_cur));
    nvs_load_str(HYPERWISOR_APP_KEY_W_SET,    s_w_hvac_set,   sizeof(s_w_hvac_set));

    for (int i = 0; i < MAX_USER_RELAYS; i++) {
        char key[16];
        snprintf(key, sizeof(key), HYPERWISOR_APP_KEY_RELAY_FMT, i);
        nvs_load_str(key, s_relay_widget[i], sizeof(s_relay_widget[i]));
    }
}

/* ==================== Helpers ==================== */

static bool push_ready(void)
{
    return hyperwisor_ws_is_connected() && s_target_id[0] != '\0';
}

static const char *hvac_mode_str(hvac_mode_t m)
{
    switch (m) {
    case HVAC_MODE_OFF:    return "OFF";
    case HVAC_MODE_AUTO:   return "AUTO";
    case HVAC_MODE_MANUAL: return "MANUAL";
    case HVAC_MODE_PURGE:  return "PURGE";
    default:               return "?";
    }
}

/* True if the coil number is part of the HVAC-reserved block. */
static bool is_hvac_owned_coil(uint16_t coil)
{
    return coil == HVAC_FAN_COIL_LOW
        || coil == HVAC_FAN_COIL_MED
        || coil == HVAC_FAN_COIL_HIGH
        || coil == HVAC_FAN_COIL_MAX
        || coil == HVAC_CLUTCH_COIL
        || coil == HVAC_IDLEUP_COIL
        || coil == HVAC_RECIRC_COIL
        || coil == HVAC_HEATER_COIL;
}

/* ==================== Status snapshot builder ==================== */

static void build_status_payload(cJSON *payload)
{
    /* relays: array of {relay, state} for coils 0..7 */
    cJSON *relays = cJSON_AddArrayToObject(payload, "relays");
    int n = 0;
    ctrl_toggle_t *r = ctrl_reading_lights(&n);
    if (n > MAX_USER_RELAYS) n = MAX_USER_RELAYS;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "relay", r[i].coil);
        cJSON_AddBoolToObject  (item, "state", r[i].on);
        cJSON_AddItemToArray(relays, item);
    }

    /* bmp280 */
    float t = 0.0f, p = 0.0f;
    bool ok = bmp280_get(&t, &p);
    cJSON *sensor = cJSON_AddObjectToObject(payload, "bmp280");
    cJSON_AddBoolToObject(sensor, "valid", ok);
    if (ok) {
        cJSON_AddNumberToObject(sensor, "temp_c", t);
        cJSON_AddNumberToObject(sensor, "hpa",    p);
    }

    /* hvac */
    hvac_status_t st = {0};
    hvac_controller_get_status(&st);
    ac_state_t *ac = ac_state();
    cJSON *hvac = cJSON_AddObjectToObject(payload, "hvac");
    cJSON_AddBoolToObject  (hvac, "power",     ac->power);
    cJSON_AddBoolToObject  (hvac, "auto",      ac->auto_mode);
    cJSON_AddNumberToObject(hvac, "fan_level", ac->fan_level);
    cJSON_AddNumberToObject(hvac, "fan_step",  st.fan_step);
    cJSON_AddBoolToObject  (hvac, "clutch",    st.clutch_on);
    cJSON_AddBoolToObject  (hvac, "cabin_valid", st.cabin_valid);
    if (st.cabin_valid) {
        cJSON_AddNumberToObject(hvac, "cabin_c", st.cabin_c);
        cJSON_AddNumberToObject(hvac, "delta_c", st.delta_c);
    }
    cJSON_AddNumberToObject(hvac, "target_c", st.target_c);
    cJSON_AddStringToObject(hvac, "mode",     hvac_mode_str(st.mode));
}

/* ==================== Custom command handlers ==================== */

/* relay_control: { command:"relay_control", actions:[{ action:"set",
 *                  params:{ coil:<int>, on:<bool> } }] }                */
static void handle_relay_control(const char *from, cJSON *payload)
{
    cJSON *actions = cJSON_GetObjectItem(payload, "actions");
    if (!cJSON_IsArray(actions)) return;

    int count = cJSON_GetArraySize(actions);
    for (int i = 0; i < count; i++) {
        cJSON *act    = cJSON_GetArrayItem(actions, i);
        cJSON *params = cJSON_GetObjectItem(act, "params");
        if (!cJSON_IsObject(params)) continue;

        /* Accept both legacy {coil,on} and new {relay,state} naming.
         * Also accept string forms ("1", "true", "on", "0", "false")
         * for dashboards that stringify all params. */
        cJSON *coil_j  = cJSON_GetObjectItem(params, "coil");
        cJSON *relay_j = cJSON_GetObjectItem(params, "relay");
        cJSON *id_j    = cJSON_IsNumber(relay_j) || cJSON_IsString(relay_j) ? relay_j : coil_j;
        int   coil     = -1;
        if (cJSON_IsNumber(id_j)) {
            coil = (int)id_j->valuedouble;
        } else if (cJSON_IsString(id_j) && id_j->valuestring) {
            coil = atoi(id_j->valuestring);
        } else {
            continue;
        }

        cJSON *on_j    = cJSON_GetObjectItem(params, "on");
        cJSON *state_j = cJSON_GetObjectItem(params, "state");
        cJSON *bool_j  = state_j ? state_j : on_j;
        bool on = false;
        if (cJSON_IsBool(bool_j)) {
            on = cJSON_IsTrue(bool_j);
        } else if (cJSON_IsNumber(bool_j)) {
            on = bool_j->valuedouble != 0.0;
        } else if (cJSON_IsString(bool_j) && bool_j->valuestring) {
            const char *s = bool_j->valuestring;
            if      (!strcasecmp(s, "true")  || !strcasecmp(s, "on")  || !strcmp(s, "1")) on = true;
            else if (!strcasecmp(s, "false") || !strcasecmp(s, "off") || !strcmp(s, "0")) on = false;
            else continue;
        } else {
            continue;
        }

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp, "relay", coil);
        cJSON_AddBoolToObject  (resp, "state", on);

        if (coil < 0 || coil >= MAX_USER_RELAYS || is_hvac_owned_coil((uint16_t)coil)) {
            ESP_LOGW(TAG, "relay_control: reject coil %d (HVAC-owned or out of range)", coil);
            cJSON_AddStringToObject(resp, "status", "rejected");
            cJSON_AddStringToObject(resp, "reason", "hvac_reserved_or_out_of_range");
        } else {
            ESP_LOGI(TAG, "relay_control: coil=%d on=%d (from %s)", coil, on, from);
            modbus_task_request((uint16_t)coil, on);
            cJSON_AddStringToObject(resp, "status", "ok");
        }
        hyperwisor_emit(from, "relay_control", "response", resp);
    }
}

/* hvac_control: { command:"hvac_control", actions:[
 *    { action:"power",     params:{ on:<bool> } },
 *    { action:"auto",      params:{ on:<bool> } },
 *    { action:"fan_level", params:{ level:<1..5> } },
 *    { action:"set_temp",  params:{ temp_c:<int> } } ] }                */
static void handle_hvac_control(const char *from, cJSON *payload)
{
    cJSON *actions = cJSON_GetObjectItem(payload, "actions");
    if (!cJSON_IsArray(actions)) return;

    ac_state_t *st = ac_state();
    cJSON *applied = cJSON_CreateArray();
    int count = cJSON_GetArraySize(actions);
    for (int i = 0; i < count; i++) {
        cJSON *act    = cJSON_GetArrayItem(actions, i);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(act, "action"));
        cJSON *params = cJSON_GetObjectItem(act, "params");
        if (!name || !cJSON_IsObject(params)) continue;

        if (strcmp(name, "power") == 0) {
            cJSON *on_j = cJSON_GetObjectItem(params, "on");
            if (cJSON_IsBool(on_j)) {
                st->power = cJSON_IsTrue(on_j);
                ESP_LOGI(TAG, "hvac_control: power=%d", st->power);
                cJSON_AddItemToArray(applied, cJSON_CreateString("power"));
            }
        } else if (strcmp(name, "auto") == 0) {
            cJSON *on_j = cJSON_GetObjectItem(params, "on");
            if (cJSON_IsBool(on_j)) {
                st->auto_mode = cJSON_IsTrue(on_j);
                ESP_LOGI(TAG, "hvac_control: auto=%d", st->auto_mode);
                cJSON_AddItemToArray(applied, cJSON_CreateString("auto"));
            }
        } else if (strcmp(name, "fan_level") == 0) {
            int lvl = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(params, "level"));
            if (lvl >= 1 && lvl <= 5) {
                st->fan_level = lvl;
                ESP_LOGI(TAG, "hvac_control: fan_level=%d", lvl);
                cJSON_AddItemToArray(applied, cJSON_CreateString("fan_level"));
            }
        } else if (strcmp(name, "set_temp") == 0) {
            int t = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(params, "temp_c"));
            if (t >= 16 && t <= 30) {
                st->temp_c = t;
                ESP_LOGI(TAG, "hvac_control: set_temp=%d", t);
                cJSON_AddItemToArray(applied, cJSON_CreateString("set_temp"));
            }
        } else {
            ESP_LOGW(TAG, "hvac_control: unknown action '%s'", name);
        }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddItemToObject  (resp, "applied", applied);
    hyperwisor_emit(from, "hvac_control", "response", resp);
}

/* get_status: respond with full live snapshot (relays + bmp280 + hvac) */
static void handle_get_status(const char *from, cJSON *payload)
{
    (void)payload;
    cJSON *p = cJSON_CreateObject();
    build_status_payload(p);
    hyperwisor_emit(from, "get_status", "response", p);
}

/* config: { command:"config", actions:[{ action:"bind", params:{
 *     targetid:"...",
 *     rw_0:"...", rw_1:"...", ...
 *     w_temp:"...", w_hpa:"...",
 *     w_hvac_mode:"...", w_hvac_fan:"...", w_hvac_clutch:"...",
 *     w_hvac_cur:"...", w_hvac_set:"..." } }] }                          */
static void save_field(cJSON *params, const char *key, char *dst, size_t dst_len)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(params, key));
    if (!v) return;
    strncpy(dst, v, dst_len - 1);
    dst[dst_len - 1] = '\0';
    nvs_save_str(key, v);
    ESP_LOGI(TAG, "config bind: %s = %s", key, v);
}

static void handle_config(const char *from, cJSON *payload)
{
    cJSON *actions = cJSON_GetObjectItem(payload, "actions");
    if (!cJSON_IsArray(actions)) return;

    int count = cJSON_GetArraySize(actions);
    for (int i = 0; i < count; i++) {
        cJSON *act    = cJSON_GetArrayItem(actions, i);
        cJSON *params = cJSON_GetObjectItem(act, "params");
        if (!cJSON_IsObject(params)) continue;

        save_field(params, HYPERWISOR_APP_KEY_TARGET,   s_target_id,    sizeof(s_target_id));
        save_field(params, HYPERWISOR_APP_KEY_W_TEMP,   s_w_temp,       sizeof(s_w_temp));
        save_field(params, HYPERWISOR_APP_KEY_W_HPA,    s_w_hpa,        sizeof(s_w_hpa));
        save_field(params, HYPERWISOR_APP_KEY_W_MODE,   s_w_hvac_mode,  sizeof(s_w_hvac_mode));
        save_field(params, HYPERWISOR_APP_KEY_W_FAN,    s_w_hvac_fan,   sizeof(s_w_hvac_fan));
        save_field(params, HYPERWISOR_APP_KEY_W_CLUTCH, s_w_hvac_clutch, sizeof(s_w_hvac_clutch));
        save_field(params, HYPERWISOR_APP_KEY_W_CUR,    s_w_hvac_cur,   sizeof(s_w_hvac_cur));
        save_field(params, HYPERWISOR_APP_KEY_W_SET,    s_w_hvac_set,   sizeof(s_w_hvac_set));

        for (int k = 0; k < MAX_USER_RELAYS; k++) {
            char key[16];
            snprintf(key, sizeof(key), HYPERWISOR_APP_KEY_RELAY_FMT, k);
            save_field(params, key, s_relay_widget[k], sizeof(s_relay_widget[k]));
        }
    }

    /* Acknowledge */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "targetid", s_target_id);
    hyperwisor_emit(from, "config", "response", resp);
}

/* ---- rgb_control helpers ---- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool parse_hex_colour(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!s) return false;
    if (*s == '#') s++;
    int n[6];
    for (int i = 0; i < 6; i++) {
        n[i] = hex_nibble(s[i]);
        if (n[i] < 0) return false;
    }
    if (s[6] != '\0') return false;
    *r = (uint8_t)((n[0] << 4) | n[1]);
    *g = (uint8_t)((n[2] << 4) | n[3]);
    *b = (uint8_t)((n[4] << 4) | n[5]);
    return true;
}

/* Extract an 8-bit RGB triple from a picker-widget params block.
 * The dashboard sends several redundant encodings at once (hex, nested
 * rgb{}, flat r/g/b, hsl, hsv, cmyk). We try them in the order that
 * gives the best fidelity. */
static inline uint8_t clamp_u8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static bool rgb_from_params(cJSON *params, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* 1) Nested rgb:{r,g,b} -- most explicit form from the widget. */
    cJSON *rgb_obj = cJSON_GetObjectItem(params, "rgb");
    if (cJSON_IsObject(rgb_obj)) {
        cJSON *rj = cJSON_GetObjectItem(rgb_obj, "r");
        cJSON *gj = cJSON_GetObjectItem(rgb_obj, "g");
        cJSON *bj = cJSON_GetObjectItem(rgb_obj, "b");
        if (cJSON_IsNumber(rj) && cJSON_IsNumber(gj) && cJSON_IsNumber(bj)) {
            *r = clamp_u8((int)rj->valuedouble);
            *g = clamp_u8((int)gj->valuedouble);
            *b = clamp_u8((int)bj->valuedouble);
            return true;
        }
    }
    /* 2) Flat r/g/b siblings of the params object. */
    cJSON *rj = cJSON_GetObjectItem(params, "r");
    cJSON *gj = cJSON_GetObjectItem(params, "g");
    cJSON *bj = cJSON_GetObjectItem(params, "b");
    if (cJSON_IsNumber(rj) && cJSON_IsNumber(gj) && cJSON_IsNumber(bj)) {
        *r = clamp_u8((int)rj->valuedouble);
        *g = clamp_u8((int)gj->valuedouble);
        *b = clamp_u8((int)bj->valuedouble);
        return true;
    }
    /* 3) Hex string ("#rrggbb"), either as "hex" or "color". */
    const char *hex = cJSON_GetStringValue(cJSON_GetObjectItem(params, "hex"));
    if (!hex) hex = cJSON_GetStringValue(cJSON_GetObjectItem(params, "color"));
    return parse_hex_colour(hex, r, g, b);
}

/* Pull HSV from picker-widget params. Prefers the nested hsv:{h,s,v} object
 * (what the dashboard natively emits), falls back to top-level h/s/v.
 * Returns false if none of them are present or numeric. */
static bool hsv_from_params(cJSON *params, uint16_t *h, uint8_t *s, uint8_t *v)
{
    cJSON *hsv_obj = cJSON_GetObjectItem(params, "hsv");
    cJSON *hj = NULL, *sj = NULL, *vj = NULL;
    if (cJSON_IsObject(hsv_obj)) {
        hj = cJSON_GetObjectItem(hsv_obj, "h");
        sj = cJSON_GetObjectItem(hsv_obj, "s");
        vj = cJSON_GetObjectItem(hsv_obj, "v");
    }
    if (!(cJSON_IsNumber(hj) && cJSON_IsNumber(sj) && cJSON_IsNumber(vj))) {
        hj = cJSON_GetObjectItem(params, "h");
        sj = cJSON_GetObjectItem(params, "s");
        vj = cJSON_GetObjectItem(params, "v");
    }
    if (!(cJSON_IsNumber(hj) && cJSON_IsNumber(sj) && cJSON_IsNumber(vj))) {
        return false;
    }
    int hi = (int)hj->valuedouble;
    int si = (int)sj->valuedouble;
    int vi = (int)vj->valuedouble;
    /* wrap hue into [0, 359]; clamp sat/val into [0, 100] */
    hi %= 360;
    if (hi < 0) hi += 360;
    if (si < 0) si = 0; else if (si > 100) si = 100;
    if (vi < 0) vi = 0; else if (vi > 100) vi = 100;
    *h = (uint16_t)hi;
    *s = (uint8_t)si;
    *v = (uint8_t)vi;
    return true;
}

/* RGB -> HSV fallback (0..255 -> h:0..359, s/v:0..100), used when the
 * dashboard omits the hsv block and we still want to sync the wheel. */
static void rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b,
                       uint16_t *h, uint8_t *s, uint8_t *v)
{
    uint8_t cmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint8_t cmin = r < g ? (r < b ? r : b) : (g < b ? g : b);
    uint8_t d = cmax - cmin;
    int hue = 0;
    if (d != 0) {
        if (cmax == r) {
            hue = ((int)g - (int)b) * 60 / d;
        } else if (cmax == g) {
            hue = ((int)b - (int)r) * 60 / d + 120;
        } else {
            hue = ((int)r - (int)g) * 60 / d + 240;
        }
        if (hue < 0) hue += 360;
    }
    *h = (uint16_t)hue;
    *s = (cmax == 0) ? 0 : (uint8_t)(((int)d * 100) / cmax);
    *v = (uint8_t)(((int)cmax * 100) / 255);
}

/* rgb_control: { command:"rgb_control", actions:[
 *    { action:"roof"|"floor",
 *      params:{ rgb:{r,g,b} | r,g,b | color|hex:"#rrggbb", ... } } ] }
 *
 * The picker widget also emits hsl/hsv/cmyk fields we ignore -- we want
 * the 8-bit RGB the slave's PWM channels drive directly. */
static void handle_rgb_control(const char *from, cJSON *payload)
{
    cJSON *actions = cJSON_GetObjectItem(payload, "actions");
    if (!cJSON_IsArray(actions)) return;

    cJSON *applied = cJSON_CreateArray();
    int count = cJSON_GetArraySize(actions);
    for (int i = 0; i < count; i++) {
        cJSON *act = cJSON_GetArrayItem(actions, i);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(act, "action"));
        cJSON *params = cJSON_GetObjectItem(act, "params");
        if (!name || !cJSON_IsObject(params)) continue;

        uint16_t start_reg;
        if (strcasecmp(name, "roof") == 0) {
            start_reg = CLOUD_RGB_START_REG_ROOF;
        } else if (strcasecmp(name, "floor") == 0) {
            start_reg = CLOUD_RGB_START_REG_FLOOR;
        } else {
            ESP_LOGW(TAG, "rgb_control: unknown zone '%s'", name);
            continue;
        }

        uint8_t r = 0, g = 0, b = 0;
        if (!rgb_from_params(params, &r, &g, &b)) {
            ESP_LOGW(TAG, "rgb_control[%s]: no usable rgb/hex in params", name);
            continue;
        }

        /* Picking a non-black colour implies ON; explicit OFF comes
         * through as r=g=b=0 or as a discrete relay_control action. */
        bool on = (r | g | b) != 0;

        ESP_LOGI(TAG, "rgb_control: zone=%s rgb=(%u,%u,%u) on=%d (from %s)",
                 name, r, g, b, on, from);
        modbus_task_request_rgb(CLOUD_RGB_SLAVE_ADDR, start_reg, r, g, b, on);

        /* Mirror the change into the on-device UI model so the RGB tab's
         * colorwheel + brightness slider come up with the correct values
         * next time the user opens that detail screen. We prefer the HSV
         * block the dashboard sends directly (exact match to ctrl_rgb_t's
         * fields); only fall back to RGB->HSV conversion if it's missing. */
        ctrl_rgb_t *zone_state = (start_reg == CLOUD_RGB_START_REG_ROOF)
                                     ? ctrl_rgb_roof()
                                     : ctrl_rgb_floor();
        if (zone_state) {
            uint16_t h = 0; uint8_t s = 0, v = 0;
            if (!hsv_from_params(params, &h, &s, &v)) {
                rgb_to_hsv(r, g, b, &h, &s, &v);
            }
            /* Keep the previous hue/sat when the user picks pure black --
             * otherwise the wheel would snap to red on next open. */
            if (on) {
                zone_state->hue = h;
                zone_state->sat = s;
            }
            zone_state->brightness = v;
            zone_state->on         = on;
        }

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "zone", name);
        cJSON_AddNumberToObject(item, "r", r);
        cJSON_AddNumberToObject(item, "g", g);
        cJSON_AddNumberToObject(item, "b", b);
        cJSON_AddBoolToObject  (item, "on", on);
        cJSON_AddItemToArray(applied, item);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddItemToObject  (resp, "applied", applied);
    hyperwisor_emit(from, "rgb_control", "response", resp);
}

/* ==================== Public API ==================== */

esp_err_t hyperwisor_app_init(void)
{
    load_cached_bindings();

    hyperwisor_register_cmd_handler("relay_control", handle_relay_control);
    hyperwisor_register_cmd_handler("hvac_control",  handle_hvac_control);
    hyperwisor_register_cmd_handler("get_status",    handle_get_status);
    hyperwisor_register_cmd_handler("config",        handle_config);

    /* Also expose built-in SYSTEM handler (restart / uptime). */
    hyperwisor_register_cmd_handler("SYSTEM",        hyperwisor_cmd_handle_system);

    /* DEVICE_STATUS: server/app pings us to check liveness; reply "online".
     * Matches the Arduino hyperwisor-iot library behaviour exactly. */
    hyperwisor_register_cmd_handler("DEVICE_STATUS", hyperwisor_cmd_handle_device_status);

    /* rgb_control: dashboard colour-picker widget; forwards to the
     * Arduino RGB slave at 0x20 over RS-485. action="roof"|"floor". */
    hyperwisor_register_cmd_handler("rgb_control",   handle_rgb_control);

    ESP_LOGI(TAG, "app init: target='%s' temp_w='%s' hpa_w='%s'",
             s_target_id, s_w_temp, s_w_hpa);
    return ESP_OK;
}

bool hyperwisor_app_is_configured(void)
{
    return s_target_id[0] != '\0';
}

const char *hyperwisor_app_get_target_id(void)
{
    return s_target_id;
}

/* ==================== Proactive pushers ==================== */

void hyperwisor_app_push_relay_state(int coil_index, bool on)
{
    if (!push_ready()) return;
    if (coil_index < 0 || coil_index >= MAX_USER_RELAYS) return;

    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "relay", coil_index);
    cJSON_AddBoolToObject  (p, "state", on);
    hyperwisor_emit(s_target_id, "relay_control", "update", p);
}

void hyperwisor_app_push_bmp280(float temp_c, float pressure_hpa)
{
    if (!push_ready()) return;

    cJSON *p = cJSON_CreateObject();
    cJSON_AddBoolToObject  (p, "valid",  true);
    cJSON_AddNumberToObject(p, "temp_c", temp_c);
    cJSON_AddNumberToObject(p, "hpa",    pressure_hpa);
    hyperwisor_emit(s_target_id, "environment", "update", p);
}

void hyperwisor_app_push_hvac_status(void)
{
    if (!push_ready()) return;

    hvac_status_t st = {0};
    hvac_controller_get_status(&st);
    ac_state_t *ac = ac_state();

    cJSON *p = cJSON_CreateObject();
    cJSON_AddBoolToObject  (p, "power",       ac->power);
    cJSON_AddBoolToObject  (p, "auto",        ac->auto_mode);
    cJSON_AddNumberToObject(p, "fan_level",   ac->fan_level);
    cJSON_AddNumberToObject(p, "fan_step",    st.fan_step);
    cJSON_AddBoolToObject  (p, "clutch",      st.clutch_on);
    cJSON_AddBoolToObject  (p, "cabin_valid", st.cabin_valid);
    if (st.cabin_valid) {
        cJSON_AddNumberToObject(p, "cabin_c", st.cabin_c);
        cJSON_AddNumberToObject(p, "delta_c", st.delta_c);
    }
    cJSON_AddNumberToObject(p, "target_c", st.target_c);
    cJSON_AddStringToObject(p, "mode",     hvac_mode_str(st.mode));
    hyperwisor_emit(s_target_id, "hvac_control", "update", p);
}
