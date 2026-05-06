#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------
 * Control-tab state model.
 *
 * Everything is kept behind accessor functions + exposed arrays so
 * the UI can walk a list of "items" without caring how many there
 * are or what they are called.  Adding a new reading light or switch
 * at a later date means editing the arrays in ctrl_state.c -- NO UI
 * change is required.
 *
 * `name` is a fixed-size char buffer (not a pointer to literal) so
 * the label can be mutated at runtime later, for example loaded from
 * NVS or renamed via a settings screen.
 * ------------------------------------------------------------------ */

#define CTRL_NAME_MAX 24

/* 0xFFFF sentinel = "not mapped to any Modbus coil" */
#define CTRL_COIL_NONE 0xFFFFu

typedef struct {
    char     name[CTRL_NAME_MAX];
    bool     on;        /* desired/displayed state */
    bool     pending;   /* true while a Modbus write is in flight */
    uint16_t coil;      /* Modbus coil address, or CTRL_COIL_NONE */
} ctrl_toggle_t;

/* RGB zone: hue/sat come from the colorwheel, brightness is a
 * separate slider, master on/off is a pill switch. */
typedef struct {
    uint16_t hue;        /* 0..359 */
    uint8_t  sat;        /* 0..100 */
    uint8_t  brightness; /* 0..100 */
    bool     on;
} ctrl_rgb_t;

/* Ambient fibre-optic star roof. */
typedef struct {
    bool    on;
    uint8_t intensity; /* 0..100 */
} ctrl_star_t;

/* Singletons ------------------------------------------------------- */
ctrl_rgb_t    *ctrl_rgb_roof(void);
ctrl_rgb_t    *ctrl_rgb_floor(void);
ctrl_star_t   *ctrl_star_roof(void);

/* Arrays ----------------------------------------------------------- */
/* Both return a pointer to the first element; count is written
 * through out_count (may be NULL). */
ctrl_toggle_t *ctrl_reading_lights(int *out_count);
ctrl_toggle_t *ctrl_switches(int *out_count);
