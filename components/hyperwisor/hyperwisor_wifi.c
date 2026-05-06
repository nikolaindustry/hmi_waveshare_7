/**
 * @file hyperwisor_wifi.c
 * @brief WiFi STA/AP management for ESP-IDF (S3 port: native radio, no bsp wifi hw init)
 */

#include "hyperwisor_wifi.h"
#include "hyperwisor_nvs.h"
#include "hyperwisor_core.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/ip_addr.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "HYPER_WIFI";

static bool s_wifi_initialized = false;
static bool s_sta_connected = false;
static hyperwisor_wifi_cb_t s_user_cb = NULL;
static httpd_handle_t s_prov_server = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (s_user_cb) {
            s_user_cb(HYPERWISOR_WIFI_EVT_DISCONNECTED, NULL);
        }
        /* Auto-reconnect after 5s */
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_connected = true;
        if (s_user_cb) {
            s_user_cb(HYPERWISOR_WIFI_EVT_CONNECTED, NULL);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_user_cb) {
            s_user_cb(HYPERWISOR_WIFI_EVT_AP_START, NULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_user_cb) {
            s_user_cb(HYPERWISOR_WIFI_EVT_GOT_IP, &event->ip_info.ip);
        }
    }
}

esp_err_t hyperwisor_wifi_init(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    /* esp_event_loop_create_default may have been created by application, tolerate. */
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }

    /* S3: native WiFi radio, no bsp hardware init required */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

void hyperwisor_wifi_register_cb(hyperwisor_wifi_cb_t cb)
{
    s_user_cb = cb;
}

esp_err_t hyperwisor_wifi_connect_sta(void)
{
    hyperwisor_state_t *state = hyperwisor_get_state();
    if (strlen(state->ssid) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return hyperwisor_wifi_connect(state->ssid, state->password);
}

esp_err_t hyperwisor_wifi_connect(const char *ssid, const char *pass)
{
    esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Save credentials */
    hyperwisor_nvs_set_str("ssid", ssid);
    hyperwisor_nvs_set_str("pass", pass);

    ESP_LOGI(TAG, "Connecting to %s...", ssid);
    return ESP_OK;
}

esp_err_t hyperwisor_wifi_start_ap(const char *ssid, const char *pass)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.channel = HYPERWISOR_AP_CHANNEL;
    wifi_config.ap.max_connection = HYPERWISOR_AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    if (pass && strlen(pass) > 0) {
        strncpy((char *)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password) - 1);
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: %s", ssid);
    return ESP_OK;
}

esp_err_t hyperwisor_wifi_stop_ap(void)
{
    ESP_ERROR_CHECK(esp_wifi_stop());
    return ESP_OK;
}

bool hyperwisor_wifi_is_connected(void)
{
    return s_sta_connected;
}

esp_err_t hyperwisor_wifi_get_ip(char *out_buf)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
    snprintf(out_buf, 16, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

/* ==================== Provisioning HTTP Server ==================== */

static const char *PROV_SUCCESS_HTML =
    "<!DOCTYPE html>"
    "<html><head><title>Provisioning Success</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
    "display:flex;flex-direction:column;justify-content:center;align-items:center;height:100vh;"
    "margin:0;background:#f0f2f5;text-align:center;padding:20px}"
    ".card{background:white;padding:40px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.1);max-width:400px}"
    "h1{color:#28a745;font-size:24px}"
    "p{color:#666;font-size:16px;margin-bottom:30px}"
    "a{display:inline-block;background:#007aff;color:white;padding:15px 30px;text-decoration:none;"
    "border-radius:8px;font-weight:500;font-size:18px}"
    "</style></head><body>"
    "<div class=\"card\"><h1>Configuration Sent!</h1>"
    "<p>Your device will now attempt to connect to the new Wi-Fi network.</p>"
    "<a href=\"hypervisorv4://provisioning?status=success&message=Device_is_connecting_to_the_new_network.\">"
    "Return to App</a></div></body></html>";

static const char *PROV_ERROR_HTML =
    "<!DOCTYPE html>"
    "<html><head><title>Provisioning Error</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
    "display:flex;flex-direction:column;justify-content:center;align-items:center;height:100vh;"
    "margin:0;background:#f0f2f5;text-align:center;padding:20px}"
    ".card{background:white;padding:40px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.1);max-width:400px}"
    "h1{color:#dc3545;font-size:24px}"
    "p{color:#666;font-size:16px;margin-bottom:30px}"
    "a{display:inline-block;background:#6c757d;color:white;padding:15px 30px;text-decoration:none;"
    "border-radius:8px;font-weight:500;font-size:18px}"
    "</style></head><body>"
    "<div class=\"card\"><h1>Provisioning Failed</h1>"
    "<p>Error: Missing SSID</p>"
    "<a href=\"hypervisorv4://provisioning?status=error&message=Missing_SSID\">"
    "Return to App</a></div></body></html>";

static esp_err_t prov_handler(httpd_req_t *req)
{
    char query[512];
    char ssid[64] = {0};
    char password[64] = {0};
    char device_id[64] = {0};
    char user_id[64] = {0};

    size_t qlen = httpd_req_get_url_query_len(req);
    ESP_LOGW(TAG, "=== /api/provision GET, query_len=%u ===", (unsigned)qlen);

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGW(TAG, "raw query: %s", query);
        esp_err_t e1 = httpd_query_key_value(query, "ssid",      ssid,      sizeof(ssid));
        esp_err_t e2 = httpd_query_key_value(query, "password",  password,  sizeof(password));
        esp_err_t e3 = httpd_query_key_value(query, "target_id", device_id, sizeof(device_id));
        esp_err_t e4 = httpd_query_key_value(query, "user_id",   user_id,   sizeof(user_id));
        ESP_LOGW(TAG, "parsed: ssid=[%s] (e=%d)",      ssid,      e1);
        ESP_LOGW(TAG, "parsed: pass len=%u (e=%d)",     (unsigned)strlen(password), e2);
        ESP_LOGW(TAG, "parsed: target_id=[%s] (e=%d)", device_id, e3);
        ESP_LOGW(TAG, "parsed: user_id=[%s] (e=%d)",   user_id,   e4);
        if (e3 != ESP_OK || strlen(device_id) == 0) {
            ESP_LOGE(TAG, "target_id missing/empty -- cloud will be offline. "
                          "App must include 'target_id' in /api/provision query.");
        }
    } else {
        ESP_LOGE(TAG, "failed to read query string");
    }

    if (strlen(ssid) > 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PROV_SUCCESS_HTML, HTTPD_RESP_USE_STRLEN);

        /* Save credentials and restart */
        hyperwisor_set_credentials(ssid, password, device_id, user_id);
        ESP_LOGI(TAG, "Provisioned: ssid=%s device=%s user=%s", ssid, device_id, user_id);

        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PROV_ERROR_HTML, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

esp_err_t hyperwisor_wifi_start_provisioning_server(void)
{
    if (s_prov_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_prov_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start prov server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t prov_uri = {
        .uri = "/api/provision",
        .method = HTTP_GET,
        .handler = prov_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_prov_server, &prov_uri);

    ESP_LOGI(TAG, "Provisioning server started on port 80");
    return ESP_OK;
}

esp_err_t hyperwisor_wifi_stop_provisioning_server(void)
{
    if (s_prov_server) {
        httpd_stop(s_prov_server);
        s_prov_server = NULL;
        ESP_LOGI(TAG, "Provisioning server stopped");
    }
    return ESP_OK;
}
