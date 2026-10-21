#pragma once

/* =====================================================================
 *  hds_assets -- image blobs for the dashboard renderer.
 *
 *  tools/hds_transcode.py fetches every image the dashboard references,
 *  downscales it to the widget box and writes it out as an "HDSI" blob:
 *
 *      offset  size  field
 *      0       4     magic "HDSI"
 *      4       2     width   (u16 LE)
 *      6       2     height  (u16 LE)
 *      8       1     colour format: 4 = TRUE_COLOR, 5 = TRUE_COLOR_ALPHA
 *      9       3     reserved
 *      12      ...   pixels: RGB565 LE, plus one A8 byte per pixel when
 *                    the format is TRUE_COLOR_ALPHA
 *
 *  That is LVGL's native in-memory layout for LV_COLOR_DEPTH 16 with
 *  LV_COLOR_16_SWAP 0, so loading is a read into a buffer and a header
 *  fill -- no decode, no PNG library, no rasteriser on the device.
 * ===================================================================== */

#include "lvgl.h"

/* Point the loader at the directory holding <key>.bin, e.g.
 * "assets/dashboard/assets" in the sim, "/spiffs/dashboard/assets" on
 * the device. */
void hds_assets_init(const char *dir);

/* Load (or return a cached) descriptor for an asset key. NULL if the
 * blob is missing or malformed -- the caller should draw a placeholder
 * rather than fail the whole page. */
const lv_img_dsc_t *hds_assets_get(const char *key);

/* Drop every cached blob. Call when swapping to a different dashboard. */
void hds_assets_reset(void);
