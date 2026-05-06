#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * HMI role persistence.
 *
 * Two physical displays on the same RS-485 bus; one acts as the
 * Modbus master (talks to the relay board + RGB slave) and owns the
 * BMP280 + HVAC controller, the other acts as a Modbus slave at
 * address 0x10 and mirrors the primary's state.
 *
 * The role is stored in NVS so the same firmware binary can be
 * flashed to both displays. Default on first boot is PRIMARY; the
 * user changes it via the Maintenance tab and the display reboots.
 * ------------------------------------------------------------------ */

typedef enum {
    HMI_ROLE_PRIMARY   = 0,
    HMI_ROLE_SECONDARY = 1,
} hmi_role_t;

/* Read the persisted role (default PRIMARY). Safe to call once at
 * boot before any UI/Modbus code. Opens NVS if needed. */
esp_err_t hmi_role_init(void);

/* Return the current role. Never fails after init (falls back to
 * PRIMARY if init was skipped). */
hmi_role_t hmi_role_get(void);

/* Persist the new role. Caller is expected to reboot shortly after
 * so every subsystem picks up the new behaviour cleanly. */
esp_err_t hmi_role_set(hmi_role_t role);

/* Short display string for UI labels ("PRIMARY" / "SECONDARY"). */
const char *hmi_role_str(hmi_role_t role);

#ifdef __cplusplus
}
#endif
