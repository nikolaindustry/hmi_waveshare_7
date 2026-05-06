/**
 * @file hyperwisor_cmd.h
 * @brief JSON command parser and router (S3 port: no GPIO/OTA built-ins)
 */

#ifndef HYPERWISOR_CMD_H
#define HYPERWISOR_CMD_H

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_HYPERWISOR_CMD_MAX_HANDLERS
#define HYPERWISOR_CMD_MAX_HANDLERS CONFIG_HYPERWISOR_CMD_MAX_HANDLERS
#else
#define HYPERWISOR_CMD_MAX_HANDLERS 16
#endif

esp_err_t hyperwisor_cmd_process(const char *json_str);
cJSON *hyperwisor_cmd_build_response(const char *from, cJSON *payload);
esp_err_t hyperwisor_cmd_register(const char *command,
                                   void (*handler)(const char *from, cJSON *payload));

/* Canonical outbound emitter. Builds and sends over the websocket:
 *   { "targetId": <target_id>,
 *     "payload": { "commands": [ { "command": <command>,
 *                                   "actions":  [ { "action": <action>,
 *                                                    "params": <params> } ] } ] } }
 * Takes ownership of `params` (may be NULL for an empty params object). */
esp_err_t hyperwisor_emit(const char *target_id,
                           const char *command,
                           const char *action,
                           cJSON *params);

/* Built-in SYSTEM handler (restart, uptime). Application may register this
 * explicitly via hyperwisor_register_cmd_handler("SYSTEM", hyperwisor_cmd_handle_system). */
void hyperwisor_cmd_handle_system(const char *from, cJSON *payload);

/* Built-in DEVICE_STATUS handler. Mirrors the Arduino hyperwisor-iot library:
 *   realtime.sendTo(from, { status: "online", response: "device_status" });
 * Produces the flat `{"targetId":from,"payload":{status,response}}` shape the
 * Android/dashboard clients expect (NOT the commands[]/actions[] wrapped one). */
void hyperwisor_cmd_handle_device_status(const char *from, cJSON *payload);

/* JSON utility helpers for Arduino-style payload.commands[] / actions[] / params traversal */
cJSON *hyperwisor_cmd_find_command(cJSON *payload, const char *command_name);
cJSON *hyperwisor_cmd_find_action(cJSON *payload, const char *command_name, const char *action_name);
cJSON *hyperwisor_cmd_find_params(cJSON *payload, const char *command_name, const char *action_name);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_CMD_H */
