#pragma once

/* =====================================================================
 *  hds_sink -- route dashboard widget actions, local path first.
 *
 *  The renderer (components/app/hds) is display-only: it turns a spec
 *  into LVGL objects and hands every actuation to a sink. This is that
 *  sink for the real board.
 *
 *  The rule it implements: a panel bolted to the machine it controls
 *  must not round-trip a relay through the cloud. Pressing a switch
 *  should close the contact whether or not there is WiFi, and with
 *  panel-tap latency rather than internet latency. So a command this
 *  board can satisfy itself goes straight to the Modbus worker, and
 *  only genuinely remote commands go out over the websocket.
 *
 *  Install it before building the dashboard:
 *      hds_set_emit(hds_sink_emit, NULL);
 * ===================================================================== */

#include "hds.h"

/* An hds_emit_fn. `payload` arrives with live values already injected.
 * Resolves locally where it can, falls back to the websocket
 * otherwise. */
void hds_sink_emit(const char *target, const cJSON *payload, void *user);

/* Counters, for the service screen / diagnostics. */
void hds_sink_stats(uint32_t *local, uint32_t *remote, uint32_t *dropped);
