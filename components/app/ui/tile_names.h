#pragma once
#include "esp_err.h"
#include <stdint.h>

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
 * The same module also persists the two per-tile behaviour flags --
 * momentary (push-to-on/release-to-off) and home (show as a quick
 * button on the Overview screen) -- as two bitmask keys, "mom_r"/
 * "mom_s" and "home_r"/"home_s". A bitmask rather than a key per tile
 * because the flags are always read and written as a group.
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

/* Persist the per-tile behaviour flags for one group and apply them to
 * the live ctrl_state array. `momentary_mask` / `home_mask` are bit i =
 * tile i. Call once per group after the user edits them; writing the
 * whole group at once keeps NVS wear down versus a key per tile. */
esp_err_t tile_flags_set(int kind, uint16_t momentary_mask, uint16_t home_mask);

/* Count tiles with home=true across BOTH groups. The Overview screen
 * shows at most CTRL_HOME_MAX of them. */
int tile_home_count(void);
