/*
 * hello_hyperwisor — minimal example showing how to bring the
 * Hyperwisor IoT client online with three function calls.
 *
 * Behaviour:
 *   1. On first boot (no NVS credentials) the device starts SoftAP
 *      provisioning mode "NIKOLAINDUSTRY_Setup-XXXX" / "0123456789".
 *   2. Once provisioned, it joins WiFi, syncs NTP and connects to the
 *      Hyperwisor cloud via WebSocket.
 *   3. A custom "BLINK" command is registered so the dashboard can
 *      toggle GPIO2.
 */

#include "hyperwisor.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define BLINK_GPIO   GPIO_NUM_2
static const char *TAG = "HELLO_HYPER";

static void cmd_blink(const char *from, cJSON *payload)
{
    int level = cJSON_IsTrue(cJSON_GetObjectItem(payload, "on")) ? 1 : 0;
    gpio_set_level(BLINK_GPIO, level);

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "status", "ok");
    cJSON_AddNumberToObject(reply, "level",  level);
    hyperwisor_send_response(from, reply);
    cJSON_Delete(reply);
}

void app_main(void)
{
    /* Configure the demo GPIO before bringing the network up. */
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    /* 1. init the library (loads NVS, prepares state) */
    hyperwisor_init();

    /* 2. provision once at first boot — values persist in NVS */
    if (!hyperwisor_has_credentials()) {
        ESP_LOGW(TAG, "No credentials, edit main.c or use SoftAP setup");
        /* hyperwisor_set_credentials("MySSID", "MyPass",
         *                            "device-id", "user-id"); */
    }

    /* 3. register custom commands BEFORE start() */
    hyperwisor_register_cmd_handler("BLINK", cmd_blink);

    /* 4. spawn the core task */
    hyperwisor_start();
}
