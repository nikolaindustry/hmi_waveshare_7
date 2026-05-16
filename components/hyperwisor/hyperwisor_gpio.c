/**
 * @file hyperwisor_gpio.c
 * @brief GPIO management from the Hyperwisor cloud dashboard
 *
 * Ported from the ESP32-P4 reference, enhanced with:
 *   - Pin allowlist for safety (cloud can only touch listed pins)
 *   - NVS state persistence (pins restored after reboot, matching Arduino)
 *   - Actions: ON, OFF, READ (Arduino only does ON/OFF; we add READ)
 */

#include "hyperwisor_gpio.h"

#if CONFIG_HYPERWISOR_ENABLE_GPIO

#include "hyperwisor_core.h"
#include "hyperwisor_cmd.h"
#include "hyperwisor_ws.h"
#include "hyperwisor_nvs.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "HYPER_GPIO";

/* ---- Pin allowlist ---- */
static const int *s_allowed_pins = NULL;
static int s_allowed_pin_count = 0;

void hyperwisor_gpio_set_allowlist(const int *gpios, int count)
{
    s_allowed_pins = gpios;
    s_allowed_pin_count = count;
    if (gpios && count > 0) {
        ESP_LOGI(TAG, "GPIO allowlist set: %d pin(s)", count);
    } else {
        ESP_LOGI(TAG, "GPIO allowlist empty — cloud GPIO access denied");
    }
}

static bool pin_is_allowed(int pin)
{
    if (!s_allowed_pins || s_allowed_pin_count <= 0) {
        return false;
    }
    for (int i = 0; i < s_allowed_pin_count; i++) {
        if (s_allowed_pins[i] == pin) {
            return true;
        }
    }
    return false;
}

/* ---- Low-level helpers ---- */

esp_err_t hyperwisor_gpio_set_pin_mode(int pin, const char *mode)
{
    gpio_mode_t gpio_mode = GPIO_MODE_DISABLE;

    if (strcmp(mode, "OUTPUT") == 0) {
        gpio_mode = GPIO_MODE_OUTPUT;
    } else if (strcmp(mode, "INPUT") == 0) {
        gpio_mode = GPIO_MODE_INPUT;
    } else if (strcmp(mode, "INPUT_PULLUP") == 0) {
        gpio_mode = GPIO_MODE_INPUT;
    } else if (strcmp(mode, "INPUT_PULLDOWN") == 0) {
        gpio_mode = GPIO_MODE_INPUT;
    } else {
        ESP_LOGW(TAG, "Unknown pin mode: %s", mode);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = gpio_mode,
        .pull_up_en = (strcmp(mode, "INPUT_PULLUP") == 0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (strcmp(mode, "INPUT_PULLDOWN") == 0) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&io_conf);
}

esp_err_t hyperwisor_gpio_digital_write(int pin, const char *level)
{
    int lvl = 0;
    if (strcmp(level, "HIGH") == 0 || strcmp(level, "1") == 0) {
        lvl = 1;
    } else if (strcmp(level, "LOW") == 0 || strcmp(level, "0") == 0) {
        lvl = 0;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return gpio_set_level(pin, lvl);
}

bool hyperwisor_gpio_digital_read(int pin)
{
    return gpio_get_level(pin) != 0;
}

esp_err_t hyperwisor_gpio_save_state(int pin, int state)
{
    char key[16];
    snprintf(key, sizeof(key), "pin_%d", pin);
    esp_err_t err = hyperwisor_nvs_set_i32(key, state);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved GPIO %d state: %d", pin, state);
    }
    return err;
}

int hyperwisor_gpio_load_state(int pin)
{
    char key[16];
    snprintf(key, sizeof(key), "pin_%d", pin);
    int32_t state = 0;
    esp_err_t err = hyperwisor_nvs_get_i32(key, &state);
    if (err != ESP_OK) {
        return -1;
    }
    return (int)state;
}

void hyperwisor_gpio_restore_all_states(void)
{
    if (!s_allowed_pins || s_allowed_pin_count <= 0) {
        return;
    }
    for (int i = 0; i < s_allowed_pin_count; i++) {
        int pin = (int)s_allowed_pins[i];
        int state = hyperwisor_gpio_load_state(pin);
        if (state >= 0) {
            gpio_config_t io_conf = {
                .pin_bit_mask = (1ULL << pin),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&io_conf);
            gpio_set_level(pin, state);
            ESP_LOGI(TAG, "Restored pin %d to state %d", pin, state);
        }
    }
}

/* ---- Command handler ---- */

/**
 * Handle the GPIO_MANAGEMENT command from the dashboard.
 *
 * Arduino format (hyperwisor-iot.cpp):
 *   command: "GPIO_MANAGEMENT"
 *   actions: [{"action":"ON"/"OFF", "params":{"gpio":4,"pinmode":"OUTPUT","status":"HIGH"}}]
 *
 * We also support a "READ" action that returns the current pin level.
 */
static void handle_gpio_management(const char *from, cJSON *payload)
{
    cJSON *actions = cJSON_GetObjectItem(payload, "actions");
    if (!cJSON_IsArray(actions)) {
        return;
    }

    int action_count = cJSON_GetArraySize(actions);
    for (int i = 0; i < action_count; i++) {
        cJSON *act = cJSON_GetArrayItem(actions, i);
        cJSON *action_str = cJSON_GetObjectItem(act, "action");
        cJSON *params = cJSON_GetObjectItem(act, "params");

        if (!cJSON_IsString(action_str) || !cJSON_IsObject(params)) {
            continue;
        }

        int gpio_num = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(params, "gpio"));
        const char *pinmode = cJSON_GetStringValue(cJSON_GetObjectItem(params, "pinmode"));
        const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(params, "status"));

        /* Safety: reject pins not in the allowlist */
        if (!pin_is_allowed(gpio_num)) {
            ESP_LOGW(TAG, "GPIO %d not in allowlist — rejected", gpio_num);
            continue;
        }

        const char *action_name = action_str->valuestring;

        /* Set pin mode first (if provided) */
        if (pinmode) {
            hyperwisor_gpio_set_pin_mode(gpio_num, pinmode);
        }

        /* Handle action */
        if (strcmp(action_name, "ON") == 0 || strcmp(action_name, "OFF") == 0) {
            /* Write the pin level */
            if (status) {
                hyperwisor_gpio_digital_write(gpio_num, status);
                hyperwisor_gpio_save_state(gpio_num, strcmp(status, "HIGH") == 0 || strcmp(status, "1") == 0 ? 1 : 0);
            }
            ESP_LOGI(TAG, "Pin %d -> %s (mode: %s)", gpio_num,
                     status ? status : "N/A", pinmode ? pinmode : "N/A");
        } else if (strcmp(action_name, "READ") == 0) {
            /* Read and report the current pin level */
            bool level = hyperwisor_gpio_digital_read(gpio_num);
            cJSON *rp = cJSON_CreateObject();
            cJSON_AddNumberToObject(rp, "gpio", gpio_num);
            cJSON_AddStringToObject(rp, "level", level ? "HIGH" : "LOW");
            hyperwisor_emit(from, "GPIO_MANAGEMENT", "response", rp);
            ESP_LOGI(TAG, "Read GPIO %d -> %s", gpio_num, level ? "HIGH" : "LOW");
        }
    }
}

/* ---- Auto-registration ---- */

void hyperwisor_gpio_auto_register(void)
{
    hyperwisor_register_cmd_handler("GPIO_MANAGEMENT", handle_gpio_management);
    ESP_LOGI(TAG, "GPIO_MANAGEMENT command handler registered");
}

#endif /* CONFIG_HYPERWISOR_ENABLE_GPIO */
