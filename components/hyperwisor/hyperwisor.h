#pragma once

/* Umbrella header for the Hyperwisor IoT component.
 * Downstream code should `#include "hyperwisor.h"` and pull everything
 * (core, wifi, ws, commands, nvs, widget, ntp, http) in one shot.
 * Optional features (OTA, GPIO) are included when their Kconfig is on.
 */

#include "hyperwisor_core.h"
#include "hyperwisor_wifi.h"
#include "hyperwisor_ws.h"
#include "hyperwisor_cmd.h"
#include "hyperwisor_nvs.h"
#include "hyperwisor_widget.h"
#include "hyperwisor_ntp.h"
#include "hyperwisor_http.h"

#if CONFIG_HYPERWISOR_ENABLE_OTA
#include "hyperwisor_ota.h"
#endif
