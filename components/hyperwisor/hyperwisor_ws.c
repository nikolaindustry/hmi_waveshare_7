/**
 * @file hyperwisor_ws.c
 * @brief WebSocket client using esp_websocket_client
 */

#include "hyperwisor_ws.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "HYPER_WS";

static esp_websocket_client_handle_t s_ws_client = NULL;
static hyperwisor_ws_msg_cb_t s_msg_cb = NULL;
static hyperwisor_ws_conn_cb_t s_conn_cb = NULL;
static bool s_ws_connected = false;
static int s_reconnect_attempt = 0;  /* For exponential backoff tracking */

/* HSC v1 handshake state */
static bool s_hsc_enabled = false;
static bool s_authenticated = false;
static hyperwisor_ws_signer_cb_t s_signer = NULL;
static char s_device_id[64] = {0};

void hyperwisor_ws_enable_hsc(hyperwisor_ws_signer_cb_t signer, const char *device_id)
{
    s_signer = signer;
    if (device_id) {
        strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
        s_device_id[sizeof(s_device_id) - 1] = '\0';
    }
    s_hsc_enabled = true;
    ESP_LOGI(TAG, "HSC: handshake enabled");
}

/* Handle an HSC control frame (challenge / auth_ok / auth_fail). */
static void hsc_handle_data(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(t, "challenge") == 0) {
        const cJSON *jn = cJSON_GetObjectItem(root, "nonce");
        const cJSON *jt = cJSON_GetObjectItem(root, "ts");
        const char *nonce = cJSON_IsString(jn) ? jn->valuestring : "";
        char ts[24] = {0};
        if (cJSON_IsNumber(jt)) {
            /* ts is a JS millisecond integer; double holds it exactly (<2^53). */
            snprintf(ts, sizeof(ts), "%.0f", jt->valuedouble);
        } else if (cJSON_IsString(jt)) {
            strncpy(ts, jt->valuestring, sizeof(ts) - 1);
        }
        char sig[128] = {0};
        if (s_signer && s_signer(nonce, ts, sig, sizeof(sig)) == ESP_OK) {
            cJSON *out = cJSON_CreateObject();
            cJSON_AddStringToObject(out, "hsc", "1");
            cJSON_AddStringToObject(out, "type", "auth");
            cJSON_AddStringToObject(out, "deviceId", s_device_id);
            cJSON_AddStringToObject(out, "sig", sig);
            hyperwisor_ws_send_json(out);
            cJSON_Delete(out);
            ESP_LOGI(TAG, "HSC: signed challenge -> sent auth");
        } else {
            ESP_LOGE(TAG, "HSC: signing failed");
        }
    } else if (strcmp(t, "auth_ok") == 0) {
        s_authenticated = true;
        ESP_LOGI(TAG, "HSC: authenticated");
        if (s_conn_cb) s_conn_cb(true);
    } else if (strcmp(t, "auth_fail") == 0) {
        const cJSON *reason = cJSON_GetObjectItem(root, "reason");
        ESP_LOGE(TAG, "HSC: auth rejected: %s",
                 cJSON_IsString(reason) ? reason->valuestring : "unknown");
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_ws_connected = true;
        s_authenticated = false;
        s_reconnect_attempt = 0;  /* Reset backoff on successful connect */
        if (s_hsc_enabled) {
            /* Wait for the HSC challenge -> auth_ok before reporting connected. */
            ESP_LOGI(TAG, "HSC: awaiting challenge...");
        } else if (s_conn_cb) {
            s_conn_cb(true);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected (attempt %d)", s_reconnect_attempt + 1);
        s_ws_connected = false;
        s_authenticated = false;
        /* Exponential backoff with jitter:
         * delay = min(base * 2^attempt, max) + random(0, base)
         * This avoids thundering herd when many devices disconnect simultaneously.
         * The esp_websocket_client library handles reconnection automatically
         * using the reconnect_timeout_ms we set at creation time. For backoff,
         * we destroy the old client and recreate with a longer timeout. */
        {
            int base = HYPERWISOR_WS_RECONNECT_MS;
            int delay_ms = base << s_reconnect_attempt;  /* base * 2^attempt */
            if (delay_ms > HYPERWISOR_WS_RECONNECT_MAX_MS) {
                delay_ms = HYPERWISOR_WS_RECONNECT_MAX_MS;
            }
            /* Add random jitter: 0..base ms */
            delay_ms += (esp_random() % base);
            ESP_LOGI(TAG, "Next reconnect in %d ms (backoff attempt %d)", delay_ms, s_reconnect_attempt + 1);

            /* Destroy current client and set flag so core task recreates
             * with the new timeout. The reconnect delay is achieved by
             * the core task waiting before calling ws_connect again. */
            if (s_ws_client) {
                esp_websocket_client_stop(s_ws_client);
                esp_websocket_client_destroy(s_ws_client);
                s_ws_client = NULL;
            }
            s_reconnect_attempt++;
        }
        if (s_conn_cb) {
            s_conn_cb(false);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            if (s_hsc_enabled && !s_authenticated) {
                /* Before auth: only handshake frames are processed; drop the rest. */
                hsc_handle_data(data->data_ptr, data->data_len);
            } else if (s_msg_cb) {
                s_msg_cb(data->data_ptr, data->data_len);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

esp_err_t hyperwisor_ws_init(void)
{
    ESP_LOGI(TAG, "WebSocket client initialized");
    return ESP_OK;
}

esp_err_t hyperwisor_ws_connect(const char *uri)
{
    /* If a client already exists, DO NOT destroy & recreate from this
     * context -- the WS library has its own internal task + event loop,
     * and destroying while that loop is active races the mutex and
     * triggers `assert failed: spinlock_acquire lock->count == 0` inside
     * esp_event_loop_delete(). The library already handles reconnects
     * automatically via reconnect_timeout_ms=HYPERWISOR_WS_RECONNECT_MS,
     * so on a disconnect event we simply leave the client alone and let
     * it reconnect itself. Re-creation should only happen after an
     * explicit hyperwisor_ws_disconnect() (e.g. credentials changed). */
    if (s_ws_client != NULL) {
        return ESP_OK;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .reconnect_timeout_ms = HYPERWISOR_WS_RECONNECT_MS,
        .network_timeout_ms = 10000,
        /* The default websocket_task stack (4 KB) overflows inside
         * mbedtls_x509_crt_parse_der() when verifying Render's full
         * ECDSA chain against the bundled CA store (cert chain is
         * ~2.5 KB and ECDSA sig verify pushes another ~2 KB of locals).
         * 8 KB is the minimum that completes the handshake cleanly. */
        .task_stack = 8192,
        .keep_alive_enable = true,
        .keep_alive_idle = HYPERWISOR_WS_PING_INTERVAL_MS / 1000,
        .keep_alive_interval = HYPERWISOR_WS_PONG_TIMEOUT_MS / 1000,
        .keep_alive_count = HYPERWISOR_WS_PONG_FAIL_COUNT,
        /* Attach IDF's bundled CA store so TLS can verify the
         * Google Trust Services ECDSA chain presented by *.onrender.com.
         *
         * Do NOT set skip_cert_common_name_check = true here. In esp-tls
         * (components/esp-tls/esp_tls_mbedtls.c, set_client_config) that
         * flag maps to cfg->skip_common_name, which then calls
         *     mbedtls_ssl_set_hostname(&tls->ssl, NULL);
         * That removes the SNI extension from the ClientHello entirely.
         * Render's edge is a Cloudflare-style tenant router and replies
         * with a fatal TLS alert [2:40] (handshake_failure) whenever the
         * ClientHello lacks SNI, because it cannot pick the ECDSA cert
         * for the correct tenant without it. The certificate SAN is
         * `*.onrender.com`, so normal CN/SAN verification succeeds. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Render edge's Cloudflare-style fingerprinting requires a
         * normal-looking User-Agent; without one we get a fatal TLS
         * alert during handshake (MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE). */
        .user_agent = "ESP32-Hyperwisor/1.0",
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_client) {
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                   ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Connecting to %s...", uri);
    return ESP_OK;
}

void hyperwisor_ws_disconnect(void)
{
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        s_ws_connected = false;
    }
}

void hyperwisor_ws_register_msg_cb(hyperwisor_ws_msg_cb_t cb)
{
    s_msg_cb = cb;
}

void hyperwisor_ws_register_conn_cb(hyperwisor_ws_conn_cb_t cb)
{
    s_conn_cb = cb;
}

esp_err_t hyperwisor_ws_send(const char *data, int len)
{
    if (!s_ws_client || !s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int l = (len > 0) ? len : strlen(data);
    int ret = esp_websocket_client_send_text(s_ws_client, data, l, pdMS_TO_TICKS(5000));
    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t hyperwisor_ws_send_json(cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    if (!str) {
        return ESP_FAIL;
    }
    esp_err_t err = hyperwisor_ws_send(str, 0);
    free(str);
    return err;
}

bool hyperwisor_ws_is_connected(void)
{
    /* When HSC is on, "connected" means authenticated. */
    if (s_hsc_enabled) return s_ws_connected && s_authenticated;
    return s_ws_connected;
}

bool hyperwisor_ws_is_started(void)
{
    return s_ws_client != NULL;
}

esp_err_t hyperwisor_ws_connect_with_device_id(const char *host, int port, const char *device_id)
{
    if (!host || !device_id) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Omit explicit :443 for wss and :80 for ws. Some TLS edges
     * (Render / Cloudflare-style proxies) reject the ClientHello
     * when the SNI hostname accidentally includes the port, and
     * some WS client versions propagate the port into SNI when
     * the URI contains an explicit `:port`. Using the scheme's
     * default port keeps the URI clean and matches what the
     * Arduino `beginSSL(host, 443, path)` reference produces. */
    char uri[256];
    const char *scheme = (port == 443) ? "wss" : "ws";
    bool default_port = (port == 443) || (port == 80);
    if (default_port) {
        snprintf(uri, sizeof(uri), "%s://%s/?id=%s", scheme, host, device_id);
    } else {
        snprintf(uri, sizeof(uri), "%s://%s:%d/?id=%s", scheme, host, port, device_id);
    }

    return hyperwisor_ws_connect(uri);
}
