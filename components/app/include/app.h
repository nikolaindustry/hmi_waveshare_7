#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Entry point called from app_main() after bsp_init(). Builds the LVGL UI. */
void app_start(void);

#ifdef __cplusplus
}
#endif
