#pragma once
#include "lvgl.h"

/* Entry point after splash. Builds persistent topbar + bottom tabbar
 * + content container on the supplied screen, then paints the default
 * (Climate) tab inside the content container. */
void ui_shell_build(lv_obj_t *scr);
