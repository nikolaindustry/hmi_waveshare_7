/**
 * @file hyperwisor_core.c
 * @brief Hyperwisor IoT core orchestrator
 */

#include "hyperwisor_core.h"
#include "hyperwisor_nvs.h"
#include "hyperwisor_wifi.h"
#include "hyperwisor_ws.h"
#include "hyperwisor_cmd.h"
#include "hyperwisor_port.h"
#include "hyperwisor_hsc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

#if CONFIG_HYPERWISOR_ENABLE_OTA
#include "hyperwisor_ota.h"
#endif

#if CONFIG_HYPERWISOR_ENABLE_GPIO
#include "hyperwisor_gpio.h"
#endif

static const char *TAG = "HYPER_CORE";

static hyperwisor_state_t s_state = {0};
static TaskHandle_t s_hyper_task = NULL;
static hyperwisor_user_msg_cb_t s_user_msg_cb = NULL;
static uint32_t s_ap_start_time = 0;
static bool s_ap_started = false;
static SemaphoreHandle_t s_state_mutex = NULL;  /* Protects s_state */
#if CONFIG_HYPERWISOR_ENABLE_OTA
static bool s_ota_report_pending = false;       /* Send OTA success on WS connect */
#endif

/* Forward declarations */
static void on_wifi_event(hyperwisor_wifi_event_t evt, void *data);
static void on_ws_message(const char *data, int len);
static void on_ws_conn(bool connected);

esp_err_t hyperwisor_init(void)
{
    ESP_LOGI(TAG, "Hyperwisor IoT v%s initializing...", HYPERWISOR_VERSION);

    /* Create state mutex */
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize NVS first */
    ESP_ERROR_CHECK(hyperwisor_nvs_init());

    /* Load persisted config */
    hyperwisor_nvs_get_str("ssid", s_state.ssid, sizeof(s_state.ssid));
    hyperwisor_nvs_get_str("pass", s_state.password, sizeof(s_state.password));
    hyperwisor_nvs_get_str("deviceid", s_state.device_id, sizeof(s_state.device_id));
    hyperwisor_nvs_get_str("userid", s_state.user_id, sizeof(s_state.user_id));
    hyperwisor_nvs_get_str("email", s_state.email, sizeof(s_state.email));
    hyperwisor_nvs_get_str("productid", s_state.product_id, sizeof(s_state.product_id));
    hyperwisor_nvs_get_str("apikey", s_state.api_key, sizeof(s_state.api_key));
    hyperwisor_nvs_get_str("secretkey", s_state.secret_key, sizeof(s_state.secret_key));
    hyperwisor_nvs_get_str("firmware", s_state.version, sizeof(s_state.version));

    if (strlen(s_state.version) == 0) {
        strncpy(s_state.version, HYPERWISOR_VERSION, sizeof(s_state.version) - 1);
    }

    /* WebSocket host/port are hardcoded to match the Arduino reference library
     * (nikolaindustry-realtime.cpp): wss://nikolaindustry-realtime.onrender.com:443/?id=<deviceId>. */

    ESP_LOGI(TAG, "Device ID: %s", s_state.device_id);
    ESP_LOGI(TAG, "Product ID: %s", s_state.product_id);

    /* Init subsystems */
    esp_err_t err;

    err = hyperwisor_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return err;
    }

    err = hyperwisor_ws_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket init failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return err;
    }

    /* Register callbacks */
    hyperwisor_wifi_register_cb(on_wifi_event);
    hyperwisor_ws_register_msg_cb(on_ws_message);
    hyperwisor_ws_register_conn_cb(on_ws_conn);

#if CONFIG_HYPERWISOR_ENABLE_OTA
    /* Auto-register the OTA_UPDATE command handler.
     * After an OTA reboot, also confirm the new image is good
     * so the bootloader doesn't roll back. */
    hyperwisor_ota_auto_register();
    s_ota_report_pending = hyperwisor_ota_confirm_good_boot();
    ESP_LOGI(TAG, "OTA report pending: %s", s_ota_report_pending ? "YES" : "no");
#endif

#if CONFIG_HYPERWISOR_ENABLE_GPIO
    /* Auto-register the GPIO_MANAGEMENT command handler.
     * Only pins in the board-supplied allowlist are accessible.
     * If no allowlist is set, all cloud GPIO access is denied. */
    hyperwisor_gpio_auto_register();
    {
        const hyperwisor_port_t *port = hyperwisor_get_port();
        if (port && port->managed_gpios) {
            hyperwisor_gpio_set_allowlist(
                port->managed_gpios,
                port->managed_gpio_count);
        }
    }
#endif

#if CONFIG_HYPERWISOR_ENABLE_SYSTEM
    /* Auto-register the SYSTEM command handler (restart, status, info). */
    hyperwisor_register_cmd_handler("SYSTEM", hyperwisor_cmd_handle_system);
#endif

    ESP_LOGI(TAG, "Hyperwisor core initialized");
    return ESP_OK;
}

esp_err_t hyperwisor_start(void)
{
    ESP_LOGI(TAG, "Starting Hyperwisor services...");

    /* Start WiFi (STA first, fallback to AP) */
    esp_err_t err = hyperwisor_wifi_connect_sta();
    if (err != ESP_OK || strlen(s_state.ssid) == 0) {
        ESP_LOGW(TAG, "No stored WiFi credentials, starting AP mode");
        char ap_ssid[64];
        /* Append at most 6 chars from the tail of device_id. The %.6s
         * precision specifier bounds the directive so -Wformat-truncation
         * is satisfied even when device_id is up to 63 chars. */
        size_t did_len = strlen(s_state.device_id);
        const char *suffix = did_len > 6 ? s_state.device_id + did_len - 6
                                          : s_state.device_id;
        snprintf(ap_ssid, sizeof(ap_ssid), "%s%.6s",
                 HYPERWISOR_AP_SSID_PREFIX, suffix);
        hyperwisor_wifi_start_ap(ap_ssid, HYPERWISOR_AP_PASS);
        s_state.ap_mode_active = true;
    }

    /* Create background task */
    xTaskCreate(hyperwisor_task, "hyper_task", 8192, NULL, 5, &s_hyper_task);

    return ESP_OK;
}

hyperwisor_state_t *hyperwisor_get_state(void)
{
    return &s_state;
}

void hyperwisor_state_lock(void)
{
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    }
}

void hyperwisor_state_unlock(void)
{
    if (s_state_mutex) {
        xSemaphoreGive(s_state_mutex);
    }
}

esp_err_t hyperwisor_register_cmd_handler(const char *command, hyperwisor_cmd_handler_t handler)
{
    return hyperwisor_cmd_register(command, handler);
}

void hyperwisor_register_user_msg_cb(hyperwisor_user_msg_cb_t cb)
{
    s_user_msg_cb = cb;
}

esp_err_t hyperwisor_send_response(const char *to, cJSON *payload)
{
    cJSON *root = hyperwisor_cmd_build_response(to, payload);
    if (!root) {
        return ESP_FAIL;
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        return ESP_FAIL;
    }

    esp_err_t err = hyperwisor_ws_send(json_str, 0);
    free(json_str);
    return err;
}

esp_err_t hyperwisor_send_to(const char *target_id, void (*payload_builder)(cJSON *payload))
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "targetId", target_id);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    payload_builder(payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        return ESP_FAIL;
    }

    esp_err_t err = hyperwisor_ws_send(json_str, 0);
    free(json_str);
    return err;
}

esp_err_t hyperwisor_set_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid) strncpy(s_state.ssid, ssid, sizeof(s_state.ssid) - 1);
    if (password) strncpy(s_state.password, password, sizeof(s_state.password) - 1);
    hyperwisor_nvs_set_str("ssid", s_state.ssid);
    hyperwisor_nvs_set_str("pass", s_state.password);
    ESP_LOGI(TAG, "WiFi credentials saved");
    return ESP_OK;
}

esp_err_t hyperwisor_set_device_id(const char *device_id)
{
    if (!device_id || device_id[0] == '\0') {
        ESP_LOGW(TAG, "set_device_id: refusing to overwrite saved id with empty");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_state.device_id, device_id, sizeof(s_state.device_id) - 1);
    s_state.device_id[sizeof(s_state.device_id) - 1] = '\0';
    hyperwisor_nvs_set_str("deviceid", s_state.device_id);
    ESP_LOGI(TAG, "Device ID saved: %s", s_state.device_id);
    return ESP_OK;
}

esp_err_t hyperwisor_set_user_id(const char *user_id)
{
    if (!user_id || user_id[0] == '\0') {
        ESP_LOGW(TAG, "set_user_id: refusing to overwrite saved id with empty");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_state.user_id, user_id, sizeof(s_state.user_id) - 1);
    s_state.user_id[sizeof(s_state.user_id) - 1] = '\0';
    hyperwisor_nvs_set_str("userid", s_state.user_id);
    ESP_LOGI(TAG, "User ID saved: %s", s_state.user_id);
    return ESP_OK;
}

esp_err_t hyperwisor_set_api_keys(const char *api_key, const char *secret_key)
{
    if (api_key) strncpy(s_state.api_key, api_key, sizeof(s_state.api_key) - 1);
    if (secret_key) strncpy(s_state.secret_key, secret_key, sizeof(s_state.secret_key) - 1);
    hyperwisor_nvs_set_str("apikey", s_state.api_key);
    hyperwisor_nvs_set_str("secretkey", s_state.secret_key);
    ESP_LOGI(TAG, "API keys saved");
    return ESP_OK;
}

esp_err_t hyperwisor_set_credentials(const char *ssid, const char *password,
                                      const char *device_id, const char *user_id)
{
    hyperwisor_set_wifi_credentials(ssid, password);
    hyperwisor_set_device_id(device_id);
    if (user_id && strlen(user_id) > 0) {
        hyperwisor_set_user_id(user_id);
    }
    ESP_LOGI(TAG, "All credentials saved");
    return ESP_OK;
}

esp_err_t hyperwisor_clear_credentials(void)
{
    memset(s_state.ssid, 0, sizeof(s_state.ssid));
    memset(s_state.password, 0, sizeof(s_state.password));
    memset(s_state.device_id, 0, sizeof(s_state.device_id));
    memset(s_state.user_id, 0, sizeof(s_state.user_id));
    memset(s_state.email, 0, sizeof(s_state.email));
    memset(s_state.product_id, 0, sizeof(s_state.product_id));
    hyperwisor_nvs_set_str("ssid", "");
    hyperwisor_nvs_set_str("pass", "");
    hyperwisor_nvs_set_str("deviceid", "");
    hyperwisor_nvs_set_str("userid", "");
    hyperwisor_nvs_set_str("email", "");
    hyperwisor_nvs_set_str("productid", "");
    ESP_LOGI(TAG, "All credentials cleared");
    return ESP_OK;
}

bool hyperwisor_has_credentials(void)
{
    return strlen(s_state.ssid) > 0 && strlen(s_state.password) > 0 && strlen(s_state.device_id) > 0;
}

esp_err_t hyperwisor_get_device_id(char *out_buf, size_t buf_len)
{
    if (!out_buf || buf_len == 0) return ESP_ERR_INVALID_ARG;
    strncpy(out_buf, s_state.device_id, buf_len - 1);
    out_buf[buf_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t hyperwisor_get_user_id(char *out_buf, size_t buf_len)
{
    if (!out_buf || buf_len == 0) return ESP_ERR_INVALID_ARG;
    strncpy(out_buf, s_state.user_id, buf_len - 1);
    out_buf[buf_len - 1] = '\0';
    return ESP_OK;
}

/* --- HSC v1 security (opt-in) --- */

/* Bridge the transport's signer callback to the HSC signer, using our device id. */
static esp_err_t hsc_signer_bridge(const char *nonce, const char *ts,
                                   char *sig_out, size_t sig_len)
{
    return hyperwisor_hsc_sign(s_state.device_id, nonce, ts, sig_out, sig_len);
}

esp_err_t hyperwisor_enable_security(void)
{
    esp_err_t err = hyperwisor_hsc_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable_security: HSC init failed — security NOT enabled");
        return err;
    }
    hyperwisor_ws_enable_hsc(hsc_signer_bridge, s_state.device_id);
    ESP_LOGI(TAG, "Security enabled — device will authenticate to the secured relay");
    return ESP_OK;
}

esp_err_t hyperwisor_get_public_key_b64(char *out_buf, size_t buf_len)
{
    return hyperwisor_hsc_get_public_key_b64(out_buf, buf_len);
}

void hyperwisor_task(void *pvParam)
{
    ESP_LOGI(TAG, "Hyperwisor task running");

    while (1) {
        s_state.uptime_seconds++;

        /* Maintain WebSocket connection.
         *
         * We gate on ws_is_STARTED, not ws_is_CONNECTED: the WS library
         * has its own internal 5 s reconnect timer, so once we create
         * the client it will keep retrying on its own. If we instead
         * destroyed + recreated on every disconnect (as the old check
         * `!is_connected` did), we'd race the WS lib's internal event
         * loop and crash with a spinlock assert. */
        if (s_state.wifi_connected && !hyperwisor_ws_is_started() &&
            strlen(s_state.device_id) > 0
#if CONFIG_HYPERWISOR_ENABLE_OTA
            && !hyperwisor_ota_is_in_progress()
#endif
        ) {
            ESP_LOGI(TAG, "Starting WebSocket client for device %s", s_state.device_id);
            hyperwisor_ws_connect_with_device_id(
                CONFIG_HYPERWISOR_WS_HOST, CONFIG_HYPERWISOR_WS_PORT, s_state.device_id);
        }

        /* AP mode timeout: reboot after 4 minutes if still in AP */
        if (s_state.ap_mode_active && s_ap_started) {
            if (s_state.uptime_seconds - s_ap_start_time > 240) {
                ESP_LOGW(TAG, "Stuck in AP mode too long. Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }

        /* Send OTA success notification from the main task (not from the
         * WS event callback) to avoid ws_send deadlock. */
#if CONFIG_HYPERWISOR_ENABLE_OTA
        if (s_ota_report_pending && s_state.ws_connected) {
            s_ota_report_pending = false;
            hyperwisor_nvs_set_str("ota_pending_report", "0");

            char version[32] = {0};
            hyperwisor_ota_get_version(version, sizeof(version));

            cJSON *fp = cJSON_CreateObject();
            cJSON_AddStringToObject(fp, "status", "OTA_Update_Success");
            cJSON_AddStringToObject(fp, "value",  version);
            char *json = cJSON_PrintUnformatted(fp);
            cJSON_Delete(fp);
            if (json) {
                hyperwisor_ws_send(json, 0);
                free(json);
            }
            ESP_LOGI(TAG, "OTA success reported to cloud (v%s)", version);
        }

        /* Report OTA failure (download failed before reboot) */
        {
            char reason[64] = {0};
            hyperwisor_nvs_get_str("ota_failed_reason", reason, sizeof(reason));
            if (reason[0] != '\0' && s_state.ws_connected) {
                hyperwisor_nvs_set_str("ota_failed_reason", "");
                cJSON *fp = cJSON_CreateObject();
                cJSON_AddStringToObject(fp, "status", "OTA_Update_Failed");
                cJSON_AddStringToObject(fp, "value",  reason);
                char *json = cJSON_PrintUnformatted(fp);
                cJSON_Delete(fp);
                if (json) {
                    hyperwisor_ws_send(json, 0);
                    free(json);
                }
                ESP_LOGW(TAG, "OTA failure reported to cloud: %s", reason);
            }
        }
#endif

        /* Send periodic heartbeat/status */
        if (s_state.ws_connected && (s_state.uptime_seconds % 30 == 0)) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "uptime", s_state.uptime_seconds);
            cJSON_AddBoolToObject  (p, "wifi",   s_state.wifi_connected);
            hyperwisor_emit("server", "heartbeat", "update", p);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ==================== Callbacks ==================== */

static void on_wifi_event(hyperwisor_wifi_event_t evt, void *data)
{
    switch (evt) {
    case HYPERWISOR_WIFI_EVT_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        s_state.wifi_connected = true;
        s_state.ap_mode_active = false;
        break;

    case HYPERWISOR_WIFI_EVT_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi disconnected");
        s_state.wifi_connected = false;
        s_state.ws_connected = false;
        hyperwisor_ws_disconnect();
        break;

    case HYPERWISOR_WIFI_EVT_GOT_IP:
        ESP_LOGI(TAG, "Got IP address");
        if (strlen(s_state.device_id) > 0) {
            hyperwisor_ws_connect_with_device_id(
                CONFIG_HYPERWISOR_WS_HOST, CONFIG_HYPERWISOR_WS_PORT, s_state.device_id);
        }
        break;

    case HYPERWISOR_WIFI_EVT_AP_START:
        ESP_LOGI(TAG, "AP mode started");
        s_state.ap_mode_active = true;
        hyperwisor_wifi_start_provisioning_server();
        s_ap_start_time = s_state.uptime_seconds;
        s_ap_started = true;
        break;

    default:
        break;
    }
}

static void on_ws_message(const char *data, int len)
{
    ESP_LOGI(TAG, "WS recv: %.*s", len, data);
    hyperwisor_cmd_process(data);
    if (s_user_msg_cb) {
        s_user_msg_cb(data, len);
    }
}

static void on_ws_conn(bool connected)
{
    s_state.ws_connected = connected;
    if (connected) {
        ESP_LOGI(TAG, "WebSocket connected");
        /* Send device identification */
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "deviceId",  s_state.device_id);
        cJSON_AddStringToObject(p, "productId", s_state.product_id);
        cJSON_AddStringToObject(p, "version",   s_state.version);
        hyperwisor_emit("server", "identify", "register", p);

        /* s_ota_report_pending is sent from the main task to avoid
         * calling ws_send() from within the WS event callback (deadlock). */
    } else {
        ESP_LOGW(TAG, "WebSocket disconnected");
    }
}
