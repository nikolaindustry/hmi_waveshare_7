/**
 * @file hyperwisor_wifi.h
 * @brief WiFi management: STA, AP, provisioning
 */

#ifndef HYPERWISOR_WIFI_H
#define HYPERWISOR_WIFI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AP config macros are defined in hyperwisor_core.h to match Arduino library */

/**
 * @brief Outcome of the most recent provisioning attempt.
 *
 * Provisioning is a trial: the station is brought up alongside the setup AP
 * and the credentials are only committed to NVS once they are known to work.
 * This is what /api/status reports, and it is the only way a caller can tell a
 * wrong password from a working one -- the device is the sole party that sees
 * the association failure.
 */
typedef enum {
    HYPERWISOR_PROV_IDLE = 0,       /**< nothing attempted this boot          */
    HYPERWISOR_PROV_TRYING,         /**< joining; ask again in a second       */
    HYPERWISOR_PROV_CONNECTED,      /**< joined and addressed; creds kept     */
    HYPERWISOR_PROV_AUTH_FAILED,    /**< network refused the password         */
    HYPERWISOR_PROV_AP_NOT_FOUND,   /**< that SSID is not on the air (or 5GHz)*/
    HYPERWISOR_PROV_TIMEOUT,        /**< no address in time; cause unknown    */
} hyperwisor_prov_state_t;

/**
 * @brief WiFi event callback types
 */
typedef enum {
    HYPERWISOR_WIFI_EVT_CONNECTED,
    HYPERWISOR_WIFI_EVT_DISCONNECTED,
    HYPERWISOR_WIFI_EVT_AP_START,
    HYPERWISOR_WIFI_EVT_AP_STOP,
    HYPERWISOR_WIFI_EVT_GOT_IP,
} hyperwisor_wifi_event_t;

typedef void (*hyperwisor_wifi_cb_t)(hyperwisor_wifi_event_t evt, void *data);

esp_err_t hyperwisor_wifi_init(void);
void      hyperwisor_wifi_register_cb(hyperwisor_wifi_cb_t cb);
esp_err_t hyperwisor_wifi_connect_sta(void);
esp_err_t hyperwisor_wifi_connect(const char *ssid, const char *pass);
esp_err_t hyperwisor_wifi_start_ap(const char *ssid, const char *pass);
esp_err_t hyperwisor_wifi_stop_ap(void);
bool      hyperwisor_wifi_is_connected(void);
esp_err_t hyperwisor_wifi_get_ip(char *out_buf);
esp_err_t hyperwisor_wifi_start_provisioning_server(void);
esp_err_t hyperwisor_wifi_stop_provisioning_server(void);

/** @brief Outcome of the most recent provisioning trial. */
hyperwisor_prov_state_t hyperwisor_wifi_prov_state(void);

/** @brief Raw wifi_err_reason_t from the last station disconnect, 0 if none. */
uint8_t hyperwisor_wifi_prov_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_WIFI_H */
