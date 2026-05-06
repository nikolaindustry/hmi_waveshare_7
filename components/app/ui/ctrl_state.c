#include "ctrl_state.h"

/* ---------- singletons ---------- */
/* Default colour = warm white (~2700-3000K incandescent look).
 * HSV (h=35, s=25, v=100) -> RGB roughly (255, 245, 192) which reads
 * as a soft cream/amber white on the XY-MOS LED strip -- matches the
 * brown leather cabin palette on boot instead of a cold blue-white. */
static ctrl_rgb_t  s_rgb_roof  = { .hue = 35, .sat = 25, .brightness = 100, .on = false };
static ctrl_rgb_t  s_rgb_floor = { .hue = 35, .sat = 25, .brightness = 100, .on = false };
static ctrl_star_t s_star      = { .on = false, .intensity = 60 };

/* ---------- arrays (edit these to add/rename without touching UI) ----------
 *
 * `coil` maps each tile to a Modbus coil on the Waveshare RTU Relay 16CH
 * (slave 0x01). Reading Lights 1..8 drive relays 1..8 (coils 0..7).
 * Switches 1..8 drive relays 9..16 (coils 8..15).
 */
static ctrl_toggle_t s_reading[] = {
    { "Reading Light 1", false, false, 0 },
    { "Reading Light 2", false, false, 1 },
    { "Reading Light 3", false, false, 2 },
    { "Reading Light 4", false, false, 3 },
    { "Reading Light 5", false, false, 4 },
    { "Reading Light 6", false, false, 5 },
    { "Reading Light 7", false, false, 6 },
    { "Reading Light 8", false, false, 7 },
};

static ctrl_toggle_t s_switches[] = {
    { "Switch 1", false, false,  8 },
    { "Switch 2", false, false,  9 },
    { "Switch 3", false, false, 10 },
    { "Switch 4", false, false, 11 },
    { "Switch 5", false, false, 12 },
    { "Switch 6", false, false, 13 },
    { "Switch 7", false, false, 14 },
    { "Switch 8", false, false, 15 },
};

/* ---------- getters ---------- */
ctrl_rgb_t  *ctrl_rgb_roof(void)  { return &s_rgb_roof;  }
ctrl_rgb_t  *ctrl_rgb_floor(void) { return &s_rgb_floor; }
ctrl_star_t *ctrl_star_roof(void) { return &s_star;      }

ctrl_toggle_t *ctrl_reading_lights(int *out_count)
{
    if (out_count) *out_count = (int)(sizeof(s_reading) / sizeof(s_reading[0]));
    return s_reading;
}

ctrl_toggle_t *ctrl_switches(int *out_count)
{
    if (out_count) *out_count = (int)(sizeof(s_switches) / sizeof(s_switches[0]));
    return s_switches;
}
