/**
 * @file hyperwisor_ws.h
 * @brief WebSocket client for nikolaindustry-realtime protocol
 */

#ifndef HYPERWISOR_WS_H
#define HYPERWISOR_WS_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HYPERWISOR_WS_RECONNECT_MS     5000   /* Initial reconnect delay */
#define HYPERWISOR_WS_RECONNECT_MAX_MS 60000  /* Max reconnect delay (cap) */
#define HYPERWISOR_WS_PING_INTERVAL_MS 15000
#define HYPERWISOR_WS_PONG_TIMEOUT_MS  3000
#define HYPERWISOR_WS_PONG_FAIL_COUNT  2

typedef void (*hyperwisor_ws_msg_cb_t)(const char *data, int len);
typedef void (*hyperwisor_ws_conn_cb_t)(bool connected);

/* HSC signer: sign the challenge (nonce, ts) and write the base64 signature
 * into sig_out. Return ESP_OK on success. Wired to hyperwisor_hsc_sign(). */
typedef esp_err_t (*hyperwisor_ws_signer_cb_t)(const char *nonce, const char *ts,
                                               char *sig_out, size_t sig_len);

/* Enable the HSC handshake. When enabled, the transport is not reported as
 * connected (no conn_cb(true), no message delivery) until the relay returns
 * auth_ok. `device_id` is used to build the auth frame. */
void hyperwisor_ws_enable_hsc(hyperwisor_ws_signer_cb_t signer, const char *device_id);

esp_err_t hyperwisor_ws_init(void);
esp_err_t hyperwisor_ws_connect(const char *uri);
esp_err_t hyperwisor_ws_connect_with_device_id(const char *host, int port, const char *device_id);
void      hyperwisor_ws_disconnect(void);
void      hyperwisor_ws_register_msg_cb(hyperwisor_ws_msg_cb_t cb);
void      hyperwisor_ws_register_conn_cb(hyperwisor_ws_conn_cb_t cb);
esp_err_t hyperwisor_ws_send(const char *data, int len);
esp_err_t hyperwisor_ws_send_json(cJSON *json);
bool      hyperwisor_ws_is_connected(void);
bool      hyperwisor_ws_is_started(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_WS_H */
