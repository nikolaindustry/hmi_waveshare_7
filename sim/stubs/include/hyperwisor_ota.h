#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* Desktop-simulator stub of the OTA module.
 *
 * get_progress() normally reports -1 (idle). Set HMI_SIM_OTA=1 in the
 * environment and it instead ramps 0..100 over ~20 s a few seconds
 * after start, so the updating overlay can be checked on the desktop
 * without pushing a real firmware image to a panel. */
void hyperwisor_ota_get_version(char *out_buf, size_t buf_len);
int  hyperwisor_ota_get_progress(void);
bool hyperwisor_ota_is_in_progress(void);
