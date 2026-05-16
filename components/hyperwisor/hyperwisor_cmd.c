/**
 * @file hyperwisor_cmd.c
 * @brief JSON command parser and router with built-in SYSTEM/DEVICE_STATUS handlers
 */

#include "hyperwisor_cmd.h"
#include "hyperwisor_core.h"
#include "hyperwisor_ws.h"
#include "hyperwisor_port.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HYPER_CMD";

static struct {
    const char *command;
    void (*handler)(const char *from, cJSON *payload);
} s_cmd_handlers[HYPERWISOR_CMD_MAX_HANDLERS];

static int s_cmd_count = 0;

esp_err_t hyperwisor_cmd_process(const char *json_str)
{
    if (!json_str || *json_str == '\0') {
        ESP_LOGE(TAG, "Empty command received");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    cJSON *from = cJSON_GetObjectItem(root, "from");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "Command missing 'payload' object");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *commands = cJSON_GetObjectItem(payload, "commands");
    if (!cJSON_IsArray(commands)) {
        ESP_LOGW(TAG, "Command payload missing 'commands' array");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *from_str = cJSON_IsString(from) ? from->valuestring : "unknown";

    int cmd_array_size = cJSON_GetArraySize(commands);
    for (int i = 0; i < cmd_array_size; i++) {
        cJSON *cmd_obj = cJSON_GetArrayItem(commands, i);
        cJSON *cmd_name = cJSON_GetObjectItem(cmd_obj, "command");

        if (!cJSON_IsString(cmd_name)) {
            continue;
        }

        bool handled = false;
        for (int j = 0; j < s_cmd_count; j++) {
            if (strcmp(s_cmd_handlers[j].command, cmd_name->valuestring) == 0) {
                s_cmd_handlers[j].handler(from_str, cmd_obj);
                handled = true;
                break;
            }
        }

        if (!handled) {
            ESP_LOGW(TAG, "Unknown command: %s", cmd_name->valuestring);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

cJSON *hyperwisor_cmd_build_response(const char *from, cJSON *payload)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "from", hyperwisor_get_state()->device_id);
    cJSON_AddStringToObject(root, "sendTo", from);
    cJSON_AddItemToObject(root, "payload", payload);
    return root;
}

esp_err_t hyperwisor_emit(const char *target_id,
                           const char *command,
                           const char *action,
                           cJSON *params)
{
    if (!target_id || !command || !action) {
        if (params) cJSON_Delete(params);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "targetId", target_id);

    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON *commands = cJSON_AddArrayToObject(payload, "commands");

    cJSON *cmd_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd_obj, "command", command);
    cJSON *actions = cJSON_AddArrayToObject(cmd_obj, "actions");

    cJSON *act_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(act_obj, "action", action);
    cJSON_AddItemToObject(act_obj, "params", params ? params : cJSON_CreateObject());

    cJSON_AddItemToArray(actions, act_obj);
    cJSON_AddItemToArray(commands, cmd_obj);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_FAIL;

    esp_err_t err = hyperwisor_ws_send(json_str, 0);
    free(json_str);
    return err;
}

esp_err_t hyperwisor_cmd_register(const char *command,
                                   void (*handler)(const char *from, cJSON *payload))
{
    if (s_cmd_count >= HYPERWISOR_CMD_MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    s_cmd_handlers[s_cmd_count].command = command;
    s_cmd_handlers[s_cmd_count].handler = handler;
    s_cmd_count++;
    ESP_LOGI(TAG, "Registered handler for: %s", command);
    return ESP_OK;
}

/* ==================== Built-in SYSTEM handler ==================== */

void hyperwisor_cmd_handle_system(const char *from, cJSON *payload)
{
    if (!from || !payload) {
        ESP_LOGW(TAG, "SYSTEM handler: null from or payload");
        return;
    }

    cJSON *params = cJSON_GetObjectItem(payload, "params");
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(params, "action"));

    if (!action) {
        ESP_LOGW(TAG, "SYSTEM handler: missing action in params");
        return;
    }

    if (strcmp(action, "restart") == 0) {
        ESP_LOGI(TAG, "System restart requested");
        cJSON *rp = cJSON_CreateObject();
        cJSON_AddStringToObject(rp, "status", "restarting");
        hyperwisor_emit(from, "SYSTEM", "response", rp);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else if (strcmp(action, "status") == 0) {
        hyperwisor_state_t *st = hyperwisor_get_state();
        cJSON *rp = cJSON_CreateObject();
        cJSON_AddNumberToObject(rp, "uptime",    st->uptime_seconds);
        cJSON_AddBoolToObject  (rp, "wifi",      st->wifi_connected);
        cJSON_AddBoolToObject  (rp, "websocket", st->ws_connected);
        cJSON_AddStringToObject(rp, "version",   st->version);
        hyperwisor_emit(from, "SYSTEM", "response", rp);
    } else if (strcmp(action, "info") == 0) {
        /* Detailed device info: chip model, revision, free heap, IDF version */
        hyperwisor_state_t *st = hyperwisor_get_state();
        esp_chip_info_t chip_info;
        esp_chip_info(&chip_info);

        cJSON *rp = cJSON_CreateObject();
        cJSON_AddStringToObject(rp, "chip",       CONFIG_IDF_TARGET);
        cJSON_AddNumberToObject(rp, "cores",      chip_info.cores);
        cJSON_AddNumberToObject(rp, "revision",   chip_info.revision);
        cJSON_AddNumberToObject(rp, "free_heap",  esp_get_free_heap_size());
        cJSON_AddNumberToObject(rp, "min_heap",   esp_get_minimum_free_heap_size());
        cJSON_AddStringToObject(rp, "idf_version", esp_get_idf_version());
        cJSON_AddStringToObject(rp, "app_version", st->version);
        cJSON_AddNumberToObject(rp, "uptime",      st->uptime_seconds);
        cJSON_AddBoolToObject  (rp, "wifi",        st->wifi_connected);
        cJSON_AddBoolToObject  (rp, "websocket",   st->ws_connected);

        /* Board info from port (if available) */
        const hyperwisor_port_t *port = hyperwisor_get_port();
        if (port) {
            if (port->board_name)   cJSON_AddStringToObject(rp, "board",   port->board_name);
            if (port->manufacturer) cJSON_AddStringToObject(rp, "maker",   port->manufacturer);
            cJSON_AddNumberToObject(rp, "managed_gpios", port->managed_gpio_count);
        }

        hyperwisor_emit(from, "SYSTEM", "response", rp);
    }
}

/* ==================== Built-in DEVICE_STATUS handler ==================== */

/* Arduino reference (hyperwisor-iot.cpp, DEVICE_STATUS branch) does:
 *   realtime.sendTo(this->newtarget, [](JsonObject &payload) {
 *     payload["status"]   = "online";
 *     payload["response"] = "device_status";
 *   });
 * which emits {"targetId":<from>,"payload":{"status":"online","response":"device_status"}}.
 * We deliberately use hyperwisor_send_to() (flat payload) here rather than
 * hyperwisor_emit() (which wraps into commands[]/actions[]/params) so the
 * dashboard/Android client sees the exact shape it expects. */
static void device_status_payload_builder(cJSON *payload)
{
    cJSON_AddStringToObject(payload, "status",   "online");
    cJSON_AddStringToObject(payload, "response", "device_status");
}

void hyperwisor_cmd_handle_device_status(const char *from, cJSON *payload)
{
    (void)payload; /* DEVICE_STATUS request carries no params */
    if (!from || !*from) {
        ESP_LOGW(TAG, "DEVICE_STATUS from empty sender; dropping response");
        return;
    }
    ESP_LOGI(TAG, "DEVICE_STATUS -> replying online to %s", from);
    esp_err_t err = hyperwisor_send_to(from, device_status_payload_builder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DEVICE_STATUS response send failed: %s", esp_err_to_name(err));
    }
}

/* ==================== JSON Utility Functions ==================== */

cJSON *hyperwisor_cmd_find_command(cJSON *payload, const char *command_name)
{
    if (!payload || !command_name) {
        return NULL;
    }

    cJSON *commands = cJSON_GetObjectItem(payload, "commands");
    if (!cJSON_IsArray(commands)) {
        return NULL;
    }

    int size = cJSON_GetArraySize(commands);
    for (int i = 0; i < size; i++) {
        cJSON *cmd_obj = cJSON_GetArrayItem(commands, i);
        cJSON *cmd_name = cJSON_GetObjectItem(cmd_obj, "command");
        if (cJSON_IsString(cmd_name) && strcmp(cmd_name->valuestring, command_name) == 0) {
            return cmd_obj;
        }
    }
    return NULL;
}

cJSON *hyperwisor_cmd_find_action(cJSON *payload, const char *command_name, const char *action_name)
{
    cJSON *command_obj = hyperwisor_cmd_find_command(payload, command_name);
    if (!command_obj) {
        return NULL;
    }

    cJSON *actions = cJSON_GetObjectItem(command_obj, "actions");
    if (!cJSON_IsArray(actions)) {
        return NULL;
    }

    int size = cJSON_GetArraySize(actions);
    for (int i = 0; i < size; i++) {
        cJSON *act_obj = cJSON_GetArrayItem(actions, i);
        cJSON *act_name = cJSON_GetObjectItem(act_obj, "action");
        if (cJSON_IsString(act_name) && strcmp(act_name->valuestring, action_name) == 0) {
            return act_obj;
        }
    }
    return NULL;
}

cJSON *hyperwisor_cmd_find_params(cJSON *payload, const char *command_name, const char *action_name)
{
    cJSON *action_obj = hyperwisor_cmd_find_action(payload, command_name, action_name);
    if (!action_obj) {
        return NULL;
    }

    cJSON *params = cJSON_GetObjectItem(action_obj, "params");
    if (cJSON_IsObject(params)) {
        return params;
    }
    return NULL;
}
