#pragma once
#include "lvgl.h"

/* Each tab paints into the content container supplied here.
 * The content container is cleared by the shell before each call. */
typedef void (*ui_tab_builder_t)(lv_obj_t *content);

void ui_tab_overview_build(lv_obj_t *content);
void ui_tab_control_build(lv_obj_t *content);
void ui_tab_climate_build(lv_obj_t *content);
void ui_tab_entertainment_build(lv_obj_t *content);
void ui_tab_maintenance_build(lv_obj_t *content);
