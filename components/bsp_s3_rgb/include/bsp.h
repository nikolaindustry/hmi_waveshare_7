#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "driver/i2c_master.h"
#include "bsp_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- I2C ---- */
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void);

/* ---- CH422G I/O expander ---- */
esp_err_t bsp_io_expander_init(void);
esp_err_t bsp_io_expander_set(uint8_t bit, bool level);
esp_err_t bsp_io_expander_write_byte(uint8_t byte);

/* ---- Display (RGB panel) ---- */
esp_err_t bsp_display_start(void);
esp_lcd_panel_handle_t bsp_display_get_panel(void);

/* Backlight (via CH422G bit 2) */
static inline esp_err_t bsp_display_backlight(bool on)
{
    return bsp_io_expander_write_byte(on ? CH422G_IO_BL_ON_VAL
                                         : CH422G_IO_BL_OFF_VAL);
}

/* ---- Touch (GT911) ---- */
esp_err_t bsp_touch_init(void);
esp_lcd_touch_handle_t bsp_touch_get_handle(void);

/* ---- One-shot bring-up: I2C + CH422G + panel + touch + LVGL ---- */
esp_err_t bsp_init(void);

#ifdef __cplusplus
}
#endif
