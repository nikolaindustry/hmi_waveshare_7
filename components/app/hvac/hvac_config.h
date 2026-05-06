#pragma once

/* ======================================================================
 *  HVAC hardware & policy configuration
 *
 *  Every knob the firmware needs to drive the bus AC lives here so the
 *  control logic itself never needs to change when the hardware arrives
 *  and the real wiring is confirmed. Edit these values, recompile.
 *
 *  Relay coils 8..15 on the Waveshare 16CH relay board (slave 0x01) are
 *  the "spare" block -- reading lights occupy coils 0..7. Coils that
 *  are not used are set to HVAC_COIL_UNUSED (0xFFFF) which the
 *  controller treats as "skip this output, hardware not installed".
 * ====================================================================== */

#define HVAC_COIL_UNUSED          0xFFFFu

/* ---- Blower-fan speed taps ----
 * Each active coil energises one tap of the OEM blower motor's
 * resistor pack. Only ONE may be ON at any time -- the controller
 * enforces break-before-make between speed changes. Set any slot to
 * HVAC_COIL_UNUSED if your blower has fewer speeds. */
#define HVAC_FAN_COIL_LOW         8u
#define HVAC_FAN_COIL_MED         9u
#define HVAC_FAN_COIL_HIGH        10u
#define HVAC_FAN_COIL_MAX         HVAC_COIL_UNUSED   /* 4-speed? set to 11u */

/* ---- Compressor clutch ----
 * 12 V/24 V electromagnetic clutch on the A/C compressor. The only
 * relay that actually produces cold air. */
#define HVAC_CLUTCH_COIL          12u

/* ---- Optional extras (wire later if the OEM box needs them) ---- */
#define HVAC_IDLEUP_COIL          HVAC_COIL_UNUSED   /* engine idle-up signal */
#define HVAC_RECIRC_COIL          HVAC_COIL_UNUSED   /* motorised recirc flap */
#define HVAC_HEATER_COIL          HVAC_COIL_UNUSED   /* heater valve solenoid */

/* ---- Temperature thresholds (delta = cabin - target, in °C) ----
 *
 *   delta >= HVAC_TEMP_BAND_HIGH_C  -> fan HIGH + clutch ON
 *   delta >= HVAC_TEMP_BAND_MED_C   -> fan MED  + clutch ON
 *   delta >= HVAC_TEMP_BAND_LOW_C   -> fan LOW  + clutch ON
 *   delta <= -HVAC_TEMP_HYST_C      -> clutch OFF (fan LOW for circulation)
 *   otherwise                       -> hold previous state (deadband)
 */
#define HVAC_TEMP_BAND_HIGH_C     3.0f
#define HVAC_TEMP_BAND_MED_C      1.5f
#define HVAC_TEMP_BAND_LOW_C      0.3f
#define HVAC_TEMP_HYST_C          0.3f

/* ---- Safety interlocks (milliseconds) ----
 *
 *  HVAC_BLOWER_PRESPIN_MS : blower must run this long before clutch is
 *                           allowed to engage. Prevents evap coil freeze.
 *  HVAC_CLUTCH_MIN_OFF_MS : once clutch goes OFF, it cannot go back ON
 *                           for this many ms. Anti-short-cycle protection
 *                           for the compressor motor windings (~3 min is
 *                           standard for automotive compressors).
 *  HVAC_SPEED_GAP_MS      : dead time between turning the old speed tap
 *                           OFF and the new one ON. Break-before-make
 *                           keeps the resistor pack from being fed by
 *                           two taps simultaneously.
 *  HVAC_POWEROFF_BLOWER_MS: after master OFF the blower keeps running
 *                           for this long with clutch already de-energised,
 *                           flushing cold air and draining the coil.
 */
/* Tuned for "start cooling ASAP" while still keeping the compressor
 * from being destroyed by short cycling:
 *   PRESPIN  500 ms  -> fan just needs to be moving, not fully spun up
 *   MIN_OFF   30 s   -> minimum safe dwell for a small A/C compressor
 *                      (180 s is OEM-conservative, 30 s is the lower
 *                      bound most compressor makers publish)
 *   SPEED_GAP 80 ms  -> keep, protects the resistor pack
 *   POWEROFF  5 s    -> short purge is enough; long purge was annoying */
#define HVAC_BLOWER_PRESPIN_MS    500u
#define HVAC_CLUTCH_MIN_OFF_MS    30000u
#define HVAC_SPEED_GAP_MS         80u
#define HVAC_POWEROFF_BLOWER_MS   5000u

/* ---- Control loop cadence (ms) ----
 * One second is plenty for cabin air -- nothing changes fast enough to
 * need tighter updates, and we avoid hammering the Modbus bus. */
#define HVAC_TICK_MS              1000u
