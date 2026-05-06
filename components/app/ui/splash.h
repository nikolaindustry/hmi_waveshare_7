#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Chintamani logo image descriptor (generated C array) */
extern const lv_img_dsc_t logo_chintamani;

/**
 * Show splash screen for @p duration_ms, then invoke @p on_done(target_scr).
 * The callback receives a freshly-created screen to paint the main UI onto.
 * splash_show() must be called with the LVGL port mutex already held.
 */
typedef void (*splash_done_cb_t)(lv_obj_t *target_scr);
void splash_show(uint32_t duration_ms, splash_done_cb_t on_done);

#ifdef __cplusplus
}
#endif
