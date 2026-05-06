#include "ac_state.h"

static ac_state_t s_state = {
    .power         = false,
    .auto_mode     = true,
    .fan_level     = 3,
    .temp_c        = 20,
    .airflow_mask  = AC_AIRFLOW_MID,
    .front_defrost = false,
    .rear_defrost  = false,
    .fresh_air     = true,
};

ac_state_t *ac_state(void) { return &s_state; }
