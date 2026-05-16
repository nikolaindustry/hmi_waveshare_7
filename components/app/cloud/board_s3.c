/**
 * @file board_s3.c
 * @brief Waveshare ESP32-S3-Touch-LCD-7 board port for the Hyperwisor library
 *
 * Supplies:
 *   - Board identity (name, manufacturer)
 *   - Display brightness callbacks (wrapping display_brightness module)
 *   - GPIO allowlist (empty — all pins are dedicated to display/I2C/RS-485;
 *     relay control on this board goes through Modbus, not direct GPIO)
 */

#include "hyperwisor_port.h"
#include "display_brightness.h"
#include "bsp.h"

/* ---- Board identity ---- */
#define BOARD_NAME       "Waveshare ESP32-S3-Touch-LCD-7"
#define BOARD_MFG        "Waveshare"

/* ---- GPIO allowlist ----
 * On this board every GPIO is consumed by the RGB LCD, I2C bus, or
 * RS-485 transceiver. There are no free pins for cloud GPIO control.
 * Relay control goes through the Modbus RTU master (RS-485), not
 * direct GPIO. If a user wires external hardware to the I2C header
 * pins, they can add GPIO8/GPIO9 here. */
static const int s_managed_gpios[] = {};
#define MANAGED_GPIO_COUNT  0

/* ---- Display brightness wrappers ---- */

static void s3_backlight_set(bool on)
{
    bsp_display_backlight(on);
}

static void s3_backlight_set_level(uint8_t level_0_100)
{
    display_brightness_set(level_0_100);
}

static uint8_t s3_backlight_get_level(void)
{
    return display_brightness_get();
}

/* ---- Public port descriptor ---- */

const hyperwisor_port_t board_port_s3 = {
    .board_name          = BOARD_NAME,
    .manufacturer        = BOARD_MFG,
    .managed_gpios       = s_managed_gpios,
    .managed_gpio_count  = MANAGED_GPIO_COUNT,
    .backlight_set       = s3_backlight_set,
    .backlight_set_level = s3_backlight_set_level,
    .backlight_get_level = s3_backlight_get_level,
};
