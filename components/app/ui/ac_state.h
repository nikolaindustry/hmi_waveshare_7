#pragma once
#include <stdbool.h>

/* Mock AC state. All UI writes + reads go through the getter so a
 * transport (MQTT / CAN / etc.) can be plugged in later without
 * touching the widgets. */

typedef enum {
    AC_AIRFLOW_UPPER = 1 << 0,
    AC_AIRFLOW_MID   = 1 << 1,
    AC_AIRFLOW_LOWER = 1 << 2,
    AC_AIRFLOW_WIND  = 1 << 3,
} ac_airflow_bit_t;

typedef struct {
    bool   power;           /* master on/off                 */
    bool   auto_mode;       /* AUTO button state             */
    int    fan_level;       /* 1..5                          */
    int    temp_c;          /* 16..30                        */
    int    airflow_mask;    /* OR of ac_airflow_bit_t values */
    bool   front_defrost;
    bool   rear_defrost;
    bool   fresh_air;       /* false = recirculation         */
} ac_state_t;

ac_state_t *ac_state(void);
