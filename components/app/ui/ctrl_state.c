#include "ctrl_state.h"
#include "lvgl.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ctrl_state";

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
    /* Coil 15 now drives the star roof (CTRL_STAR_COIL), so this tile is
     * left unmapped rather than fighting it for the same relay. Point it
     * at a spare coil if the vehicle actually has an 8th switch. */
    { "Switch 8", false, false, CTRL_COIL_NONE },
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

/* =====================================================================
 *  Persistence (RGB zones + star roof)
 *
 *  These are user *preferences* with no hardware to read them back
 *  from -- unlike relay coils, which the primary re-reads from the
 *  relay board on every poll. So without this the cabin lighting came
 *  back as default warm-white on every power cycle and the customer had
 *  to dial their colour in again each time.
 *
 *  Rather than have every mutation site remember to call a save()
 *  (the getters hand out mutable pointers, and state is also changed by
 *  hmi_sync from the other HMI and by the cloud handler), a 1 Hz timer
 *  polls for changes. A value is only written once it has stopped
 *  moving for a tick, which naturally debounces a colourwheel drag --
 *  dragging emits ~60 changes/sec and must not become 60 flash writes.
 * ===================================================================== */

#define CTRL_NVS_NS   "ctrl"
#define CTRL_NVS_KEY  "zones"
#define CTRL_BLOB_VER 1

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint16_t roof_hue;   uint8_t roof_sat,  roof_bri,  roof_on;
    uint16_t floor_hue;  uint8_t floor_sat, floor_bri, floor_on;
    uint8_t  star_on,    star_intensity;
} ctrl_blob_t;

static ctrl_blob_t s_saved;   /* what is currently in NVS   */
static ctrl_blob_t s_seen;    /* what we saw one tick ago   */

static void ctrl_snapshot(ctrl_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version        = CTRL_BLOB_VER;
    b->roof_hue       = s_rgb_roof.hue;
    b->roof_sat       = s_rgb_roof.sat;
    b->roof_bri       = s_rgb_roof.brightness;
    b->roof_on        = s_rgb_roof.on ? 1 : 0;
    b->floor_hue      = s_rgb_floor.hue;
    b->floor_sat      = s_rgb_floor.sat;
    b->floor_bri      = s_rgb_floor.brightness;
    b->floor_on       = s_rgb_floor.on ? 1 : 0;
    b->star_on        = s_star.on ? 1 : 0;
    b->star_intensity = s_star.intensity;
}

/* Clamp on the way in: a corrupt or older blob must not be able to put
 * an out-of-range hue/percentage into the UI widgets. */
static void ctrl_apply(const ctrl_blob_t *b)
{
    s_rgb_roof.hue         = b->roof_hue  % 360;
    s_rgb_roof.sat         = b->roof_sat  > 100 ? 100 : b->roof_sat;
    s_rgb_roof.brightness  = b->roof_bri  > 100 ? 100 : b->roof_bri;
    s_rgb_roof.on          = b->roof_on != 0;
    s_rgb_floor.hue        = b->floor_hue % 360;
    s_rgb_floor.sat        = b->floor_sat > 100 ? 100 : b->floor_sat;
    s_rgb_floor.brightness = b->floor_bri > 100 ? 100 : b->floor_bri;
    s_rgb_floor.on         = b->floor_on != 0;
    s_star.on              = b->star_on != 0;
    s_star.intensity       = b->star_intensity > 100 ? 100 : b->star_intensity;
}

static void ctrl_persist_tick(lv_timer_t *t)
{
    (void)t;
    ctrl_blob_t now;
    ctrl_snapshot(&now);

    if (memcmp(&now, &s_saved, sizeof(now)) == 0) {   /* already stored  */
        s_seen = now;
        return;
    }
    if (memcmp(&now, &s_seen, sizeof(now)) != 0) {    /* still changing  */
        s_seen = now;
        return;
    }

    nvs_handle_t h;
    if (nvs_open(CTRL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, CTRL_NVS_KEY, &now, sizeof(now)) == ESP_OK &&
        nvs_commit(h) == ESP_OK) {
        s_saved = now;
        ESP_LOGI(TAG, "zone settings saved (roof %u/%u%%/%s, floor %u/%u%%/%s)",
                 now.roof_hue,  now.roof_bri,  now.roof_on  ? "on" : "off",
                 now.floor_hue, now.floor_bri, now.floor_on ? "on" : "off");
    }
    nvs_close(h);
}

void ctrl_state_init(void)
{
    nvs_handle_t h;
    if (nvs_open(CTRL_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ctrl_blob_t b;
        size_t len = sizeof(b);
        if (nvs_get_blob(h, CTRL_NVS_KEY, &b, &len) == ESP_OK &&
            len == sizeof(b) && b.version == CTRL_BLOB_VER) {
            ctrl_apply(&b);
            ESP_LOGI(TAG, "restored zone settings from NVS");
        } else {
            ESP_LOGI(TAG, "no saved zone settings, using defaults");
        }
        nvs_close(h);
    }

    /* Seed both baselines from whatever we ended up with, so an
     * unchanged boot never triggers a redundant write. */
    ctrl_snapshot(&s_saved);
    s_seen = s_saved;
}

void ctrl_state_start_autosave(void)
{
    lv_timer_create(ctrl_persist_tick, 1000, NULL);
}
