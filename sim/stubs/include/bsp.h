/* Minimal bsp.h for the sim: the real one pulls in esp_lcd / GT911 /
 * i2c_master. The UI only ever calls the backlight helper. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t bsp_init(void);
void      bsp_display_backlight(bool on);
esp_err_t bsp_io_expander_set(uint8_t bit, uint8_t level);
