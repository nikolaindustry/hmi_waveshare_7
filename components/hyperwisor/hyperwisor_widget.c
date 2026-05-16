/**
 * @file hyperwisor_widget.c
 * @brief Widget update APIs ported from Arduino HyperwisorIOT library
 */

#include "hyperwisor_widget.h"
#include "hyperwisor_core.h"
#include "hyperwisor_ws.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG __attribute__((unused)) = "HYPER_WIDGET";

/* Helper: build envelope {targetId, payload: {...}} and send */
static esp_err_t send_payload(const char *target_id, cJSON *payload)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "targetId", target_id);
    cJSON_AddItemToObject(root, "payload", payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        return ESP_FAIL;
    }

    esp_err_t err = hyperwisor_ws_send(json_str, 0);
    free(json_str);
    return err;
}

/* ==================== Basic Widget Updates ==================== */

esp_err_t hyperwisor_update_widget_string(const char *target_id, const char *widget_id, const char *value)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON_AddStringToObject(payload, "value", value);
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_widget_float(const char *target_id, const char *widget_id, float value)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON_AddNumberToObject(payload, "value", value);
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_widget_float_array(const char *target_id, const char *widget_id, const float *values, int count)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON *arr = cJSON_AddArrayToObject(payload, "value");
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(values[i]));
    }
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_widget_int_array(const char *target_id, const char *widget_id, const int *values, int count)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON *arr = cJSON_AddArrayToObject(payload, "value");
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(values[i]));
    }
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_widget_string_array(const char *target_id, const char *widget_id, const char **values, int count)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON *arr = cJSON_AddArrayToObject(payload, "value");
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(values[i]));
    }
    return send_payload(target_id, payload);
}

/* ==================== Dialogs ==================== */

esp_err_t hyperwisor_show_dialog(const char *target_id, const char *title, const char *description, const char *icon)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "type", "dialog");
    cJSON_AddStringToObject(payload, "title", title);
    cJSON_AddStringToObject(payload, "description", description);
    cJSON_AddStringToObject(payload, "icon", icon);
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_info_dialog(const char *target_id, const char *title, const char *description)
{
    return hyperwisor_show_dialog(target_id, title, description, "info");
}

esp_err_t hyperwisor_warning_dialog(const char *target_id, const char *title, const char *description)
{
    return hyperwisor_show_dialog(target_id, title, description, "warning");
}

esp_err_t hyperwisor_success_dialog(const char *target_id, const char *title, const char *description)
{
    return hyperwisor_show_dialog(target_id, title, description, "success");
}

esp_err_t hyperwisor_error_dialog(const char *target_id, const char *title, const char *description)
{
    return hyperwisor_show_dialog(target_id, title, description, "error");
}

esp_err_t hyperwisor_risk_dialog(const char *target_id, const char *title, const char *description)
{
    return hyperwisor_show_dialog(target_id, title, description, "risk");
}

/* ==================== Specialized Widgets ==================== */

esp_err_t hyperwisor_update_flight_attitude(const char *target_id, const char *widget_id, float roll, float pitch)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON *value = cJSON_AddObjectToObject(payload, "value");
    cJSON_AddNumberToObject(value, "roll", roll);
    cJSON_AddNumberToObject(value, "pitch", pitch);
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_widget_position(const char *target_id, const char *widget_id, int x, int y, int w, int h, int r)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON_AddNumberToObject(payload, "x", x);
    cJSON_AddNumberToObject(payload, "y", y);
    cJSON_AddNumberToObject(payload, "w", w);
    cJSON_AddNumberToObject(payload, "h", h);
    cJSON_AddNumberToObject(payload, "r", r);
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_countdown(const char *target_id, const char *widget_id, const char *hours, const char *minutes, const char *seconds)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON_AddStringToObject(payload, "hr", hours);
    cJSON_AddStringToObject(payload, "min", minutes);
    cJSON_AddStringToObject(payload, "sec", seconds);
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_heat_map(const char *target_id, const char *widget_id, const hyperwisor_heat_map_point_t *points, int count)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON *value = cJSON_AddArrayToObject(payload, "value");
    for (int i = 0; i < count; i++) {
        cJSON *pt = cJSON_CreateObject();
        cJSON_AddNumberToObject(pt, "x", points[i].x);
        cJSON_AddNumberToObject(pt, "y", points[i].y);
        cJSON_AddNumberToObject(pt, "value", points[i].value);
        cJSON_AddItemToArray(value, pt);
    }
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_3d_model(const char *target_id, const char *widget_id, const char *model_url)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON_AddStringToObject(payload, "value", model_url);
    return send_payload(target_id, payload);
}

esp_err_t hyperwisor_update_3d_widget(const char *target_id, const char *widget_id, const hyperwisor_3d_model_update_t *models, int count)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "widgetId", widget_id);
    cJSON *value = cJSON_AddArrayToObject(payload, "value");
    for (int i = 0; i < count; i++) {
        cJSON *model_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(model_obj, "modelId", models[i].model_id);
        cJSON *updates = cJSON_AddObjectToObject(model_obj, "updates");
        cJSON *pos = cJSON_AddArrayToObject(updates, "position");
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(models[i].position[0]));
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(models[i].position[1]));
        cJSON_AddItemToArray(pos, cJSON_CreateNumber(models[i].position[2]));
        cJSON *rot = cJSON_AddArrayToObject(updates, "rotation");
        cJSON_AddItemToArray(rot, cJSON_CreateNumber(models[i].rotation[0]));
        cJSON_AddItemToArray(rot, cJSON_CreateNumber(models[i].rotation[1]));
        cJSON_AddItemToArray(rot, cJSON_CreateNumber(models[i].rotation[2]));
        cJSON *sca = cJSON_AddArrayToObject(updates, "scale");
        cJSON_AddItemToArray(sca, cJSON_CreateNumber(models[i].scale[0]));
        cJSON_AddItemToArray(sca, cJSON_CreateNumber(models[i].scale[1]));
        cJSON_AddItemToArray(sca, cJSON_CreateNumber(models[i].scale[2]));
        cJSON_AddStringToObject(updates, "color", models[i].color);
        cJSON_AddNumberToObject(updates, "metalness", models[i].metalness);
        cJSON_AddNumberToObject(updates, "roughness", models[i].roughness);
        cJSON_AddNumberToObject(updates, "opacity", models[i].opacity);
        cJSON_AddBoolToObject(updates, "wireframe", models[i].wireframe);
        cJSON_AddBoolToObject(updates, "visible", models[i].visible);
        cJSON_AddItemToArray(value, model_obj);
    }
    return send_payload(target_id, payload);
}

/* ==================== Sensor Data Logger ==================== */

esp_err_t hyperwisor_send_sensor_data_logger(const char *target_id, const char *config_id,
                                               const char **names, const float *values, int count)
{
    char device_id[HYPERWISOR_DEVICE_ID_LEN];
    hyperwisor_get_device_id(device_id, sizeof(device_id));

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "type", "sensorDataResponse");
    cJSON_AddStringToObject(payload, "configId", config_id);
    cJSON_AddStringToObject(payload, "deviceId", device_id);
    cJSON *data = cJSON_AddObjectToObject(payload, "data");
    for (int i = 0; i < count; i++) {
        cJSON_AddNumberToObject(data, names[i], values[i]);
    }
    return send_payload(target_id, payload);
}

/* ==================== Device Status ==================== */

esp_err_t hyperwisor_send_device_status(const char *target_id)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "status", "online");
    cJSON_AddStringToObject(payload, "response", "device_status");
    return send_payload(target_id, payload);
}
