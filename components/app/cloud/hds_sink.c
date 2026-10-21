#include "hds_sink.h"
#include "hyperwisor_app.h"
#include "hyperwisor.h"
#include "hyperwisor_cmd.h"
#include "hyperwisor_ws.h"
#include "modbus_task.h"
#include "hvac_config.h"
#include "ctrl_state.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hds_sink";

#define MAX_USER_RELAYS   8     /* coils 0..7 user-controllable; 8..15 HVAC */
#define RELAY_BOARD_COILS 16

static uint32_t s_local, s_remote, s_dropped;

/* ---------------------------------------------------------------------
 *  Vocabulary bridge
 *
 *  The dashboard builder and the firmware's own cloud handlers grew up
 *  apart, and they disagree in three places. Rather than teach the
 *  renderer about relays, the differences are absorbed here:
 *
 *    command   builder says "RELAY_CONTROL", handlers register
 *              "relay_control" -- and hyperwisor_cmd.c dispatches with
 *              strcmp, so the case has to be folded somewhere.
 *    coil id   builder says {"channel":0}; the handlers read
 *              {"coil":0} or {"relay":0}.
 *    state     builder puts it in the action name ("TURN_ON"), the
 *              handlers read {"state":true} or {"on":true}.
 * ------------------------------------------------------------------- */

static int coil_from_params(const cJSON *params)
{
    static const char *keys[] = { "channel", "coil", "relay" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *v = cJSON_GetObjectItem(params, keys[i]);
        if (cJSON_IsNumber(v)) {
            return (int)v->valuedouble;
        }
        if (cJSON_IsString(v) && v->valuestring) {
            return atoi(v->valuestring);
        }
    }
    return -1;
}

static bool state_from(const char *action, const cJSON *params, bool *ok)
{
    *ok = true;
    if (action) {
        if (!strcasecmp(action, "TURN_ON")  || !strcasecmp(action, "ON")) {
            return true;
        }
        if (!strcasecmp(action, "TURN_OFF") || !strcasecmp(action, "OFF")) {
            return false;
        }
    }

    const cJSON *v = cJSON_GetObjectItem(params, "state");
    if (!v) {
        v = cJSON_GetObjectItem(params, "on");
    }
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    if (cJSON_IsString(v) && v->valuestring) {
        const char *s = v->valuestring;
        if (!strcasecmp(s, "true")  || !strcasecmp(s, "on")  || !strcmp(s, "1")) return true;
        if (!strcasecmp(s, "false") || !strcasecmp(s, "off") || !strcmp(s, "0")) return false;
    }

    /* "TOGGLE" with no state would mean reading the current coil back;
     * the worker owns that, so say we could not resolve it rather than
     * guessing a direction. */
    *ok = false;
    return false;
}

static bool is_hvac_owned_coil(uint16_t coil)
{
    return coil == HVAC_FAN_COIL_LOW
        || coil == HVAC_FAN_COIL_MED
        || coil == HVAC_FAN_COIL_HIGH
        || coil == HVAC_FAN_COIL_MAX
        || coil == HVAC_CLUTCH_COIL
        || coil == HVAC_IDLEUP_COIL
        || coil == HVAC_RECIRC_COIL
        || coil == HVAC_HEATER_COIL;
}

/* ---------------------------------------------------------------------
 *  Local resolution
 * ------------------------------------------------------------------- */

/* Returns true when the command was fully satisfied on this board. */
static bool try_local(const char *cmd, const char *action, const cJSON *params)
{
    if (!cmd || !cJSON_IsObject(params)) {
        return false;
    }
    bool is_relay = !strcasecmp(cmd, "RELAY_CONTROL");
    bool is_coil  = !strcasecmp(cmd, "COIL_CONTROL");
    if (!is_relay && !is_coil) {
        return false;
    }

    int coil = coil_from_params(params);
    if (coil < 0 || coil >= RELAY_BOARD_COILS) {
        ESP_LOGW(TAG, "%s: no usable coil id in params", cmd);
        return false;
    }

    bool ok = false;
    bool on = state_from(action, params, &ok);
    if (!ok) {
        ESP_LOGW(TAG, "%s: cannot resolve state from action '%s'",
                 cmd, action ? action : "(none)");
        return false;
    }

    /* RELAY_CONTROL is the narrow, user-facing command: coils 0..7 only.
     * COIL_CONTROL may address the whole board. Neither may stamp on a
     * coil the climate controller is driving -- it would reassert within
     * a tick anyway, and the relay would chatter. */
    if (is_relay && coil >= MAX_USER_RELAYS) {
        ESP_LOGW(TAG, "relay_control: coil %d out of user range", coil);
        return false;
    }
    if (is_hvac_owned_coil((uint16_t)coil)) {
        ESP_LOGW(TAG, "%s: coil %d is HVAC-owned, refusing", cmd, coil);
        return false;
    }

    ESP_LOGI(TAG, "local: coil=%d on=%d", coil, on);
    modbus_task_request((uint16_t)coil, on);

    if (coil == CTRL_STAR_COIL) {
        ctrl_star_t *st = ctrl_star_roof();
        if (st) {
            st->on = on;
        }
    }

    /* Tell the cloud what the panel just did, so a phone looking at the
     * same dashboard follows along. Quiet no-op when offline. */
    if (coil < MAX_USER_RELAYS) {
        hyperwisor_app_push_relay_state(coil, on);
    }
    return true;
}

/* ---------------------------------------------------------------------
 *  Sink
 * ------------------------------------------------------------------- */

void hds_sink_emit(const char *target, const cJSON *payload, void *user)
{
    (void)user;

    const char *cmd    = hds_payload_command(payload);
    const char *action = hds_payload_action(payload);
    cJSON      *params = hds_payload_params(payload);

    if (!cmd) {
        /* Some other payload shape -- nothing to route locally, so send
         * it on untouched rather than guessing at it. */
        ESP_LOGD(TAG, "payload carries no commands[]; forwarding as-is");
    } else if (try_local(cmd, action, params)) {
        s_local++;
        return;
    }

    /* Anything this board cannot satisfy -- schedules, setup, a target
     * that is not us -- goes out over the websocket. hyperwisor_emit
     * takes ownership of the params it is given, and ours is borrowed. */
    if (!hyperwisor_ws_is_connected()) {
        ESP_LOGW(TAG, "offline: dropping %s/%s", cmd, action ? action : "-");
        s_dropped++;
        return;
    }

    /* Send the payload exactly as the web dashboard would -- the whole
     * authored object, placeholders filled, not just the first command.
     * hyperwisor_ws_send_json takes the {targetId, payload} envelope. */
    cJSON *env = cJSON_CreateObject();
    if (!env) {
        s_dropped++;
        return;
    }
    if (target) {
        cJSON_AddStringToObject(env, "targetId", target);
    }
    cJSON_AddItemToObject(env, "payload",
                          cJSON_Duplicate((cJSON *)payload, true));

    /* ws_send_json serialises but does not take ownership. */
    esp_err_t err = hyperwisor_ws_send_json(env);
    cJSON_Delete(env);

    if (err == ESP_OK) {
        s_remote++;
        ESP_LOGI(TAG, "remote: %s/%s -> %s", cmd ? cmd : "?",
                 action ? action : "-", target ? target : "(default)");
    } else {
        s_dropped++;
        ESP_LOGW(TAG, "ws send failed (%s) for %s",
                 esp_err_to_name(err), cmd ? cmd : "?");
    }
}

void hds_sink_stats(uint32_t *local, uint32_t *remote, uint32_t *dropped)
{
    if (local)   *local   = s_local;
    if (remote)  *remote  = s_remote;
    if (dropped) *dropped = s_dropped;
}
