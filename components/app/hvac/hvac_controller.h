#pragma once
#include "esp_err.h"

/* ------------------------------------------------------------------
 *  HVAC climate controller
 *
 *  A small FreeRTOS task that:
 *    1) Reads the live Climate tab state (ac_state) + BMP280 temperature.
 *    2) Decides what the blower and compressor clutch should do.
 *    3) Writes the decision to the Waveshare relay board via the
 *       existing modbus_task_request() queue (coils configured in
 *       hvac_config.h).
 *
 *  Modes
 *  -----
 *  - AUTO   : fan speed + clutch driven by (cabin_temp - target_c)
 *             against the bands in hvac_config.h. Deadband prevents
 *             clutch chattering around the setpoint.
 *  - MANUAL : user's fan_level slider (1..5) mapped to LOW/MED/HIGH.
 *             Clutch is still gated by safety interlocks.
 *
 *  Safety interlocks (always enforced, in both modes)
 *  --------------------------------------------------
 *  - Blower must run >= HVAC_BLOWER_PRESPIN_MS before clutch may engage.
 *  - After clutch OFF, enforce HVAC_CLUTCH_MIN_OFF_MS lockout.
 *  - Break-before-make on fan speed changes (HVAC_SPEED_GAP_MS dead time).
 *  - Master OFF: clutch drops instantly, blower runs HVAC_POWEROFF_BLOWER_MS
 *    longer to purge cold condensate from the evap coil.
 *  - BMP280 unavailable + AUTO mode: controller holds the previous
 *    commanded state and logs a warning, rather than guessing.
 * ------------------------------------------------------------------ */

/* Start the HVAC worker task. Safe to call exactly once, AFTER
 * modbus_task_start() and bmp280_task_start(). Returns ESP_OK on
 * success or ESP_FAIL if the task could not be created. */
esp_err_t hvac_controller_start(void);

/* ------------------------------------------------------------------
 *  Live status snapshot (for UI display)
 *
 *  The struct is filled atomically per tick. A UI poller can call
 *  hvac_controller_get_status() at any cadence and get a consistent
 *  view of what the controller commanded last.
 * ------------------------------------------------------------------ */
typedef enum {
    HVAC_MODE_OFF    = 0,    /* master power off */
    HVAC_MODE_AUTO   = 1,    /* thermostat driving */
    HVAC_MODE_MANUAL = 2,    /* user slider driving */
    HVAC_MODE_PURGE  = 3,    /* master just went off, blower draining evap */
} hvac_mode_t;

typedef enum {
    HVAC_WAIT_NONE      = 0, /* running as commanded */
    HVAC_WAIT_PRESPIN   = 1, /* fan just came on, clutch waiting for prespin */
    HVAC_WAIT_LOCKOUT   = 2, /* anti-short-cycle rest after clutch off */
    HVAC_WAIT_FAN_OFF   = 3, /* want clutch but fan is off */
    HVAC_WAIT_NO_SENSOR = 4, /* AUTO: BMP280 unavailable, holding state */
    HVAC_WAIT_PURGE     = 5, /* blower-purge countdown after master off */
} hvac_wait_t;

typedef struct {
    hvac_mode_t mode;
    hvac_wait_t wait;
    int         fan_step;       /* 0=OFF 1=LOW 2=MED 3=HIGH 4=MAX */
    int         fan_coil;       /* coil id (0..15), or -1 if OFF/unused */
    bool        clutch_on;
    int         clutch_coil;    /* coil id, or -1 if unused */
    bool        cabin_valid;    /* true if BMP280 reading is fresh */
    float       cabin_c;        /* last cabin temperature (only if valid) */
    float       target_c;       /* current UI setpoint */
    float       delta_c;        /* cabin - target (only if cabin_valid) */
    int         wait_ms_left;   /* remaining countdown for the active gate */
} hvac_status_t;

/* Copy the latest snapshot into *out. Always succeeds; if the task hasn't
 * ticked yet the struct is zero-filled with mode=OFF. */
void hvac_controller_get_status(hvac_status_t *out);
