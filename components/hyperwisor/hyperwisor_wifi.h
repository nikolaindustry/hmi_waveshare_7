/**
 * @file hyperwisor_wifi.h
 * @brief WiFi management: STA, AP, provisioning
 */

#ifndef HYPERWISOR_WIFI_H
#define HYPERWISOR_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AP config macros are defined in hyperwisor_core.h to match Arduino library */

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

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_WIFI_H */
