/**
 * @file hyperwisor_wifi.c
 * @brief WiFi STA/AP management for ESP-IDF (S3 port: native radio, no bsp wifi hw init)
 *
 * Provisioning here is a *trial*, not a commit. See hyperwisor_wifi_prov_state()
 * and the long comment above prov_handler() for why that distinction is the
 * whole point of this file.
 */

#include "hyperwisor_wifi.h"
#include "hyperwisor_nvs.h"
#include "hyperwisor_core.h"
#include "hyperwisor_hsc.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/ip_addr.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "HYPER_WIFI";

/* How long a provisioning trial waits for an IP before giving up. A join that
 * is going to work almost always completes in under 8s; the rest is headroom
 * for a slow DHCP server. */
#define PROV_TRIAL_TIMEOUT_MS   25000

/* How long after boot a device with stored credentials waits for an IP before
 * it also raises its setup AP. Without this a device whose network changed --
 * or which was provisioned with a wrong password by an older firmware -- has no
 * way back at all. */
#define JOIN_FALLBACK_MS        45000

/* Reconnect backoff. Deliberately not a vTaskDelay in the event handler: that
 * blocks the system event task, so every other WiFi and IP event in the
 * process queues behind it. */
#define RECONNECT_DELAY_MS      5000

static bool s_wifi_initialized = false;
static bool s_sta_connected = false;
static hyperwisor_wifi_cb_t s_user_cb = NULL;
static httpd_handle_t s_prov_server = NULL;

/* Radio composition. esp_wifi_set_mode() takes the whole mode, so AP and STA
 * cannot each set it independently without one silently cancelling the other --
 * which is what used to happen: start_ap() after connect() dropped the station. */
static bool s_want_sta = false;
static bool s_want_ap  = false;
static bool s_netif_sta_made = false;
static bool s_netif_ap_made  = false;
static bool s_radio_started  = false;

/* The AP's own credentials, kept so a fallback AP can be raised later without
 * the caller having to supply them again. */
static char s_ap_ssid[HYPERWISOR_SSID_LEN] = {0};
static char s_ap_pass[HYPERWISOR_PASS_LEN] = {0};
static bool s_ap_configured = false;

/* Provisioning trial state. */
static volatile hyperwisor_prov_state_t s_prov_state = HYPERWISOR_PROV_IDLE;
static volatile uint8_t s_prov_reason = 0;       /* raw wifi_err_reason_t */
static bool  s_trial_active = false;
static char  s_trial_ssid[HYPERWISOR_SSID_LEN] = {0};
static char  s_trial_pass[HYPERWISOR_PASS_LEN] = {0};
static char  s_prev_ssid[HYPERWISOR_SSID_LEN]  = {0};
static char  s_prev_pass[HYPERWISOR_PASS_LEN]  = {0};

static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_trial_timer     = NULL;
static esp_timer_handle_t s_fallback_timer  = NULL;

static esp_err_t apply_mode(void);
static esp_err_t sta_configure_and_connect(const char *ssid, const char *pass);
static void raise_fallback_ap(void);

/* ==================== mode composition ==================== */

static esp_err_t apply_mode(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (s_want_sta && s_want_ap) mode = WIFI_MODE_APSTA;
    else if (s_want_sta)         mode = WIFI_MODE_STA;
    else if (s_want_ap)          mode = WIFI_MODE_AP;

    if (mode == WIFI_MODE_NULL) {
        if (s_radio_started) {
            esp_wifi_stop();
            s_radio_started = false;
        }
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    if (!s_radio_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_radio_started = true;
    }
    return ESP_OK;
}

/* ==================== trial bookkeeping ==================== */

/* A join failure is either definitive or transient, and treating the two alike
 * is what made provisioning unreportable. AUTH_FAIL and the WPA handshake
 * timeout mean the password is wrong; NO_AP_FOUND means that SSID is not on the
 * air (a typo, or a 5 GHz-only network this radio cannot see). Those are
 * answers. Everything else -- a beacon miss, an AP still booting, a transient
 * association failure -- deserves another attempt inside the trial window. */
static bool reason_is_definitive(uint8_t reason, hyperwisor_prov_state_t *out)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        *out = HYPERWISOR_PROV_AUTH_FAILED;
        return true;
    case WIFI_REASON_NO_AP_FOUND:
        *out = HYPERWISOR_PROV_AP_NOT_FOUND;
        return true;
    default:
        return false;
    }
}

static void trial_finish(hyperwisor_prov_state_t result)
{
    if (!s_trial_active) return;
    s_trial_active = false;
    if (s_trial_timer) esp_timer_stop(s_trial_timer);

    if (result == HYPERWISOR_PROV_CONNECTED) {
        /* Only now are these credentials worth keeping. */
        hyperwisor_set_wifi_credentials(s_trial_ssid, s_trial_pass);
        ESP_LOGI(TAG, "provisioning trial joined %s -- credentials committed",
                 s_trial_ssid);
    } else {
        /* Put back whatever was working before, so a failed attempt never
         * costs a device its existing network. Before this, a mistyped
         * password overwrote a good one in NVS and the only recovery was a
         * cable. */
        hyperwisor_set_wifi_credentials(s_prev_ssid, s_prev_pass);
        ESP_LOGW(TAG, "provisioning trial failed (%d), reverted to %s",
                 (int)result, s_prev_ssid[0] ? s_prev_ssid : "(none)");
    }
    s_prov_state = result;
}

static void trial_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "provisioning trial timed out after %d ms", PROV_TRIAL_TIMEOUT_MS);
    trial_finish(HYPERWISOR_PROV_TIMEOUT);
}

static void reconnect_cb(void *arg)
{
    (void)arg;
    if (s_want_sta) esp_wifi_connect();
}

static void fallback_cb(void *arg)
{
    (void)arg;
    if (s_sta_connected) return;
    ESP_LOGW(TAG, "no IP %d ms after boot -- raising setup AP so the device "
                  "can be re-provisioned", JOIN_FALLBACK_MS);
    raise_fallback_ap();
}

/* ==================== events ==================== */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = d ? d->reason : 0;
        s_prov_reason = reason;
        s_sta_connected = false;

        if (s_trial_active) {
            hyperwisor_prov_state_t verdict;
            if (reason_is_definitive(reason, &verdict)) {
                ESP_LOGW(TAG, "trial join refused: reason=%u", (unsigned)reason);
                trial_finish(verdict);
                /* Deliberately no reconnect: the answer is known and the AP is
                 * still up, so the app can read it and the person can retype. */
                if (s_user_cb) s_user_cb(HYPERWISOR_WIFI_EVT_DISCONNECTED, NULL);
                return;
            }
            ESP_LOGW(TAG, "trial join attempt failed: reason=%u, retrying",
                     (unsigned)reason);
        }

        if (s_user_cb) {
            s_user_cb(HYPERWISOR_WIFI_EVT_DISCONNECTED, NULL);
        }
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_MS * 1000ULL);
        }
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
        if (s_fallback_timer) esp_timer_stop(s_fallback_timer);
        trial_finish(HYPERWISOR_PROV_CONNECTED);
        if (s_user_cb) {
            s_user_cb(HYPERWISOR_WIFI_EVT_GOT_IP, &event->ip_info.ip);
        }
    }
}

/* ==================== init ==================== */

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

    const esp_timer_create_args_t rc = { .callback = reconnect_cb, .name = "hw_recon" };
    ESP_ERROR_CHECK(esp_timer_create(&rc, &s_reconnect_timer));
    const esp_timer_create_args_t tt = { .callback = trial_timeout_cb, .name = "hw_trial" };
    ESP_ERROR_CHECK(esp_timer_create(&tt, &s_trial_timer));
    const esp_timer_create_args_t fb = { .callback = fallback_cb, .name = "hw_apfb" };
    ESP_ERROR_CHECK(esp_timer_create(&fb, &s_fallback_timer));

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

void hyperwisor_wifi_register_cb(hyperwisor_wifi_cb_t cb)
{
    s_user_cb = cb;
}

/* ==================== station ==================== */

static esp_err_t sta_configure_and_connect(const char *ssid, const char *pass)
{
    if (!s_netif_sta_made) {
        esp_netif_create_default_wifi_sta();
        s_netif_sta_made = true;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    /* An authmode threshold of WPA2_PSK rejects anything weaker, open networks
     * included -- so a device told to join an open network sat refusing it
     * while the UI offered "leave blank for open networks". Gate the threshold
     * on whether a password was actually given. */
    wifi_config.sta.threshold.authmode =
        (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_want_sta = true;
    ESP_ERROR_CHECK(apply_mode());
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* Power save is an explicit choice, never an inherited default.
     *
     * IDF defaults to WIFI_PS_MIN_MODEM: the radio wakes only for DTIM
     * beacons. That is right on a battery and wrong on anything mains-powered
     * -- on a busy 2.4 GHz band the station misses beacons, gives up on the AP
     * and disassociates (wifi:bcn_timeout -> run -> init), so the connection
     * drops every couple of minutes in a way that looks like a server fault.
     * A wall panel lost a day to exactly that in October 2026.
     *
     * Hence Kconfig rather than a hardcoded call: a sensor node on a coin cell
     * wants the opposite of what a panel wants, and neither should get it by
     * accident. See HYPERWISOR_WIFI_POWER_SAVE. */
#if CONFIG_HYPERWISOR_WIFI_PS_MIN_MODEM
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
#elif CONFIG_HYPERWISOR_WIFI_PS_MAX_MODEM
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif

    /* esp_wifi_start() fires STA_START, whose handler calls esp_wifi_connect().
     * On a radio that was already running there is no such event, so connect
     * here too -- idempotent either way. */
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting to %s...", ssid);
    return ESP_OK;
}

esp_err_t hyperwisor_wifi_connect_sta(void)
{
    hyperwisor_state_t *state = hyperwisor_get_state();
    if (strlen(state->ssid) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = hyperwisor_wifi_connect(state->ssid, state->password);

    /* Stored credentials are not proof they still work. Arm the fallback so a
     * device whose network changed raises its AP instead of retrying in
     * silence forever. */
    if (err == ESP_OK && s_fallback_timer) {
        esp_timer_stop(s_fallback_timer);
        esp_timer_start_once(s_fallback_timer, JOIN_FALLBACK_MS * 1000ULL);
    }
    return err;
}

esp_err_t hyperwisor_wifi_connect(const char *ssid, const char *pass)
{
    esp_err_t err = sta_configure_and_connect(ssid, pass);
    if (err == ESP_OK) {
        /* Caller-initiated connect (not a trial): keep the old behaviour of
         * persisting straight away, since this path is used to re-apply
         * credentials that are already stored. */
        hyperwisor_nvs_set_str("ssid", ssid);
        hyperwisor_nvs_set_str("pass", pass);
    }
    return err;
}

/* ==================== access point ==================== */

esp_err_t hyperwisor_wifi_start_ap(const char *ssid, const char *pass)
{
    if (!s_netif_ap_made) {
        esp_netif_create_default_wifi_ap();
        s_netif_ap_made = true;
    }

    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid ? ssid : "");
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", pass ? pass : "");
    s_ap_configured = true;

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, s_ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(s_ap_ssid);
    wifi_config.ap.channel = HYPERWISOR_AP_CHANNEL;
    wifi_config.ap.max_connection = HYPERWISOR_AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    if (s_ap_pass[0]) {
        strncpy((char *)wifi_config.ap.password, s_ap_pass, sizeof(wifi_config.ap.password) - 1);
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    s_want_ap = true;
    ESP_ERROR_CHECK(apply_mode());
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_LOGI(TAG, "AP started: %s (mode %s)", s_ap_ssid,
             s_want_sta ? "APSTA" : "AP");
    return ESP_OK;
}

static void raise_fallback_ap(void)
{
    if (s_want_ap) return;   /* already up */

    char ssid[HYPERWISOR_SSID_LEN];
    if (s_ap_configured && s_ap_ssid[0]) {
        snprintf(ssid, sizeof(ssid), "%s", s_ap_ssid);
    } else {
        hyperwisor_state_t *st = hyperwisor_get_state();
        size_t did_len = strlen(st->device_id);
        const char *suffix = did_len > 6 ? st->device_id + did_len - 6 : st->device_id;
        snprintf(ssid, sizeof(ssid), "%s%.6s", HYPERWISOR_AP_SSID_PREFIX, suffix);
    }
    hyperwisor_wifi_start_ap(ssid, HYPERWISOR_AP_PASS);
    hyperwisor_wifi_start_provisioning_server();
    hyperwisor_get_state()->ap_mode_active = true;
}

esp_err_t hyperwisor_wifi_stop_ap(void)
{
    /* Used to call esp_wifi_stop(), which takes the station down with it --
     * so "close the setup AP" also dropped the device off the network it had
     * just joined. Drop the AP out of the mode instead. */
    s_want_ap = false;
    hyperwisor_get_state()->ap_mode_active = false;
    return apply_mode();
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

hyperwisor_prov_state_t hyperwisor_wifi_prov_state(void)
{
    return s_prov_state;
}

uint8_t hyperwisor_wifi_prov_reason(void)
{
    return s_prov_reason;
}

/* ==================== Provisioning HTTP Server ==================== */

static const char *prov_state_name(hyperwisor_prov_state_t s)
{
    switch (s) {
    case HYPERWISOR_PROV_TRYING:        return "trying";
    case HYPERWISOR_PROV_CONNECTED:     return "connected";
    case HYPERWISOR_PROV_AUTH_FAILED:   return "auth_failed";
    case HYPERWISOR_PROV_AP_NOT_FOUND:  return "ap_not_found";
    case HYPERWISOR_PROV_TIMEOUT:       return "timeout";
    default:                            return "idle";
    }
}

/* The setup page used to claim success the instant it had the credentials,
 * then reboot -- so "Configuration Sent!" was the last thing anyone ever heard
 * and a wrong password was indistinguishable from a working one. It now waits
 * on /api/status and reports what actually happened, because the phone is
 * still on this AP and is the only audience that will ever be able to act. */
static const char *PROV_WAIT_HTML =
    "<!DOCTYPE html>"
    "<html><head><title>Setting up</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
    "display:flex;flex-direction:column;justify-content:center;align-items:center;min-height:100vh;"
    "margin:0;background:#f8fafc;text-align:center;padding:20px;color:#0f172a}"
    ".card{background:#fff;padding:32px 28px;border-radius:24px;"
    "box-shadow:0 2px 12px rgba(15,23,42,.08);max-width:380px;width:100%%}"
    "h1{font-size:21px;margin:0 0 8px}"
    "p{color:#64748b;font-size:15px;line-height:1.55;margin:0 0 24px}"
    ".spin{width:34px;height:34px;margin:0 auto 18px;border-radius:50%%;"
    "border:3px solid #e2e8f0;border-top-color:#0f172a;animation:s .8s linear infinite}"
    "@keyframes s{to{transform:rotate(1turn)}}"
    "a{display:inline-block;background:#0f172a;color:#fff;padding:14px 28px;text-decoration:none;"
    "border-radius:999px;font-weight:600;font-size:16px}"
    "a.warn{background:#b45309}"
    "</style></head><body>"
    "<div class=\"card\" id=\"c\">"
    "<div class=\"spin\" id=\"sp\"></div>"
    "<h1 id=\"t\">Joining %s</h1>"
    "<p id=\"m\">Checking that the network accepts it. This takes a few seconds.</p>"
    "<div id=\"a\"></div></div>"
    "<script>"
    "var pk='%s';"
    "function link(s,l,c){return '<a class=\"'+c+'\" href=\"hypervisorv4://provisioning?status='+s+pk+'\">'+l+'</a>'}"
    "function paint(st,re){"
    "var t=document.getElementById('t'),m=document.getElementById('m'),"
    "a=document.getElementById('a'),sp=document.getElementById('sp');"
    "if(st=='trying')return;"
    "sp.style.display='none';"
    "if(st=='connected'){t.textContent='Connected';"
    "m.textContent='The device joined the network and is reaching Hyperwisor now.';"
    "a.innerHTML=link('success','Return to app','')}"
    "else if(st=='auth_failed'){t.textContent='Wrong Wi-Fi password';"
    "m.textContent='The network refused the password. The setup network is still open, so you can try again.';"
    "a.innerHTML=link('error&message=Wrong_Wi-Fi_password','Try again','warn')}"
    "else if(st=='ap_not_found'){t.textContent='Network not found';"
    "m.textContent='That name is not on the air here. Check the spelling \\u2014 and note this device cannot see 5 GHz networks.';"
    "a.innerHTML=link('error&message=Network_not_found','Try again','warn')}"
    "else{t.textContent='No answer from the network';"
    "m.textContent='It did not get an address in time. The setup network is still open, so you can try again.';"
    "a.innerHTML=link('error&message=No_answer_from_the_network','Try again','warn')}}"
    "function poll(){fetch('/api/status').then(function(r){return r.json()}).then(function(j){"
    "paint(j.state,j.reason);if(j.state=='trying')setTimeout(poll,1000)})"
    ".catch(function(){setTimeout(poll,1500)})}"
    "poll();"
    "</script></body></html>";

static const char *PROV_ERROR_HTML =
    "<!DOCTYPE html>"
    "<html><head><title>Provisioning Error</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
    "display:flex;flex-direction:column;justify-content:center;align-items:center;min-height:100vh;"
    "margin:0;background:#f8fafc;text-align:center;padding:20px;color:#0f172a}"
    ".card{background:#fff;padding:32px 28px;border-radius:24px;"
    "box-shadow:0 2px 12px rgba(15,23,42,.08);max-width:380px}"
    "h1{font-size:21px;margin:0 0 8px}"
    "p{color:#64748b;font-size:15px;margin:0 0 24px}"
    "a{display:inline-block;background:#64748b;color:#fff;padding:14px 28px;text-decoration:none;"
    "border-radius:999px;font-weight:600;font-size:16px}"
    "</style></head><body>"
    "<div class=\"card\"><h1>No network name</h1>"
    "<p>The setup request arrived without a Wi-Fi name, so there was nothing to join.</p>"
    "<a href=\"hypervisorv4://provisioning?status=error&message=Missing_SSID\">"
    "Return to app</a></div></body></html>";

/* Percent-encode the base64 chars that are unsafe in a URL query value. */
static void url_encode_b64(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < out_sz; i++) {
        char c = in[i];
        if (c == '+')      { out[o++] = '%'; out[o++] = '2'; out[o++] = 'B'; }
        else if (c == '/') { out[o++] = '%'; out[o++] = '2'; out[o++] = 'F'; }
        else if (c == '=') { out[o++] = '%'; out[o++] = '3'; out[o++] = 'D'; }
        else out[o++] = c;
    }
    out[o] = '\0';
}

static esp_err_t status_handler(httpd_req_t *req)
{
    hyperwisor_state_t *st = hyperwisor_get_state();
    char ip[16] = {0};
    if (s_sta_connected) hyperwisor_wifi_get_ip(ip);

    char body[320];
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"reason\":%u,\"ssid\":\"%s\",\"ip\":\"%s\","
             "\"device_id\":\"%s\",\"secure\":%s}",
             prov_state_name(s_prov_state), (unsigned)s_prov_reason,
             s_trial_ssid[0] ? s_trial_ssid : st->ssid,
             ip, st->device_id,
             hyperwisor_hsc_ready() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* The app fetches this from a page it did not serve. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Provisioning is a trial, not a commit.
 *
 * The old handler saved the credentials, answered "Configuration Sent!", waited
 * two seconds and called esp_restart(). Three consequences, all bad:
 *
 *   - It had not tried the network when it claimed success, so a wrong password
 *     looked exactly like a good one to the app.
 *   - The restart killed this AP, and AP mode only comes back when NO
 *     credentials are stored -- so wrong credentials left the device retrying
 *     forever with nothing listening and no way in. Recovery meant a cable.
 *   - It overwrote credentials that were working.
 *
 * So: bring the station up alongside the AP, keep serving this page, and report
 * the real outcome. The phone asking is already associated to this AP, which
 * makes it the only party that can both hear the verdict and act on it.
 *
 * One behaviour to know: on success the AP follows the station onto the
 * router's channel, which briefly drops the phone. A caller that loses the AP
 * right after asking should read that as a probable join and confirm over the
 * HSC rather than as a failure. */
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

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PROV_ERROR_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Identity is safe to persist immediately -- it is who the device is, not
     * how it gets online, and a failed join should not forget it. */
    if (device_id[0]) hyperwisor_set_device_id(device_id);
    if (user_id[0])   hyperwisor_set_user_id(user_id);

    /* HSC Phase 2: hand the device's public key back to the app in the
     * provisioning ack (deep link) so the app can register it. Proximity to
     * the device AP is the ownership proof. Only when security is enabled. */
    char suffix[300] = {0};
    char pub[128];
    if (hyperwisor_hsc_ready() &&
        hyperwisor_hsc_get_public_key_b64(pub, sizeof(pub)) == ESP_OK) {
        char enc[180];
        url_encode_b64(pub, enc, sizeof(enc));
        snprintf(suffix, sizeof(suffix), "&public_key=%s&device_id=%s", enc, device_id);
    }

    /* Remember what was working, then start the trial. */
    hyperwisor_state_t *st = hyperwisor_get_state();
    snprintf(s_prev_ssid, sizeof(s_prev_ssid), "%s", st->ssid);
    snprintf(s_prev_pass, sizeof(s_prev_pass), "%s", st->password);
    snprintf(s_trial_ssid, sizeof(s_trial_ssid), "%s", ssid);
    snprintf(s_trial_pass, sizeof(s_trial_pass), "%s", password);

    s_prov_reason = 0;
    s_prov_state  = HYPERWISOR_PROV_TRYING;
    s_trial_active = true;
    esp_timer_stop(s_trial_timer);
    esp_timer_start_once(s_trial_timer, PROV_TRIAL_TIMEOUT_MS * 1000ULL);

    /* Answer before touching the radio: the response has to reach the phone
     * over this AP, and bringing the station up can disturb it. */
    static char page[3072];
    snprintf(page, sizeof(page), PROV_WAIT_HTML, ssid, suffix);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "trial join: ssid=%s device=%s user=%s", ssid, device_id, user_id);
    sta_configure_and_connect(ssid, password);
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

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_prov_server, &status_uri);

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
