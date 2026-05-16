/**
 * @file hyperwisor_gpio.h
 * @brief GPIO management from the Hyperwisor cloud dashboard
 *
 * When CONFIG_HYPERWISOR_ENABLE_GPIO is set, the library registers a
 * GPIO_MANAGEMENT command handler that matches the Arduino
 * hyperwisor-iot behaviour:
 *
 *   command:  "GPIO_MANAGEMENT"
 *   actions:  [{"action":"ON"/"OFF", "params":{"gpio":4,"pinmode":"OUTPUT","status":"HIGH"}}]
 *
 * Safety: only pins listed in a board-supplied allowlist are accessible.
 * If no allowlist is provided (NULL), ALL pins are rejected — the cloud
 * cannot touch any GPIO.  This is the default for headless deployments.
 */

#ifndef HYPERWISOR_GPIO_H
#define HYPERWISOR_GPIO_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the allowlist of GPIOs that the cloud may control.
 *
 * Call this before or during hyperwisor_init().  The array must remain
 * valid for the lifetime of the application (static or heap, not stack).
 *
 * @param gpios  Pointer to array of GPIO numbers, or NULL to deny all.
 * @param count  Number of entries in the array.
 */
void hyperwisor_gpio_set_allowlist(const int *gpios, int count);

/**
 * @brief Auto-register the GPIO_MANAGEMENT command handler.
 *
 * Called by hyperwisor_core when CONFIG_HYPERWISOR_ENABLE_GPIO is set.
 */
void hyperwisor_gpio_auto_register(void);

/* ---- Low-level helpers (public for board ports / tests) ---- */

esp_err_t hyperwisor_gpio_set_pin_mode(int pin, const char *mode);
esp_err_t hyperwisor_gpio_digital_write(int pin, const char *level);
bool      hyperwisor_gpio_digital_read(int pin);
esp_err_t hyperwisor_gpio_save_state(int pin, int state);
int       hyperwisor_gpio_load_state(int pin);
void      hyperwisor_gpio_restore_all_states(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_GPIO_H */
