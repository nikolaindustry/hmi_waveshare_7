/**
 * Hyperwisor IoT Core Library for ESP-IDF (S3 port)
 * Display-agnostic core: WiFi, WebSocket, Commands, NVS
 */

#ifndef HYPERWISOR_CORE_H
#define HYPERWISOR_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HYPERWISOR_VERSION         "1.0.0-s3"
#define HYPERWISOR_NVS_NAMESPACE   "hyperwisor"

#define HYPERWISOR_DEVICE_ID_LEN   64
#define HYPERWISOR_SSID_LEN        64
#define HYPERWISOR_PASS_LEN        64
#define HYPERWISOR_EMAIL_LEN       64
#define HYPERWISOR_PRODUCT_ID_LEN  64
#define HYPERWISOR_USER_ID_LEN     64
#define HYPERWISOR_API_KEY_LEN     64
#define HYPERWISOR_SECRET_KEY_LEN  64
#define HYPERWISOR_AP_SSID_PREFIX  "NIKOLAINDUSTRY_Setup"
#define HYPERWISOR_AP_PASS         "0123456789"
#define HYPERWISOR_AP_CHANNEL      1
#define HYPERWISOR_AP_MAX_CONN     4

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

typedef void (*hyperwisor_cmd_handler_t)(const char *from, cJSON *payload);
typedef void (*hyperwisor_user_msg_cb_t)(const char *json_str, int len);

esp_err_t hyperwisor_init(void);
esp_err_t hyperwisor_start(void);

hyperwisor_state_t *hyperwisor_get_state(void);

/** @brief Acquire the state mutex before reading/writing shared state.
 *  Use in contexts where multiple threads access hyperwisor_get_state(). */
void hyperwisor_state_lock(void);

/** @brief Release the state mutex after accessing shared state. */
void hyperwisor_state_unlock(void);

esp_err_t hyperwisor_register_cmd_handler(const char *command, hyperwisor_cmd_handler_t handler);
void      hyperwisor_register_user_msg_cb(hyperwisor_user_msg_cb_t cb);

esp_err_t hyperwisor_send_response(const char *to, cJSON *payload);
esp_err_t hyperwisor_send_to(const char *target_id, void (*payload_builder)(cJSON *payload));

esp_err_t hyperwisor_set_wifi_credentials(const char *ssid, const char *password);
esp_err_t hyperwisor_set_device_id(const char *device_id);
esp_err_t hyperwisor_set_user_id(const char *user_id);
esp_err_t hyperwisor_set_api_keys(const char *api_key, const char *secret_key);
esp_err_t hyperwisor_set_credentials(const char *ssid, const char *password,
                                      const char *device_id, const char *user_id);
esp_err_t hyperwisor_clear_credentials(void);
bool      hyperwisor_has_credentials(void);

esp_err_t hyperwisor_get_device_id(char *out_buf, size_t buf_len);
esp_err_t hyperwisor_get_user_id(char *out_buf, size_t buf_len);

/* --- HSC v1 security (opt-in) ---
 * Enable the Hyperwisor Secure Channel: generates/loads the on-chip P-256 key
 * and makes the transport answer the relay's challenge on connect. Call AFTER
 * hyperwisor_init() and BEFORE hyperwisor_start(). Point the device at your
 * secured relay via CONFIG_HYPERWISOR_WS_HOST. Devices that don't call this
 * keep connecting as before (no handshake). */
esp_err_t hyperwisor_enable_security(void);

/* The device's public key (base64) — register with the platform during
 * onboarding. Empty until hyperwisor_enable_security() runs. */
esp_err_t hyperwisor_get_public_key_b64(char *out_buf, size_t buf_len);

void hyperwisor_task(void *pvParam);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_CORE_H */
