/**
 * @file hyperwisor_widget.h
 * @brief Widget update APIs ported from Arduino HyperwisorIOT library
 */

#ifndef HYPERWISOR_WIDGET_H
#define HYPERWISOR_WIDGET_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;
    int y;
    int value;
} hyperwisor_heat_map_point_t;

typedef struct {
    char model_id[32];
    float position[3];
    float rotation[3];
    float scale[3];
    char color[16];
    float metalness;
    float roughness;
    float opacity;
    bool wireframe;
    bool visible;
} hyperwisor_3d_model_update_t;

esp_err_t hyperwisor_update_widget_string(const char *target_id, const char *widget_id, const char *value);
esp_err_t hyperwisor_update_widget_float(const char *target_id, const char *widget_id, float value);
esp_err_t hyperwisor_update_widget_float_array(const char *target_id, const char *widget_id, const float *values, int count);
esp_err_t hyperwisor_update_widget_int_array(const char *target_id, const char *widget_id, const int *values, int count);
esp_err_t hyperwisor_update_widget_string_array(const char *target_id, const char *widget_id, const char **values, int count);

esp_err_t hyperwisor_show_dialog(const char *target_id, const char *title, const char *description, const char *icon);
esp_err_t hyperwisor_info_dialog(const char *target_id, const char *title, const char *description);
esp_err_t hyperwisor_warning_dialog(const char *target_id, const char *title, const char *description);
esp_err_t hyperwisor_success_dialog(const char *target_id, const char *title, const char *description);
esp_err_t hyperwisor_error_dialog(const char *target_id, const char *title, const char *description);
esp_err_t hyperwisor_risk_dialog(const char *target_id, const char *title, const char *description);

esp_err_t hyperwisor_update_flight_attitude(const char *target_id, const char *widget_id, float roll, float pitch);
esp_err_t hyperwisor_update_widget_position(const char *target_id, const char *widget_id, int x, int y, int w, int h, int r);
esp_err_t hyperwisor_update_countdown(const char *target_id, const char *widget_id, const char *hours, const char *minutes, const char *seconds);
esp_err_t hyperwisor_update_heat_map(const char *target_id, const char *widget_id, const hyperwisor_heat_map_point_t *points, int count);
esp_err_t hyperwisor_update_3d_model(const char *target_id, const char *widget_id, const char *model_url);
esp_err_t hyperwisor_update_3d_widget(const char *target_id, const char *widget_id, const hyperwisor_3d_model_update_t *models, int count);

esp_err_t hyperwisor_send_sensor_data_logger(const char *target_id, const char *config_id,
                                               const char **names, const float *values, int count);

esp_err_t hyperwisor_send_device_status(const char *target_id);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_WIDGET_H */
