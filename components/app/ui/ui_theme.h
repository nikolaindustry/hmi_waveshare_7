#pragma once
#include "lvgl.h"
#include <stdint.h>

/* ---- Runtime colour palette -----------------------------------------
 *
 * Each UI_COLOR_* is a 0xRRGGBB value populated at boot from NVS via
 * ui_theme_init(). All UI code reads them as plain uint32_t, so the
 * existing call sites (hex(UI_COLOR_X)) keep working unchanged.
 *
 * The palette covers three intents:
 *   - surfaces  (BG / SHELL / SURFACE / SURFACE_HI / SURFACE_LO / STROKE)
 *   - accents   (ACCENT / TAB / COOL / HOT / ON / WARN)
 *   - text      (TEXT / MUTED / DIM)
 */
extern uint32_t UI_COLOR_BG;
extern uint32_t UI_COLOR_SHELL;
extern uint32_t UI_COLOR_SURFACE;
extern uint32_t UI_COLOR_SURFACE_HI;
extern uint32_t UI_COLOR_SURFACE_LO;
extern uint32_t UI_COLOR_STROKE;

extern uint32_t UI_COLOR_ACCENT;
extern uint32_t UI_COLOR_TAB;
extern uint32_t UI_COLOR_COOL;
extern uint32_t UI_COLOR_HOT;
extern uint32_t UI_COLOR_ON;
extern uint32_t UI_COLOR_WARN;

extern uint32_t UI_COLOR_TEXT;
extern uint32_t UI_COLOR_MUTED;
extern uint32_t UI_COLOR_DIM;

/* ---- Spacing / sizes ---- */
#define UI_TOPBAR_H          48
#define UI_TABBAR_H          64
#define UI_CARD_RADIUS       20
#define UI_PAD               16
#define UI_PAD_S             8

/* ---- Theme presets ---------------------------------------------------
 *
 * UI_THEME_COUNT preset palettes that can be selected at runtime from
 * the Maintenance tab. The selected index is persisted to NVS; a
 * reboot applies it to every tab.
 */
typedef enum {
    UI_THEME_BROWN = 0,        /* default - brown leather cabin        */
    UI_THEME_PURPLE,           /* royal purple                         */
    UI_THEME_RED,              /* crimson sports                       */
    UI_THEME_WHITE_GOLD,       /* ivory + gold                         */
    UI_THEME_BLACK_GOLD,       /* obsidian + gold                      */
    UI_THEME_WHITE_BLACK,      /* pure monochrome light                */
    UI_THEME_BLACK,            /* slate black                          */
    UI_THEME_WHITE,            /* arctic white                         */
    UI_THEME_GREEN_GOLD,       /* forest green + gold                  */
    UI_THEME_SILVER_GOLD,      /* silver + gold                        */
    UI_THEME_COUNT
} ui_theme_id_t;

/* Initialise the runtime palette from NVS. Must be called BEFORE any
 * UI build runs. Falls back to UI_THEME_BROWN if NVS is unset. */
void ui_theme_init(void);

/* Return the currently-active preset id. */
ui_theme_id_t ui_theme_get(void);

/* Persist a new theme to NVS. Call esp_restart() afterwards to apply. */
void ui_theme_save(ui_theme_id_t id);

/* Human-readable preset name (e.g. "Brown Leather") for the picker. */
const char *ui_theme_name(ui_theme_id_t id);

/* Small swatch colours used by the picker to preview each preset.
 * Returns two 0xRRGGBB values: primary surface + accent. */
void ui_theme_swatch(ui_theme_id_t id, uint32_t *primary, uint32_t *accent);

/* ---- Screen-builder signature ---- */
typedef void (*ui_screen_builder_t)(lv_obj_t *scr);
