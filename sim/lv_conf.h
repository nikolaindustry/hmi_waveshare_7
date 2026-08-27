/* LVGL config for the desktop simulator.
 *
 * lv_conf_internal.h supplies a default for every option we don't set,
 * so this only needs to list what must match the real device (colour
 * depth, fonts) plus what differs on desktop (heap from malloc). Keep
 * the font list in sync with the CONFIG_LV_FONT_MONTSERRAT_* entries in
 * the project's sdkconfig, or text will render differently here than on
 * the panel. */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ---- must match the device (sdkconfig: CONFIG_LV_COLOR_DEPTH=16) ---- */
#define LV_COLOR_DEPTH        16
#define LV_COLOR_16_SWAP      0

/* ---- desktop: just use the C heap, no fixed LVGL pool to tune ---- */
#define LV_MEM_CUSTOM         1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc

/* We drive lv_tick_inc() ourselves from the SDL loop. */
#define LV_TICK_CUSTOM        0

/* ---- fonts actually referenced by the UI ---- */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_40 1

#define LV_USE_LOG            1
#define LV_LOG_LEVEL          LV_LOG_LEVEL_WARN

/* Handy while iterating on layout: set to 1 for an FPS/heap overlay. */
#define LV_USE_PERF_MONITOR   0
#define LV_USE_MEM_MONITOR    0

#endif /* LV_CONF_H */
