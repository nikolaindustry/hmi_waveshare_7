/**
 * @file hyperwisor_port.c
 * @brief Board port storage and accessor
 */

#include "hyperwisor_port.h"
#include "esp_log.h"

static const char *TAG = "HYPER_PORT";

static const hyperwisor_port_t *s_port = NULL;

void hyperwisor_set_port(const hyperwisor_port_t *port)
{
    s_port = port;
    if (port) {
        ESP_LOGI(TAG, "Board port: %s (%s), %d managed GPIOs, display=%s",
                 port->board_name   ? port->board_name   : "(unnamed)",
                 port->manufacturer ? port->manufacturer : "(unknown)",
                 port->managed_gpio_count,
                 port->backlight_set ? "yes" : "no");
    } else {
        ESP_LOGI(TAG, "Board port cleared (NULL)");
    }
}

const hyperwisor_port_t *hyperwisor_get_port(void)
{
    return s_port;
}
