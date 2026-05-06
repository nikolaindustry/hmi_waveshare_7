#pragma once
#include "esp_err.h"

/* ------------------------------------------------------------------
 * Persisted display names for the Control tab's toggle tiles.
 *
 * There are two groups of 8 tiles each:
 *   - Reading Lights (kind = TILE_KIND_READING, idx 0..7)
 *   - Switches       (kind = TILE_KIND_SWITCH,  idx 0..7)
 *
 * Names live in NVS under namespace "tiles" using keys
 * "r0".."r7" and "s0".."s7". They are loaded directly into the
 * `name[]` buffers of the ctrl_state arrays at init time so the
 * Control tab just reads items[i].name as before -- no UI change.
 *
 * tile_names_init() must run AFTER nvs_flash_init() has succeeded
 * (owner_name_init() already takes care of that), and BEFORE the
 * LVGL UI is built.
 * ------------------------------------------------------------------ */

#define TILE_KIND_READING 0
#define TILE_KIND_SWITCH  1

esp_err_t tile_names_init(void);

/* Persist `name` for the given tile and update the live
 * ctrl_state array so the Control tab picks up the new label on
 * its next rebuild. Empty / NULL names are rejected. Names longer
 * than CTRL_NAME_MAX-1 are truncated. */
esp_err_t tile_names_set(int kind, int idx, const char *name);
