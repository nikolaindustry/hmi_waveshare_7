#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise LVGL, attach RGB display + GT911 touch, spawn handler task.
 * Must be called after bsp_display_start() and bsp_touch_init(). */
esp_err_t lvgl_port_init(void);

/* Acquire/release LVGL mutex. Any lv_* call from app code must be wrapped. */
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif
