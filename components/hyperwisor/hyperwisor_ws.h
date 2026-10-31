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

/* Retry delay after the server REJECTED our identity, as opposed to the link
 * failing. Deliberately short and constant: "unknown device" almost always
 * means the owner is partway through onboarding and the credential row is
 * seconds away, so an exponential backoff punishes the one case that is about
 * to fix itself. An unreachable host still gets the backoff above. */
#define HYPERWISOR_WS_AUTH_RETRY_MS    5000
/* WebSocket PING cadence -- protocol control frames on the websocket itself.
 *
 * These used to be wired to the client's keep_alive_* fields, which are not
 * the websocket ping at all: in esp_websocket_client they are the TCP socket's
 * SO_KEEPALIVE options. The names read the same and the layers do not, so the
 * device was configuring TCP probes while sending websocket pings on the
 * library default. The TCP settings are now separate, below. */
#define HYPERWISOR_WS_PING_INTERVAL_MS 15000

/* How long without a PONG before the DEVICE gives up on the connection.
 *
 * Deliberately three ping periods, not one. This field was previously unset,
 * which left the feature off entirely, so setting it at all introduces a new
 * way for the device to hang up -- and a one-strike version of it would be
 * precisely the bug that cost a week on the HSC side, where a single lost
 * pong terminated healthy connections. A lost control frame is routine; three
 * in a row is a connection worth abandoning. */
#define HYPERWISOR_WS_PONG_TIMEOUT_MS  50000

/* TCP keepalive -- probes the socket underneath the websocket.
 *
 * Was idle 15s / interval 3s / count 2, which declares a connection dead about
 * 21 seconds after it goes quiet if the probes are lost. The HSC is in
 * Singapore and these devices are not, so losing two probes three seconds
 * apart is an ordinary afternoon rather than evidence of a dead peer -- and
 * the device then dropped a socket that was working, which is what the HSC
 * logged as the HMI closing itself (code 1006, pings and data stopping
 * together, reconnecting 10-18s later) on 2026-10-29.
 *
 * 30s idle, 10s apart, 4 probes tolerates roughly 70 seconds of loss before
 * giving up. That is the same judgement the HSC now makes with
 * MAX_MISSED_PONGS: on the open internet a lost control frame is routine, so
 * tolerate several before concluding anything. */
#define HYPERWISOR_WS_TCP_KEEPALIVE_IDLE_S     30
#define HYPERWISOR_WS_TCP_KEEPALIVE_INTERVAL_S 10
#define HYPERWISOR_WS_TCP_KEEPALIVE_COUNT      4

typedef void (*hyperwisor_ws_msg_cb_t)(const char *data, int len);
typedef void (*hyperwisor_ws_conn_cb_t)(bool connected);

/* HSC signer: sign the challenge (nonce, ts) and write the base64 signature
 * into sig_out. Return ESP_OK on success. Wired to hyperwisor_hsc_sign(). */
typedef esp_err_t (*hyperwisor_ws_signer_cb_t)(const char *nonce, const char *ts,
                                               char *sig_out, size_t sig_len);

/* Enable the HSC handshake. When enabled, the transport is not reported as
 * connected (no conn_cb(true), no message delivery) until the HSC returns
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

/* Call once per core-task tick. Completes any client teardown the
 * websocket task deferred (it may not stop/destroy itself). */
void      hyperwisor_ws_service(void);

/* False while the post-disconnect backoff window is still open. */
bool      hyperwisor_ws_reconnect_due(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_WS_H */
