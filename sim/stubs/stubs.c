/* =====================================================================
 *  stubs.c — desktop stand-ins for everything the UI layer calls that
 *  only exists on the ESP32.
 *
 *  Design rule: stub the PLATFORM, not the application. NVS is a real
 *  (in-memory) key/value store, so owner_name.c, tile_names.c,
 *  ui_theme.c, hmi_role.c, bus_config.c and display_brightness.c all
 *  compile and run unmodified here -- their save/load logic is exercised
 *  for real. Only the things with no desktop meaning (Modbus, ESP-NOW,
 *  the cloud link, the BMP280) return canned values.
 *
 *  Canned values are chosen to make the UI interesting to look at:
 *  the bus reads "online", the cloud reads "connected", the sensor
 *  returns a plausible cabin temperature.
 * ===================================================================== */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp.h"
#include "hyperwisor.h"
#include "hyperwisor_app.h"

#include "modbus_client.h"
#include "modbus_task.h"
#include "hmi_sync.h"
#include "hmi_link.h"
#include "hvac_controller.h"
#include "bmp280.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static const char *TAG = "sim";

/* ---- esp_err ---------------------------------------------------------- */

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK:                    return "ESP_OK";
        case ESP_FAIL:                  return "ESP_FAIL";
        case ESP_ERR_NO_MEM:            return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG:       return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE:     return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND:         return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_TIMEOUT:           return "ESP_ERR_TIMEOUT";
        case ESP_ERR_INVALID_RESPONSE:  return "ESP_ERR_INVALID_RESPONSE";
        case ESP_ERR_INVALID_CRC:       return "ESP_ERR_INVALID_CRC";
        case ESP_ERR_NVS_NOT_FOUND:     return "ESP_ERR_NVS_NOT_FOUND";
        default:                        return "ESP_ERR_?";
    }
}

/* ---- system / heap / mac / timer -------------------------------------- */

/* The sim can't reboot itself; UI screens that "apply and restart"
 * (HMI Role, Bus Setup) land here. Say so loudly rather than dying, so
 * you can keep clicking around afterwards. */
void esp_restart(void)
{
    ESP_LOGW(TAG, "esp_restart() -- device would reboot here (sim keeps running)");
}

uint32_t esp_get_free_heap_size(void)          { return 4u * 1024 * 1024; }
uint32_t esp_get_minimum_free_heap_size(void)  { return 3u * 1024 * 1024; }
size_t   heap_caps_get_free_size(uint32_t c)          { (void)c; return 4u*1024*1024; }
size_t   heap_caps_get_largest_free_block(uint32_t c) { (void)c; return 2u*1024*1024; }

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    (void)type;
    static const uint8_t fake[6] = { 0xDC, 0xB4, 0xD9, 0x51, 0x4D, 0x00 };
    if (mac) memcpy(mac, fake, 6);
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)lv_tick_get() * 1000;   /* us, from the LVGL tick */
}

/* ---- FreeRTOS --------------------------------------------------------- */

/* Advance LVGL's tick while "delaying" so animations and timers still
 * progress; the sim's render loop is the same thread. */
void vTaskDelay(TickType_t ticks)
{
    lv_tick_inc((uint32_t)ticks);
}

TickType_t xTaskGetTickCount(void) { return (TickType_t)lv_tick_get(); }
void       vTaskDelete(TaskHandle_t t) { (void)t; }

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *arg, uint32_t prio,
                                   TaskHandle_t *handle, int core)
{
    (void)fn; (void)stack; (void)arg; (void)prio; (void)core;
    ESP_LOGI(TAG, "xTaskCreate(%s) ignored in sim", name ? name : "?");
    if (handle) *handle = NULL;
    return pdPASS;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, uint32_t prio, TaskHandle_t *handle)
{
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, handle, 0);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)          { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)         { (void)s; return pdTRUE; }
QueueHandle_t xQueueCreate(uint32_t l, uint32_t i)     { (void)l; (void)i; return (void *)1; }
BaseType_t xQueueSend(QueueHandle_t q, const void *it, TickType_t t)
{ (void)q; (void)it; (void)t; return pdTRUE; }
BaseType_t xQueueReceive(QueueHandle_t q, void *it, TickType_t t)
{ (void)q; (void)it; (void)t; return pdFALSE; }

/* ---- NVS: a real (in-memory) key/value store -------------------------- */

#define NVS_MAX_ENTRIES 128
#define NVS_MAX_VAL     256

typedef struct {
    bool     used;
    char     ns[16];
    char     key[16];
    uint8_t  val[NVS_MAX_VAL];
    size_t   len;
} nvs_entry_t;

static nvs_entry_t s_nvs[NVS_MAX_ENTRIES];
static char        s_ns_by_handle[16][16];
static uint32_t    s_next_handle = 1;

esp_err_t nvs_flash_init(void)  { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { memset(s_nvs, 0, sizeof(s_nvs)); return ESP_OK; }

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    if (!ns || !out) return ESP_ERR_INVALID_ARG;
    if (s_next_handle >= 16) s_next_handle = 1;   /* wrap; sim only */
    uint32_t h = s_next_handle++;
    snprintf(s_ns_by_handle[h], sizeof(s_ns_by_handle[h]), "%s", ns);
    *out = h;
    return ESP_OK;
}

void      nvs_close(nvs_handle_t h)  { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

static nvs_entry_t *nvs_find(nvs_handle_t h, const char *key, bool create)
{
    if (h == 0 || h >= 16) return NULL;
    const char *ns = s_ns_by_handle[h];
    for (int i = 0; i < NVS_MAX_ENTRIES; i++) {
        if (s_nvs[i].used && !strcmp(s_nvs[i].ns, ns) && !strcmp(s_nvs[i].key, key))
            return &s_nvs[i];
    }
    if (!create) return NULL;
    for (int i = 0; i < NVS_MAX_ENTRIES; i++) {
        if (!s_nvs[i].used) {
            s_nvs[i].used = true;
            snprintf(s_nvs[i].ns,  sizeof(s_nvs[i].ns),  "%s", ns);
            snprintf(s_nvs[i].key, sizeof(s_nvs[i].key), "%s", key);
            return &s_nvs[i];
        }
    }
    return NULL;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    if (h == 0 || h >= 16) return ESP_ERR_INVALID_ARG;
    const char *ns = s_ns_by_handle[h];
    for (int i = 0; i < NVS_MAX_ENTRIES; i++)
        if (s_nvs[i].used && !strcmp(s_nvs[i].ns, ns)) s_nvs[i].used = false;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    nvs_entry_t *e = nvs_find(h, key, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    e->used = false;
    return ESP_OK;
}

static esp_err_t nvs_put(nvs_handle_t h, const char *k, const void *v, size_t n)
{
    if (n > NVS_MAX_VAL) return ESP_ERR_INVALID_SIZE;
    nvs_entry_t *e = nvs_find(h, k, true);
    if (!e) return ESP_ERR_NO_MEM;
    memcpy(e->val, v, n);
    e->len = n;
    return ESP_OK;
}

static esp_err_t nvs_take(nvs_handle_t h, const char *k, void *out, size_t n)
{
    nvs_entry_t *e = nvs_find(h, k, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->len != n) return ESP_ERR_INVALID_SIZE;
    memcpy(out, e->val, n);
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{ return nvs_put(h, k, v, strlen(v) + 1); }

esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len)
{
    nvs_entry_t *e = nvs_find(h, k, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { if (len) *len = e->len; return ESP_OK; }
    if (!len || *len < e->len) return ESP_ERR_INVALID_SIZE;
    memcpy(out, e->val, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_u8 (nvs_handle_t h, const char *k, uint8_t  v) { return nvs_put(h,k,&v,1); }
esp_err_t nvs_get_u8 (nvs_handle_t h, const char *k, uint8_t  *o){ return nvs_take(h,k,o,1); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { return nvs_put(h,k,&v,2); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *o){ return nvs_take(h,k,o,2); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return nvs_put(h,k,&v,4); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *o){ return nvs_take(h,k,o,4); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t  v) { return nvs_put(h,k,&v,4); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t  *o){ return nvs_take(h,k,o,4); }

esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n)
{ return nvs_put(h, k, v, n); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *n)
{
    nvs_entry_t *e = nvs_find(h, k, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!o) { if (n) *n = e->len; return ESP_OK; }
    if (!n || *n < e->len) return ESP_ERR_INVALID_SIZE;
    memcpy(o, e->val, e->len);
    *n = e->len;
    return ESP_OK;
}

/* ---- BSP -------------------------------------------------------------- */

esp_err_t bsp_init(void) { return ESP_OK; }
void bsp_display_backlight(bool on) { ESP_LOGI(TAG, "backlight %s", on ? "ON" : "OFF"); }
esp_err_t bsp_io_expander_set(uint8_t bit, uint8_t lvl) { (void)bit; (void)lvl; return ESP_OK; }

/* ---- Modbus: no bus on a laptop --------------------------------------- */

esp_err_t modbus_client_init(uint32_t baud) { (void)baud; return ESP_OK; }
esp_err_t modbus_client_write_coil(uint8_t s, uint16_t c, bool on)
{ ESP_LOGI(TAG, "modbus write_coil slave=0x%02x coil=%u on=%d", s, c, on); return ESP_OK; }
esp_err_t modbus_client_read_coils(uint8_t s, uint16_t st, uint16_t n, uint16_t *mask)
{ (void)s; (void)st; (void)n; if (mask) *mask = 0; return ESP_OK; }
esp_err_t modbus_client_write_hregs(uint8_t s, uint16_t st, uint16_t n, const uint16_t *r)
{
    ESP_LOGI(TAG, "modbus write_hregs slave=0x%02x start=%u count=%u v0=%u v1=%u",
             s, st, n, n > 0 ? r[0] : 0, n > 1 ? r[1] : 0);
    return ESP_OK;
}
esp_err_t modbus_client_write_hreg_single(uint8_t s, uint16_t a, uint16_t v)
{ ESP_LOGI(TAG, "modbus write_hreg slave=0x%02x addr=%u val=%u", s, a, v); return ESP_OK; }

/* Canned LED counts so the LED Strips screen shows plausible values. */
esp_err_t modbus_client_read_hregs(uint8_t s, uint16_t st, uint16_t n, uint16_t *regs)
{
    (void)s;
    if (!regs) return ESP_ERR_INVALID_ARG;
    for (uint16_t i = 0; i < n; i++) regs[i] = 0;
    if (st == 12 && n >= 2) { regs[0] = 60; regs[1] = 45; }
    return ESP_OK;
}

static uint32_t s_seq;
bool     modbus_task_link_online(void) { return true; }
uint32_t modbus_task_state_seq(void)   { return s_seq; }
esp_err_t modbus_task_start(void)      { return ESP_OK; }
esp_err_t modbus_task_pause(uint32_t t){ (void)t; return ESP_OK; }
void      modbus_task_resume(void)     { }
void modbus_task_request(uint16_t coil, bool on)
{ ESP_LOGI(TAG, "coil %u -> %d", coil, on); s_seq++; }
void modbus_task_request_rgb(uint8_t slave, uint16_t start, uint8_t r, uint8_t g,
                             uint8_t b, bool on)
{
    ESP_LOGI(TAG, "RGB slave=0x%02x reg=%u R%u G%u B%u %s",
             slave, start, r, g, b, on ? "ON" : "OFF");
    s_seq++;
}

/* ---- dual-HMI sync / wireless link ------------------------------------ */

void hmi_sync_push_intent(hmi_cmd_t c, uint16_t a0, uint16_t a1, uint16_t a2)
{ ESP_LOGI(TAG, "intent cmd=%d a0=%u a1=%u a2=%u", (int)c, a0, a1, a2); }

/* Pretend a wireless secondary is paired with a healthy signal so the
 * HMI Role screen renders its populated state rather than "--". */
bool hmi_link_paired(void)         { return true; }
bool hmi_link_pairing_active(void) { return false; }
void hmi_link_pair_begin(uint32_t s) { ESP_LOGW(TAG, "pairing window %us", s); }
void hmi_link_forget(void)         { ESP_LOGW(TAG, "peer forgotten"); }
void hmi_link_peer_str(char *buf, size_t len) { snprintf(buf, len, "dc:b4:d9:26:fe:70"); }
bool hmi_link_peer_rssi(int8_t *out) { if (out) *out = -58; return true; }

/* ---- sensors / HVAC / cloud ------------------------------------------- */

bool bmp280_get(float *temp_c, float *press_hpa)
{
    if (temp_c)    *temp_c    = 24.5f;
    if (press_hpa) *press_hpa = 1013.2f;
    return true;
}

/* The Climate tab polls this every tick; hand back a plausible
 * "cooling on MED, everything settled" snapshot. */
void hvac_controller_get_status(hvac_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->wait         = HVAC_WAIT_NONE;
    out->fan_step     = 2;      /* MED */
    out->fan_coil     = 9;
    out->clutch_on    = true;
    out->clutch_coil  = 12;
    out->cabin_valid  = true;
    out->cabin_c      = 24.5f;
    out->target_c     = 22.0f;
    out->delta_c      = 2.5f;
    out->wait_ms_left = 0;
}

/* The sim runs LVGL on the single SDL thread, so there is nothing to
 * lock against -- but display_brightness.c calls these on every apply. */
bool lvgl_port_lock(int timeout_ms) { (void)timeout_ms; return true; }
void lvgl_port_unlock(void) { }

static hyperwisor_state_t s_hw = {
    .device_id = "b61ea704-92f7-45fb-bd0c-2a52f3124c6f",
    .ssid = "NIKOLA-VAN", .user_id = "sim-user", .version = "1.0.0-sim",
    .wifi_connected = true, .ws_connected = true, .uptime_seconds = 1234,
};

hyperwisor_state_t *hyperwisor_get_state(void) { return &s_hw; }
esp_err_t hyperwisor_set_device_id(const char *id)
{ snprintf(s_hw.device_id, sizeof(s_hw.device_id), "%s", id ? id : ""); return ESP_OK; }
esp_err_t hyperwisor_set_user_id(const char *id)
{ snprintf(s_hw.user_id, sizeof(s_hw.user_id), "%s", id ? id : ""); return ESP_OK; }
esp_err_t hyperwisor_clear_credentials(void)
{ ESP_LOGW(TAG, "credentials cleared"); return ESP_OK; }

const char *hyperwisor_app_get_target_id(void) { return s_hw.device_id; }
esp_err_t   hyperwisor_app_init(void) { return ESP_OK; }
void hyperwisor_app_push_relay_state(int coil, bool on) { (void)coil; (void)on; }
