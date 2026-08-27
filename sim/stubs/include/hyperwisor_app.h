#pragma once
#include "esp_err.h"
const char *hyperwisor_app_get_target_id(void);
esp_err_t   hyperwisor_app_init(void);
void        hyperwisor_app_push_relay_state(int coil, bool on);
