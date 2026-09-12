#include "ctrl_state.h"
#include "ac_state.h"
#include "lvgl.h"
#include "nvs.h"
#include "modbus_task.h"
#include "hmi_role.h"
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
/* FAN ONLY lives under its own key rather than inside the zones blob:
 * adding a field to that blob means bumping CTRL_BLOB_VER, which would
 * discard the RGB settings already saved on every van in the field. */
#define CTRL_NVS_FANONLY "ac_fanonly"
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

/* FAN ONLY is a commissioning choice (this van has no compressor on the
 * relay board), not a daily control, so it must survive a power cycle.
 * It is a single bool and never dragged like a slider, so it needs no
 * debounce -- write it the moment it differs from what is stored. */
static uint8_t s_saved_fan_only = 0xFF;   /* 0xFF = not loaded yet */

static void persist_fan_only(void)
{
    uint8_t now = ac_state()->fan_only ? 1 : 0;
    if (now == s_saved_fan_only) return;

    nvs_handle_t h;
    if (nvs_open(CTRL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, CTRL_NVS_FANONLY, now) == ESP_OK &&
        nvs_commit(h) == ESP_OK) {
        s_saved_fan_only = now;
        ESP_LOGI(TAG, "fan-only saved (%s)", now ? "on" : "off");
    }
    nvs_close(h);
}

static void ctrl_persist_tick(lv_timer_t *t)
{
    (void)t;
    persist_fan_only();

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
        uint8_t fo = 0;
        if (nvs_get_u8(h, CTRL_NVS_FANONLY, &fo) == ESP_OK) {
            ac_state()->fan_only = (fo != 0);
            ESP_LOGI(TAG, "restored fan-only (%s)", fo ? "on" : "off");
        }
        s_saved_fan_only = ac_state()->fan_only ? 1 : 0;
        nvs_close(h);
    }

    /* Seed both baselines from whatever we ended up with, so an
     * unchanged boot never triggers a redundant write. */
    ctrl_snapshot(&s_saved);
    s_seen = s_saved;
}

/* Defined further down with the rest of the momentary-hold code. */
static lv_timer_t *s_hold_deadman;
static void hold_deadman_cb(lv_timer_t *t);

void ctrl_state_start_autosave(void)
{
    lv_timer_create(ctrl_persist_tick, 1000, NULL);
    /* Watches coils held down by a secondary over ESP-NOW. Harmless on
     * a secondary (s_remote_coil stays unset there). */
    if (!s_hold_deadman) {
        s_hold_deadman = lv_timer_create(hold_deadman_cb, 100, NULL);
    }
}


/* ------------------------------------------------------------------
 *  Driving tiles
 * ------------------------------------------------------------------ */

void ctrl_toggle_drive(ctrl_toggle_t *item, bool want)
{
    if (!item) return;
    if (item->coil != CTRL_COIL_NONE) {
        /* Leave item->on alone until the relay board confirms on the
         * next read-back; the caller paints from its own preview and
         * the amber pending border shows the write is in flight. */
        item->pending = true;
        modbus_task_request(item->coil, want);
    } else {
        item->on = want;   /* unmapped tile -- UI-only */
    }
}

/* ------------------------------------------------------------------
 *  Momentary hold
 *
 *  One finger, one held tile. See ctrl_state.h for why a wireless
 *  secondary needs a repeat + deadman rather than a plain on/off pair.
 * ------------------------------------------------------------------ */

static ctrl_toggle_t *s_hold_item;        /* locally held tile, if any  */
static lv_timer_t    *s_hold_repeat;      /* secondary: re-assert timer */
static uint16_t       s_remote_coil = CTRL_COIL_NONE;
static uint32_t       s_remote_last_ms;

static ctrl_toggle_t *find_by_coil(uint16_t coil)
{
    if (coil == CTRL_COIL_NONE) return NULL;
    for (int k = 0; k < 2; k++) {
        int n = 0;
        ctrl_toggle_t *arr = (k == 0) ? ctrl_reading_lights(&n)
                                      : ctrl_switches(&n);
        for (int i = 0; i < n; i++) {
            if (arr[i].coil == coil) return &arr[i];
        }
    }
    return NULL;
}

static void hold_repeat_cb(lv_timer_t *t)
{
    (void)t;
    if (s_hold_item) ctrl_toggle_drive(s_hold_item, true);
}

void ctrl_momentary_hold_begin(ctrl_toggle_t *item)
{
    if (!item) return;
    if (s_hold_item && s_hold_item != item) ctrl_momentary_hold_end();
    if (s_hold_item == item) return;

    s_hold_item  = item;
    item->held   = true;
    ctrl_toggle_drive(item, true);

    /* Only a wireless/wired secondary needs the re-assert: its press
     * travelled as an intent that may not have arrived. */
    if (hmi_role_is_secondary() && !s_hold_repeat) {
        s_hold_repeat = lv_timer_create(hold_repeat_cb,
                                        CTRL_HOLD_REFRESH_MS, NULL);
    }
}

void ctrl_momentary_hold_end(void)
{
    if (s_hold_repeat) {
        lv_timer_del(s_hold_repeat);
        s_hold_repeat = NULL;
    }
    if (!s_hold_item) return;
    ctrl_toggle_t *it = s_hold_item;
    s_hold_item = NULL;
    it->held    = false;
    ctrl_toggle_drive(it, false);
}

static void hold_deadman_cb(lv_timer_t *t)
{
    (void)t;
    if (s_remote_coil == CTRL_COIL_NONE) return;
    uint32_t age = lv_tick_elaps(s_remote_last_ms);
    if (age < CTRL_HOLD_DEADMAN_MS) return;

    ctrl_toggle_t *it = find_by_coil(s_remote_coil);
    ESP_LOGW(TAG, "momentary coil %u released: no refresh for %u ms",
             (unsigned)s_remote_coil, (unsigned)age);
    s_remote_coil = CTRL_COIL_NONE;
    if (it) {
        it->held = false;
        ctrl_toggle_drive(it, false);
    }
}

bool ctrl_momentary_remote(uint16_t coil, bool on)
{
    ctrl_toggle_t *it = find_by_coil(coil);
    if (!it || !it->momentary) return false;   /* not ours -- normal path */

    if (on) {
        if (s_remote_coil != coil) {
            /* A different coil was held; drop it before taking this one. */
            ctrl_toggle_t *prev = find_by_coil(s_remote_coil);
            if (prev) { prev->held = false; ctrl_toggle_drive(prev, false); }
            s_remote_coil = coil;
            it->held      = true;
            ctrl_toggle_drive(it, true);
        }
        /* Refresh either way -- this is what keeps the deadman fed. */
        s_remote_last_ms = lv_tick_get();
    } else {
        if (s_remote_coil == coil) s_remote_coil = CTRL_COIL_NONE;
        it->held = false;
        ctrl_toggle_drive(it, false);
    }
    return true;
}

void ctrl_momentary_release_all(void)
{
    ctrl_momentary_hold_end();
    for (int k = 0; k < 2; k++) {
        int n = 0;
        ctrl_toggle_t *arr = (k == 0) ? ctrl_reading_lights(&n)
                                      : ctrl_switches(&n);
        for (int i = 0; i < n; i++) {
            if (!arr[i].momentary || !arr[i].held) continue;
            arr[i].held = false;
            ESP_LOGW(TAG, "momentary \"%s\" released by cleanup", arr[i].name);
            ctrl_toggle_drive(&arr[i], false);
        }
    }
}
