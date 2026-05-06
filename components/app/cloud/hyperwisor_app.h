#pragma once

/* =====================================================================
 *  Hyperwisor application glue
 *
 *  This layer lives in the `app` component and ties the display-agnostic
 *  hyperwisor library to our domain objects (ctrl_state, bmp280,
 *  hvac_controller, ac_state, modbus_task).
 *
 *  Responsibilities:
 *    1. Register custom command handlers with the hyperwisor core:
 *         - "relay_control"  : toggle user relay coils 0..7
 *         - "hvac_control"   : mutate ac_state (power/auto/fan/temp)
 *         - "get_status"     : respond with the full live snapshot
 *         - "config"         : bind cloud widget IDs + target app ID
 *    2. Proactively push live values to the Hyperwisor cloud whenever
 *       local state changes (voluntary, device-initiated, no polling):
 *         - Relay coil state changes (modbus coil write confirmations)
 *         - BMP280 sample (temperature + pressure)
 *         - HVAC controller tick (mode, fan step, clutch, cabin, setpoint)
 * ===================================================================== */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVS namespace for application-level bindings (separate from the
 * hyperwisor library's own "hyperwisor" namespace). */
#define HYPERWISOR_APP_NVS_NS       "hypr_app"

/* NVS keys */
#define HYPERWISOR_APP_KEY_TARGET   "targetid"   /* cloud app / dashboard ID   */
#define HYPERWISOR_APP_KEY_RELAY_FMT "rw_%d"     /* widget ID for coil %d      */
#define HYPERWISOR_APP_KEY_W_TEMP   "w_temp"     /* cabin temperature widget   */
#define HYPERWISOR_APP_KEY_W_HPA    "w_hpa"      /* cabin pressure widget      */
#define HYPERWISOR_APP_KEY_W_MODE   "w_hvac_mode"
#define HYPERWISOR_APP_KEY_W_FAN    "w_hvac_fan"
#define HYPERWISOR_APP_KEY_W_CLUTCH "w_hvac_clutch"
#define HYPERWISOR_APP_KEY_W_CUR    "w_hvac_cur"
#define HYPERWISOR_APP_KEY_W_SET    "w_hvac_set"

/* Register all custom command handlers with the hyperwisor core and
 * load cached widget-ID bindings from NVS. Call AFTER hyperwisor_init()
 * and BEFORE hyperwisor_start(). */
esp_err_t hyperwisor_app_init(void);

/* -------------------- Proactive pushers --------------------
 * Each of these is safe to call from any task. Internally they check
 * that the websocket is connected and that a cloud target has been
 * bound; if not, they return quietly. */

/* Push a single relay coil's state. `coil_index` must be 0..7. Coils
 * 8..15 are HVAC-owned and are never pushed as user relays. */
void hyperwisor_app_push_relay_state(int coil_index, bool on);

/* Push the latest BMP280 sample. */
void hyperwisor_app_push_bmp280(float temp_c, float pressure_hpa);

/* Push the latest HVAC controller snapshot. */
struct hvac_status_t;  /* forward decl to keep header light */
void hyperwisor_app_push_hvac_status(void);

/* Accessors (mostly for diagnostics / UI status screen) */
bool  hyperwisor_app_is_configured(void);
const char *hyperwisor_app_get_target_id(void);

#ifdef __cplusplus
}
#endif
