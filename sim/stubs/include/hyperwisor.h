/* Minimal hyperwisor.h for the sim -- the real umbrella header pulls in
 * esp_http_client, esp_websocket_client, cJSON and the WiFi stack. Only
 * the state struct and the handful of setters the UI touches are here. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Kept in sync with hyperwisor_core.h -- the Cloud screen prints these. */
#define HYPERWISOR_AP_SSID_PREFIX  "NIKOLAINDUSTRY_Setup"
#define HYPERWISOR_AP_PASS         "0123456789"

#define HYPERWISOR_DEVICE_ID_LEN   64
#define HYPERWISOR_SSID_LEN        33
#define HYPERWISOR_PASS_LEN        65
#define HYPERWISOR_EMAIL_LEN       64
#define HYPERWISOR_PRODUCT_ID_LEN  64
#define HYPERWISOR_USER_ID_LEN     64
#define HYPERWISOR_API_KEY_LEN     64
#define HYPERWISOR_SECRET_KEY_LEN  64

typedef struct {
    char device_id[HYPERWISOR_DEVICE_ID_LEN];
    char ssid[HYPERWISOR_SSID_LEN];
    char password[HYPERWISOR_PASS_LEN];
    char email[HYPERWISOR_EMAIL_LEN];
    char product_id[HYPERWISOR_PRODUCT_ID_LEN];
    char user_id[HYPERWISOR_USER_ID_LEN];
    char api_key[HYPERWISOR_API_KEY_LEN];
    char secret_key[HYPERWISOR_SECRET_KEY_LEN];
    char version[16];
    bool wifi_connected;
    bool ws_connected;
    bool ap_mode_active;
    bool ntp_initialized;
    uint32_t uptime_seconds;
} hyperwisor_state_t;

hyperwisor_state_t *hyperwisor_get_state(void);
esp_err_t hyperwisor_set_device_id(const char *id);
esp_err_t hyperwisor_set_user_id(const char *id);
esp_err_t hyperwisor_clear_credentials(void);
