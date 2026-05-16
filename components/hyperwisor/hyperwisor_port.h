/**
 * @file hyperwisor_port.h
 * @brief Board port interface — makes the Hyperwisor library board-agnostic
 *
 * The library itself knows nothing about which GPIOs are safe to touch,
 * how to dim a backlight, or what board it runs on. The application
 * supplies a hyperwisor_port_t (typically a static const) that answers
 * those questions, and the library calls through the function pointers
 * and data arrays it finds there.
 *
 * Usage (in your main or board port source):
 *
 *   static const hyperwisor_port_t my_port = {
 *       .board_name       = "Waveshare ESP32-S3-Touch-LCD-7",
 *       .manufacturer     = "Waveshare",
 *       .managed_gpios    = (const int[] ){8, 9},
 *       .managed_gpio_count = 2,
 *       .backlight_set       = my_backlight_set,
 *       .backlight_set_level = my_backlight_set_level,
 *       .backlight_get_level = my_backlight_get_level,
 *   };
 *   hyperwisor_set_port(&my_port);
 *
 * Call hyperwisor_set_port() before or during hyperwisor_init().
 */

#ifndef HYPERWISOR_PORT_H
#define HYPERWISOR_PORT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Board port descriptor — supplied by the application, consumed
 *        by the Hyperwisor library.
 *
 * All pointer members may be NULL (features are silently disabled).
 * The struct must remain valid for the lifetime of the application
 * (static const or heap — not stack).
 */
typedef struct {
    /* ---- Board identity (used in SYSTEM/info responses) ---- */
    const char *board_name;       /**< Human-readable board name, or NULL */
    const char *manufacturer;     /**< Manufacturer string, or NULL */

    /* ---- GPIO allowlist for cloud control ---- */
    const int *managed_gpios;     /**< Array of GPIO numbers (int), or NULL (deny all) */
    int        managed_gpio_count;/**< Number of entries in managed_gpios */

    /* ---- Display brightness (optional — NULL if no display) ---- */

    /** Turn the backlight on (true) or off (false). */
    void     (*backlight_set)(bool on);

    /** Set brightness 0..100 (persists to NVS if the port implementation wants). */
    void     (*backlight_set_level)(uint8_t level_0_100);

    /** Return current brightness level 0..100. */
    uint8_t  (*backlight_get_level)(void);
} hyperwisor_port_t;

/**
 * @brief Register the board port with the Hyperwisor library.
 *
 * Must be called before or during hyperwisor_init(). If never called,
 * all port-dependent features (GPIO allowlist, brightness) are inert.
 */
void hyperwisor_set_port(const hyperwisor_port_t *port);

/**
 * @brief Get the currently registered board port (or NULL).
 */
const hyperwisor_port_t *hyperwisor_get_port(void);

#ifdef __cplusplus
}
#endif

#endif /* HYPERWISOR_PORT_H */
