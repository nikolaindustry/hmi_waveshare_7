#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal BMP280 driver for the Waveshare ESP32-S3-Touch-LCD-7 I2C
 * header. Shares the i2c_master bus owned by bsp_s3_rgb (the same one
 * GT911 + CH422G sit on). Auto-detects I2C address 0x76 / 0x77 and
 * falls back silently if no sensor is found, so the firmware still
 * runs fine without hardware attached.
 */

/* Bring the driver up. Safe to call after bsp_init() has run
 * (i.e. after the I2C bus exists). Returns ESP_OK on success,
 * ESP_ERR_NOT_FOUND if no BMP280 is on either address. */
esp_err_t bmp280_init(void);

/* Start a FreeRTOS task that samples the sensor every ~1000 ms. */
void bmp280_task_start(void);

/* True once the driver has seen at least one successful sample. */
bool bmp280_is_ready(void);

/* Copy the latest reading out under a mutex. Either pointer may be
 * NULL. temp_c is in degrees C (e.g. 24.31), pressure_hpa is absolute
 * pressure in hectopascals (e.g. 1013.25). Returns false if the
 * sensor has never produced a valid reading. */
bool bmp280_get(float *temp_c, float *pressure_hpa);

#ifdef __cplusplus
}
#endif
