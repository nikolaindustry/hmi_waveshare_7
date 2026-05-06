/**
 * @file display_brightness.h
 * @brief Persistent LCD brightness control.
 *
 * Hardware reality on this board (Waveshare ESP32-S3-Touch-LCD-7):
 *   The LCD backlight is wired to CH422G EXIO2 (a plain digital I2C I/O
 *   expander pin, NOT a PWM/LEDC channel). True analog dimming is not
 *   possible at the hardware level.
 *
 * Strategy (Option C from the design discussion):
 *   - A persistent value 0..100 is stored in its own tiny NVS namespace
 *     "disp" / key "bri".
 *   - level == 0   -> backlight turned OFF via bsp_display_backlight(false).
 *                     Reserved for a future explicit "screen off" button;
 *                     the slider in the Maintenance tab does NOT expose 0,
 *                     it clamps to >= 5 so the screen never blanks
 *                     irrecoverably.
 *   - level 1..100 -> backlight ON, and lv_layer_sys() (the always-on-top
 *                     LVGL layer) is styled opaque black with a variable
 *                     opacity = (100 - level) * 255 / 100. This darkens
 *                     the whole visible frame by compositing. The panel
 *                     itself still draws at max brightness, so power draw
 *                     is unchanged -- this is a visual-only dim.
 *
 * The module is self-contained: it initialises nvs_flash on its own (same
 * pattern owner_name uses), so it can run BEFORE hyperwisor_nvs_init()
 * and is decoupled from the cloud stack.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the persisted brightness level from NVS into the module.
 * Safe to call before LVGL is up. Defaults to 100 if no key is stored. */
void display_brightness_init(void);

/* Apply the currently-loaded brightness to the hardware backlight + the
 * LVGL overlay. Must be called AFTER bsp_init() (needs LVGL + a display). */
void display_brightness_apply(void);

/* Current level, clamped 0..100. */
uint8_t display_brightness_get(void);

/* Apply a new level visually WITHOUT persisting to NVS.
 * Intended for live slider-drag feedback. */
void display_brightness_preview(uint8_t level_0_100);

/* Apply and persist a new level. Clamps out-of-range. */
esp_err_t display_brightness_set(uint8_t level_0_100);

#ifdef __cplusplus
}
#endif
