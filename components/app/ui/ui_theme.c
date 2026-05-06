#include "ui_theme.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "ui_theme";

#define NVS_NS  "cfg"
#define NVS_KEY "theme"

/* ==========================================================================
 * Runtime palette -- populated by ui_theme_init() from the preset selected
 * in NVS. All UI code reads these as plain uint32_t values.
 * ========================================================================== */
uint32_t UI_COLOR_BG;
uint32_t UI_COLOR_SHELL;
uint32_t UI_COLOR_SURFACE;
uint32_t UI_COLOR_SURFACE_HI;
uint32_t UI_COLOR_SURFACE_LO;
uint32_t UI_COLOR_STROKE;

uint32_t UI_COLOR_ACCENT;
uint32_t UI_COLOR_TAB;
uint32_t UI_COLOR_COOL;
uint32_t UI_COLOR_HOT;
uint32_t UI_COLOR_ON;
uint32_t UI_COLOR_WARN;

uint32_t UI_COLOR_TEXT;
uint32_t UI_COLOR_MUTED;
uint32_t UI_COLOR_DIM;

/* ==========================================================================
 *  Preset palettes
 *
 *  Each preset fills every slot in the runtime palette. Layout follows the
 *  order of the extern declarations in ui_theme.h.
 *
 *  Design notes per preset:
 *    - BG / SHELL / SURFACE_LO:  three darker-than-surface shades for the
 *      shell and recessed tracks.
 *    - SURFACE / SURFACE_HI:     panel body + hover/pressed state.
 *    - TAB:                      primary accent (active tab, sliders, logo
 *      recolor).
 *    - TEXT / MUTED / DIM:       readable text ramp on top of SURFACE.
 *    - ACCENT:                   CHINTAMANI red stays in every preset so
 *      brand marks keep their identity.
 *    - COOL / HOT:               climate-slider endpoints.
 *    - ON / WARN:                success-green + warning-amber analogues.
 * ========================================================================== */

typedef struct {
    const char *name;
    uint32_t bg, shell, surface, surface_hi, surface_lo, stroke;
    uint32_t accent, tab, cool, hot, on, warn;
    uint32_t text, muted, dim;
    /* Picker swatch: primary card colour + accent dot. */
    uint32_t swatch_primary, swatch_accent;
} preset_t;

static const preset_t s_presets[UI_THEME_COUNT] = {
    /* 0: BROWN LEATHER (default) */
    {
        .name = "Brown Leather",
        .bg = 0x1A0E06, .shell = 0x2B1A0F, .surface = 0x3F2818,
        .surface_hi = 0x5A3D26, .surface_lo = 0x1F130A, .stroke = 0x5E4025,
        .accent = 0xE30613, .tab = 0xD4A574, .cool = 0xEAD6B0,
        .hot = 0xC2411A, .on = 0x8FA455, .warn = 0xD4A036,
        .text = 0xF5E6CC, .muted = 0xB59A7A, .dim = 0x7A5E45,
        .swatch_primary = 0x3F2818, .swatch_accent = 0xD4A574,
    },
    /* 1: PURPLE */
    {
        .name = "Royal Purple",
        .bg = 0x120820, .shell = 0x1F0E38, .surface = 0x2E1552,
        .surface_hi = 0x432079, .surface_lo = 0x170A2A, .stroke = 0x56299E,
        .accent = 0xE30613, .tab = 0xC79CFF, .cool = 0xDFC9FF,
        .hot = 0xE05B8D, .on = 0x8FD47A, .warn = 0xE6B84A,
        .text = 0xF2E9FF, .muted = 0xB79DE0, .dim = 0x6F5A95,
        .swatch_primary = 0x2E1552, .swatch_accent = 0xC79CFF,
    },
    /* 2: RED */
    {
        .name = "Crimson Sports",
        .bg = 0x1A0606, .shell = 0x2B0B0B, .surface = 0x421414,
        .surface_hi = 0x611E1E, .surface_lo = 0x1E0808, .stroke = 0x6D2424,
        .accent = 0xFFCC33, .tab = 0xFF5D5D, .cool = 0xFFD2B8,
        .hot = 0xFF3B3B, .on = 0x95C36A, .warn = 0xF0B83A,
        .text = 0xFFECEC, .muted = 0xE0A6A6, .dim = 0x8A5555,
        .swatch_primary = 0x421414, .swatch_accent = 0xFF5D5D,
    },
    /* 3: WHITE + GOLD */
    {
        .name = "Ivory & Gold",
        .bg = 0xECE5D8, .shell = 0xDDD2BC, .surface = 0xF7F1E3,
        .surface_hi = 0xE7DCBF, .surface_lo = 0xDCD0B3, .stroke = 0xC6A96A,
        .accent = 0xE30613, .tab = 0xB8860B, .cool = 0xDDD2BC,
        .hot = 0xC76B2C, .on = 0x6B8E4E, .warn = 0xC9981A,
        .text = 0x2B1F0A, .muted = 0x6E5F3E, .dim = 0x9E8B63,
        .swatch_primary = 0xF7F1E3, .swatch_accent = 0xB8860B,
    },
    /* 4: BLACK + GOLD */
    {
        .name = "Obsidian & Gold",
        .bg = 0x070707, .shell = 0x121212, .surface = 0x1C1C1C,
        .surface_hi = 0x2A2A2A, .surface_lo = 0x0D0D0D, .stroke = 0x3A3018,
        .accent = 0xE30613, .tab = 0xD4AF37, .cool = 0xF2E6B0,
        .hot = 0xD46A1A, .on = 0x8FB256, .warn = 0xE3B93B,
        .text = 0xFFF2C8, .muted = 0xBDA86B, .dim = 0x6E5E34,
        .swatch_primary = 0x1C1C1C, .swatch_accent = 0xD4AF37,
    },
    /* 5: WHITE + BLACK */
    {
        .name = "Arctic Mono",
        .bg = 0xF2F2F2, .shell = 0xE6E6E6, .surface = 0xFFFFFF,
        .surface_hi = 0xEDEDED, .surface_lo = 0xDCDCDC, .stroke = 0x9E9E9E,
        .accent = 0xE30613, .tab = 0x1C1C1C, .cool = 0xE6E6E6,
        .hot = 0xC64420, .on = 0x4E8B4C, .warn = 0xD4A036,
        .text = 0x0A0A0A, .muted = 0x5A5A5A, .dim = 0xA0A0A0,
        .swatch_primary = 0xFFFFFF, .swatch_accent = 0x1C1C1C,
    },
    /* 6: BLACK (dark slate) */
    {
        .name = "Slate Black",
        .bg = 0x050505, .shell = 0x0E0E0E, .surface = 0x181818,
        .surface_hi = 0x262626, .surface_lo = 0x0A0A0A, .stroke = 0x353535,
        .accent = 0xE30613, .tab = 0xEAEAEA, .cool = 0xE6E6E6,
        .hot = 0xD45630, .on = 0x8FA455, .warn = 0xD4A036,
        .text = 0xF5F5F5, .muted = 0xA0A0A0, .dim = 0x5E5E5E,
        .swatch_primary = 0x181818, .swatch_accent = 0xEAEAEA,
    },
    /* 7: WHITE (arctic white) */
    {
        .name = "Arctic White",
        .bg = 0xEDEDED, .shell = 0xDADADA, .surface = 0xFAFAFA,
        .surface_hi = 0xE6E6E6, .surface_lo = 0xD0D0D0, .stroke = 0x9A9A9A,
        .accent = 0xE30613, .tab = 0x3A5E9A, .cool = 0xCEDBEE,
        .hot = 0xC84A22, .on = 0x6B9A4D, .warn = 0xC9981A,
        .text = 0x0F0F0F, .muted = 0x545454, .dim = 0x9E9E9E,
        .swatch_primary = 0xFAFAFA, .swatch_accent = 0x3A5E9A,
    },
    /* 8: GREEN + GOLD */
    {
        .name = "Forest & Gold",
        .bg = 0x061609, .shell = 0x0E2614, .surface = 0x18361F,
        .surface_hi = 0x255030, .surface_lo = 0x0A1C10, .stroke = 0x2E5A35,
        .accent = 0xE30613, .tab = 0xD4AF37, .cool = 0xEFE4B6,
        .hot = 0xC45C2A, .on = 0x95C36A, .warn = 0xE3B93B,
        .text = 0xF0EAC8, .muted = 0xAFC4A0, .dim = 0x5E7A5E,
        .swatch_primary = 0x18361F, .swatch_accent = 0xD4AF37,
    },
    /* 9: SILVER + GOLD */
    {
        .name = "Silver & Gold",
        .bg = 0x1A1A1C, .shell = 0x292A2E, .surface = 0x3B3D42,
        .surface_hi = 0x55585E, .surface_lo = 0x202124, .stroke = 0x7A7C83,
        .accent = 0xE30613, .tab = 0xD4AF37, .cool = 0xE6E9EE,
        .hot = 0xD0652C, .on = 0x90B56A, .warn = 0xE3B93B,
        .text = 0xEDEEF2, .muted = 0xB7BAC2, .dim = 0x7C7F88,
        .swatch_primary = 0x3B3D42, .swatch_accent = 0xD4AF37,
    },
};

static ui_theme_id_t s_active_id = UI_THEME_BROWN;
static bool          s_nvs_ready = false;

static esp_err_t ensure_nvs(void)
{
    if (s_nvs_ready) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs erase+reinit");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) s_nvs_ready = true;
    else ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    return err;
}

static void apply_preset(ui_theme_id_t id)
{
    if (id >= UI_THEME_COUNT) id = UI_THEME_BROWN;
    const preset_t *p = &s_presets[id];
    UI_COLOR_BG          = p->bg;
    UI_COLOR_SHELL       = p->shell;
    UI_COLOR_SURFACE     = p->surface;
    UI_COLOR_SURFACE_HI  = p->surface_hi;
    UI_COLOR_SURFACE_LO  = p->surface_lo;
    UI_COLOR_STROKE      = p->stroke;
    UI_COLOR_ACCENT      = p->accent;
    UI_COLOR_TAB         = p->tab;
    UI_COLOR_COOL        = p->cool;
    UI_COLOR_HOT         = p->hot;
    UI_COLOR_ON          = p->on;
    UI_COLOR_WARN        = p->warn;
    UI_COLOR_TEXT        = p->text;
    UI_COLOR_MUTED       = p->muted;
    UI_COLOR_DIM         = p->dim;
    s_active_id = id;
}

void ui_theme_init(void)
{
    /* Default applied immediately in case NVS access fails -- this
     * guarantees UI code always sees a non-zero palette. */
    apply_preset(UI_THEME_BROWN);

    if (ensure_nvs() != ESP_OK) {
        ESP_LOGW(TAG, "nvs not ready, using default theme");
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved theme, using default (%s)",
                 s_presets[s_active_id].name);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ro: %s", esp_err_to_name(err));
        return;
    }
    uint8_t raw = UI_THEME_BROWN;
    err = nvs_get_u8(h, NVS_KEY, &raw);
    nvs_close(h);
    if (err == ESP_OK && raw < UI_THEME_COUNT) {
        apply_preset((ui_theme_id_t)raw);
        ESP_LOGI(TAG, "loaded theme=%u (%s)",
                 (unsigned)raw, s_presets[s_active_id].name);
    } else {
        ESP_LOGI(TAG, "theme key missing/invalid, keeping default");
    }
}

ui_theme_id_t ui_theme_get(void) { return s_active_id; }

void ui_theme_save(ui_theme_id_t id)
{
    if (id >= UI_THEME_COUNT) return;
    if (ensure_nvs() != ESP_OK) return;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, NVS_KEY, (uint8_t)id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved theme=%u (%s)", (unsigned)id, s_presets[id].name);
    } else {
        ESP_LOGE(TAG, "nvs_set_u8: %s", esp_err_to_name(err));
    }
}

const char *ui_theme_name(ui_theme_id_t id)
{
    if (id >= UI_THEME_COUNT) return "";
    return s_presets[id].name;
}

void ui_theme_swatch(ui_theme_id_t id, uint32_t *primary, uint32_t *accent)
{
    if (id >= UI_THEME_COUNT) id = UI_THEME_BROWN;
    if (primary) *primary = s_presets[id].swatch_primary;
    if (accent)  *accent  = s_presets[id].swatch_accent;
}
