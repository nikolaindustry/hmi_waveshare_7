#include "ui_tabs.h"
#include "ui_theme.h"
#include "ctrl_state.h"
#include "modbus_task.h"
#include "hmi_role.h"
#include "hmi_sync.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_tab_control";

/* ========================================================================
 *  CONTROL TAB
 *
 *  Master-detail layout inside the 800 x 368 content area:
 *
 *  +----------------+--------------------------------------------------+
 *  |  NAV LIST      |           DETAIL PANEL                           |
 *  |  (220 x 336)   |  (536 x 336, rebuilt on selection)               |
 *  |                |                                                  |
 *  |  Reading Lts>  |   < currently selected feature renders here >    |
 *  |  Switches      |                                                  |
 *  |  RGB Roof      |                                                  |
 *  |  RGB Floor     |                                                  |
 *  |  Star Roof     |                                                  |
 *  +----------------+--------------------------------------------------+
 * ======================================================================== */

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

/* ---------- forward-declared detail builders ---------- */
static void detail_rgb_roof (lv_obj_t *p);
static void detail_rgb_floor(lv_obj_t *p);
static void detail_reading  (lv_obj_t *p);
static void detail_star     (lv_obj_t *p);
static void detail_switches (lv_obj_t *p);

typedef void (*ctrl_detail_fn_t)(lv_obj_t *parent);

typedef struct {
    const char       *icon;
    const char       *title;
    ctrl_detail_fn_t  build;
} ctrl_entry_t;

static const ctrl_entry_t s_entries[] = {
    { LV_SYMBOL_EYE_OPEN, "Reading Lights", detail_reading   },
    { LV_SYMBOL_POWER,    "Switches",       detail_switches  },
    { LV_SYMBOL_HOME,     "RGB Roof",       detail_rgb_roof  },
    { LV_SYMBOL_DOWNLOAD, "RGB Floor",      detail_rgb_floor },
    { LV_SYMBOL_BELL,     "Star Roof",      detail_star      },
};
#define N_ENTRIES ((int)(sizeof(s_entries) / sizeof(s_entries[0])))

/* ---------- master-detail handles ---------- */
static lv_obj_t *s_list_rows[N_ENTRIES];
static lv_obj_t *s_detail;
static int       s_active = -1;

/* =======================================================================
 *  Shared helpers
 * ======================================================================= */

/* Card panel with the app's standard surface treatment. */
static lv_obj_t *panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, UI_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, hex(UI_COLOR_STROKE), 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/* Repaint a single nav-list row to reflect active/inactive state. */
static void paint_list_row(int idx, bool active)
{
    lv_obj_t *row = s_list_rows[idx];
    if (!row) return;
    lv_obj_set_style_bg_color(row,
        hex(active ? UI_COLOR_SURFACE_HI : UI_COLOR_SURFACE_LO), 0);
    /* a 3 px accent strip on the left lights up when active */
    lv_obj_t *strip = lv_obj_get_child(row, 0);
    if (strip) {
        lv_obj_set_style_bg_color(strip,
            hex(active ? UI_COLOR_TAB : UI_COLOR_SURFACE_LO), 0);
    }
    /* icon + title text colour */
    lv_obj_t *icon  = lv_obj_get_child(row, 1);
    lv_obj_t *title = lv_obj_get_child(row, 2);
    if (icon)  lv_obj_set_style_text_color(icon,
        hex(active ? UI_COLOR_TAB : UI_COLOR_MUTED), 0);
    if (title) lv_obj_set_style_text_color(title,
        hex(active ? UI_COLOR_TEXT : UI_COLOR_MUTED), 0);
}

static void switch_detail(int idx)
{
    if (idx < 0 || idx >= N_ENTRIES || idx == s_active) return;
    ESP_LOGW(TAG, ">>> NAV switch_detail enter: from=%d (%s) to=%d (%s) free_heap=%u min_free=%u",
             s_active,
             s_active >= 0 ? s_entries[s_active].title : "(none)",
             idx, s_entries[idx].title,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    if (s_active >= 0) paint_list_row(s_active, false);
    s_active = idx;
    paint_list_row(idx, true);
    ESP_LOGW(TAG, "NAV calling lv_obj_clean(s_detail=%p)", (void*)s_detail);
    lv_obj_clean(s_detail);
    ESP_LOGW(TAG, "NAV after clean: free_heap=%u",
             (unsigned)esp_get_free_heap_size());
    if (s_entries[idx].build) {
        ESP_LOGW(TAG, "NAV calling builder for '%s'", s_entries[idx].title);
        s_entries[idx].build(s_detail);
        ESP_LOGW(TAG, "NAV builder returned for '%s'", s_entries[idx].title);
    }
    ESP_LOGW(TAG, "<<< NAV switch_detail done: now showing '%s' free_heap=%u",
             s_entries[idx].title, (unsigned)esp_get_free_heap_size());
    ESP_LOGI(TAG, "detail -> %s", s_entries[idx].title);
}

static void on_list_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch_detail(idx);
}

/* Build the left-hand nav list.  Each row: 220 x 60, vertically stacked. */
static void build_nav_list(lv_obj_t *content)
{
    lv_obj_t *col = panel(content, 16, 16, 220, 336);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);

    for (int i = 0; i < N_ENTRIES; i++) {
        lv_obj_t *row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 204, 56);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, hex(UI_COLOR_SURFACE_LO), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_list_row_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_list_rows[i] = row;

        /* child 0: left accent strip */
        lv_obj_t *strip = lv_obj_create(row);
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, 3, 32);
        lv_obj_align(strip, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_set_style_bg_color(strip, hex(UI_COLOR_SURFACE_LO), 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(strip, 2, 0);

        /* child 1: icon */
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, s_entries[i].icon);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(icon, hex(UI_COLOR_MUTED), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);

        /* child 2: title */
        lv_obj_t *title = lv_label_create(row);
        lv_label_set_text(title, s_entries[i].title);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(title, hex(UI_COLOR_MUTED), 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 52, 0);
    }
}

/* =======================================================================
 *  Detail views
 * ======================================================================= */

/* ---------- small helpers shared by detail views ---------- */

static lv_obj_t *detail_header(lv_obj_t *p, const char *title)
{
    lv_obj_t *t = lv_label_create(p);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_set_pos(t, 20, 16);
    return t;
}

/* Apply the brown-leather palette to an lv_switch -- otherwise LVGL's
 * default theme paints the knob and the checked indicator in sky blue,
 * which clashes with the warm interior. Sets:
 *   MAIN       - track OFF colour (deep-grain)
 *   INDICATOR  - track ON colour  (brass tan)
 *   KNOB       - cream knob with subtle brass glow so it reads on
 *                both the light and the dark track. */
static void style_theme_switch(lv_obj_t *sw)
{
    /* Unchecked track. */
    lv_obj_set_style_bg_color(sw, hex(UI_COLOR_SURFACE_LO), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, hex(UI_COLOR_STROKE), LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);

    /* Checked track (overlaid on top of MAIN when LV_STATE_CHECKED). */
    lv_obj_set_style_bg_color(sw, hex(UI_COLOR_TAB),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* Knob -- cream ball with a warm brass shadow. Same in both states. */
    lv_obj_set_style_bg_color(sw, hex(UI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(sw, hex(UI_COLOR_TAB), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sw, LV_OPA_40, LV_PART_KNOB);
}

static void power_pill_paint(lv_obj_t *pill, lv_obj_t *lbl, bool on)
{
    lv_obj_set_style_bg_color(pill, hex(on ? UI_COLOR_ON : UI_COLOR_SURFACE_HI), 0);
    lv_label_set_text(lbl, on ? "ON" : "OFF");
}

/* Generic power pill used by RGB and Star detail views.  Uses a
 * user-supplied bool pointer + optional post-toggle callback. */
typedef struct {
    bool     *target;
    void    (*after)(void);   /* optional */
    lv_obj_t *pill;
    lv_obj_t *label;
} power_pill_ctx_t;

static void on_power_pill(lv_event_t *e)
{
    power_pill_ctx_t *ctx = (power_pill_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->target) return;
    *ctx->target = !*ctx->target;
    power_pill_paint(ctx->pill, ctx->label, *ctx->target);
    if (ctx->after) ctx->after();
}

static lv_obj_t *build_power_pill(lv_obj_t *parent, power_pill_ctx_t *ctx,
                                  lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 72, 34);
    lv_obj_set_pos(pill, x, y);
    lv_obj_set_style_radius(pill, 17, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pill, on_power_pill, LV_EVENT_CLICKED, ctx);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, hex(UI_COLOR_TEXT), 0);
    lv_obj_center(lbl);

    ctx->pill  = pill;
    ctx->label = lbl;
    power_pill_paint(pill, lbl, ctx->target ? *ctx->target : false);
    return pill;
}

/* =======================================================================
 *  RGB detail view (shared between RGB Roof and RGB Floor)
 * ======================================================================= */

static lv_obj_t         *s_rgb_wheel;
static lv_obj_t         *s_rgb_brt_slider;
static lv_obj_t         *s_rgb_brt_value;
static ctrl_rgb_t       *s_rgb_target;
static power_pill_ctx_t  s_rgb_pp;
static lv_timer_t       *s_rgb_timer;
static uint32_t          s_rgb_last_seq;

/* Defined below build_rgb_detail(), which installs them. */
static void rgb_sync_cb(lv_timer_t *t);
static void rgb_destroy_cb(lv_event_t *e);

/* Modbus routing for each RGB zone. Both zones now live on the same
 * ESP32 RGB slave (0x20); they differ only by their register-block
 * offset. The slave's register map is:
 *    regs 0..5  = ROOF  (R, G, B, MODE, SPEED, BRIGHTNESS)
 *    regs 6..11 = FLOOR (same layout)
 * Setting slave=0 disables the Modbus push for that zone (UI still
 * works locally), useful if the floor strip is temporarily unwired. */
#define RGB_SLAVE_ROOF        0x20
#define RGB_START_REG_ROOF    0
#define RGB_SLAVE_FLOOR       0x20
#define RGB_START_REG_FLOOR   6
static uint8_t  s_rgb_slave;
static uint16_t s_rgb_start_reg;

/* HSV (h: 0..359, s/v: 0..100) -> RGB (0..255). Standard six-sector
 * algorithm, integer math. Used to convert what the UI model stores
 * into the raw channel bytes the Modbus slave expects. */
static void hsv_to_rgb_u8(uint16_t h, uint8_t s, uint8_t v,
                          uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (h >= 360) h %= 360;
    if (s > 100)  s = 100;
    if (v > 100)  v = 100;

    uint32_t V = (uint32_t)v * 255u / 100u;   /* 0..255 */
    if (s == 0) { *r = *g = *b = (uint8_t)V; return; }

    uint32_t S  = (uint32_t)s * 255u / 100u;                 /* 0..255 */
    uint32_t hi = h / 60;                                    /* sector 0..5 */
    uint32_t f  = ((uint32_t)h - hi * 60) * 255u / 60u;      /* 0..255 in sector */
    uint32_t p  = V * (255u - S)                         / 255u;
    uint32_t q  = V * (255u - (S * f)            / 255u) / 255u;
    uint32_t t  = V * (255u - (S * (255u - f))   / 255u) / 255u;

    uint8_t R = 0, G = 0, B = 0;
    switch (hi) {
        case 0: R = V; G = t; B = p; break;
        case 1: R = q; G = V; B = p; break;
        case 2: R = p; G = V; B = t; break;
        case 3: R = p; G = q; B = V; break;
        case 4: R = t; G = p; B = V; break;
        default:R = V; G = p; B = q; break;   /* sector 5 */
    }
    *r = R; *g = G; *b = B;
}

/* Take the current zone state, convert HSV -> RGB, and push it onto
 * the Modbus RGB queue. No-op when this zone has no slave mapped yet.
 * Called from every UI interaction: colour wheel drag, brightness
 * slider move, and the power pill toggle.
 *
 * On the SECONDARY HMI we never drive the bus directly -- the primary
 * owns the master role. Instead we push an RGB intent onto the ring
 * ring buffer; the primary polls it over FC 0x03 and replays it locally,
 * which is the one place that actually calls modbus_task_request_rgb(). */
static void push_rgb_to_slave(void)
{
    if (!s_rgb_target) return;

    if (hmi_role_is_secondary()) {
        uint16_t zone = (s_rgb_target == ctrl_rgb_roof()) ? 0u : 1u;
        uint16_t a2   = ((uint16_t)(s_rgb_target->on ? 1u : 0u) << 15)
                      | ((uint16_t)s_rgb_target->sat        << 8)
                      |  (uint16_t)s_rgb_target->brightness;
        hmi_sync_push_intent(HMI_CMD_RGB_SET, zone, s_rgb_target->hue, a2);
        return;
    }

    if (s_rgb_slave == 0) return;
    uint8_t r, g, b;
    hsv_to_rgb_u8(s_rgb_target->hue, s_rgb_target->sat,
                  s_rgb_target->brightness, &r, &g, &b);
    modbus_task_request_rgb(s_rgb_slave, s_rgb_start_reg,
                            r, g, b, s_rgb_target->on);
}

static void rgb_brt_label_refresh(void)
{
    if (!s_rgb_brt_value || !s_rgb_target) return;
    char buf[12];
    snprintf(buf, sizeof(buf), "%u %%", (unsigned)s_rgb_target->brightness);
    lv_label_set_text(s_rgb_brt_value, buf);
}

static void on_rgb_wheel_change(lv_event_t *e)
{
    (void)e;
    if (!s_rgb_target || !s_rgb_wheel) return;
    lv_color_hsv_t hsv = lv_colorwheel_get_hsv(s_rgb_wheel);
    s_rgb_target->hue = hsv.h;
    s_rgb_target->sat = hsv.s;
    push_rgb_to_slave();
}

static void on_rgb_brt_change(lv_event_t *e)
{
    (void)e;
    if (!s_rgb_target || !s_rgb_brt_slider) return;
    s_rgb_target->brightness = (uint8_t)lv_slider_get_value(s_rgb_brt_slider);
    rgb_brt_label_refresh();
    push_rgb_to_slave();
}

/* Fires after the power pill flips zone->on. Push the new state so
 * the slave instantly goes dark / lights back up without waiting for
 * another wheel or slider event. */
static void on_rgb_power_changed(void)
{
    push_rgb_to_slave();
}

static void build_rgb_detail(lv_obj_t *p, ctrl_rgb_t *zone, const char *title)
{
    s_rgb_target = zone;
    /* Map zone -> (slave, start_reg). Compared against &s_rgb_roof etc
     * so there is no ambiguity about which zone this builder was
     * invoked for. */
    if (zone == ctrl_rgb_roof()) {
        s_rgb_slave     = RGB_SLAVE_ROOF;
        s_rgb_start_reg = RGB_START_REG_ROOF;
    } else if (zone == ctrl_rgb_floor()) {
        s_rgb_slave     = RGB_SLAVE_FLOOR;
        s_rgb_start_reg = RGB_START_REG_FLOOR;
    } else {
        s_rgb_slave     = 0;
        s_rgb_start_reg = 0;
    }

    detail_header(p, title);

    /* power pill top-right */
    s_rgb_pp = (power_pill_ctx_t){
        .target = &zone->on,
        .after  = on_rgb_power_changed,
    };
    build_power_pill(p, &s_rgb_pp, 444, 14);

    /* colorwheel on the left */
    s_rgb_wheel = lv_colorwheel_create(p, true);
    lv_obj_set_size(s_rgb_wheel, 220, 220);
    lv_obj_set_pos(s_rgb_wheel, 20, 68);
    lv_color_hsv_t hsv = { .h = zone->hue, .s = zone->sat, .v = 100 };
    lv_colorwheel_set_hsv(s_rgb_wheel, hsv);
    lv_obj_add_event_cb(s_rgb_wheel, on_rgb_wheel_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* brightness block on the right */
    lv_obj_t *blbl = lv_label_create(p);
    lv_label_set_text(blbl, "BRIGHTNESS");
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(blbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(blbl, 3, 0);
    lv_obj_set_pos(blbl, 268, 80);

    s_rgb_brt_value = lv_label_create(p);
    lv_obj_set_style_text_font(s_rgb_brt_value, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_rgb_brt_value, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_rgb_brt_value, 268, 100);
    rgb_brt_label_refresh();

    s_rgb_brt_slider = lv_slider_create(p);
    lv_obj_set_size(s_rgb_brt_slider, 236, 8);
    lv_obj_set_pos(s_rgb_brt_slider, 268, 170);
    lv_slider_set_range(s_rgb_brt_slider, 0, 100);
    lv_slider_set_value(s_rgb_brt_slider, zone->brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_rgb_brt_slider, hex(UI_COLOR_SURFACE_LO), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rgb_brt_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_rgb_brt_slider, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_rgb_brt_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_rgb_brt_slider, hex(UI_COLOR_TAB), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_rgb_brt_slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_rgb_brt_slider, hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_rgb_brt_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_rgb_brt_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s_rgb_brt_slider, hex(UI_COLOR_TAB), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s_rgb_brt_slider, LV_OPA_40, LV_PART_KNOB);
    lv_obj_add_event_cb(s_rgb_brt_slider, on_rgb_brt_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* subtle hint about colorwheel mode switch */
    lv_obj_t *hint = lv_label_create(p);
    lv_label_set_text(hint, "Tap center to switch: HUE / SAT / VALUE");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(hint, 20, 298);

    /* Live-refresh from remote changes (other HMI / wireless / cloud). */
    s_rgb_last_seq = modbus_task_state_seq();
    s_rgb_timer    = lv_timer_create(rgb_sync_cb, 200, NULL);
    lv_obj_add_event_cb(s_rgb_wheel, rgb_destroy_cb, LV_EVENT_DELETE, NULL);
}

/* Repaint the RGB controls when the zone changed somewhere else -- the
 * other HMI, the wireless unit, or the cloud app. Without this the
 * screen only picked up remote changes when it was rebuilt, i.e. by
 * navigating away and back. */
static void rgb_sync_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_rgb_target) return;

    uint32_t seq = modbus_task_state_seq();
    if (seq == s_rgb_last_seq) return;

    /* Never yank a control out from under the user's finger. Leave
     * s_rgb_last_seq alone so the update is applied on a later tick,
     * once they let go, rather than being dropped. */
    if (s_rgb_wheel && lv_obj_has_state(s_rgb_wheel, LV_STATE_PRESSED)) return;
    if (s_rgb_brt_slider && lv_obj_has_state(s_rgb_brt_slider, LV_STATE_PRESSED)) return;

    s_rgb_last_seq = seq;

    if (s_rgb_wheel) {
        lv_color_hsv_t hsv = { .h = s_rgb_target->hue,
                               .s = s_rgb_target->sat,
                               .v = 100 };
        lv_colorwheel_set_hsv(s_rgb_wheel, hsv);
    }
    if (s_rgb_brt_slider) {
        lv_slider_set_value(s_rgb_brt_slider, s_rgb_target->brightness, LV_ANIM_OFF);
    }
    rgb_brt_label_refresh();
    if (s_rgb_pp.pill) {
        power_pill_paint(s_rgb_pp.pill, s_rgb_pp.label, s_rgb_target->on);
    }
}

/* s_detail is cleared (lv_obj_clean) rather than deleted between
 * sections, so hook the teardown on a child widget -- same approach as
 * the toggle-list timer. */
static void rgb_destroy_cb(lv_event_t *e)
{
    (void)e;
    if (s_rgb_timer) { lv_timer_del(s_rgb_timer); s_rgb_timer = NULL; }
    s_rgb_wheel      = NULL;
    s_rgb_brt_slider = NULL;
    s_rgb_brt_value  = NULL;
    s_rgb_target     = NULL;
    s_rgb_pp.pill    = NULL;
    s_rgb_pp.label   = NULL;
}

static void detail_rgb_roof(lv_obj_t *p)  { build_rgb_detail(p, ctrl_rgb_roof(),  "RGB Roof");  }
static void detail_rgb_floor(lv_obj_t *p) { build_rgb_detail(p, ctrl_rgb_floor(), "RGB Floor"); }

/* =======================================================================
 *  Toggle-list detail view (Reading Lights, Switches)
 *
 *  One tile per ctrl_toggle_t. The entire tile is tap-to-toggle:
 *   - OFF: dim surface, muted text, no glow, dark LED dot.
 *   - ON : accented surface, bright text, outer glow halo, lit LED dot.
 *   - Pending (Modbus in flight): amber border, preview state shown so
 *     the user gets instant feedback while the relay is being written.
 *  No explicit lv_switch widget -- the tile IS the visualisation.
 * ======================================================================= */

/* State shared between the detail builder and the 200 ms sync timer.
 * These get reset each time the detail panel is rebuilt. */
static lv_obj_t *s_tl_tiles[16];
static lv_obj_t *s_tl_leds  [16];   /* small circle indicator on each tile */
static lv_obj_t *s_tl_names [16];   /* name label -- we repaint its colour */
static bool      s_tl_preview[16];  /* what the user last tapped to; shown
                                       while item->pending is true so the
                                       tile reflects the desired state */
static ctrl_toggle_t *s_tl_items;
static int        s_tl_count;
static lv_timer_t *s_tl_timer;
static lv_obj_t  *s_link_pill;
static lv_obj_t  *s_link_pill_lbl;
static lv_obj_t  *s_master_sw;        /* master "All ..." switch, NULL on unmapped tabs */
static uint32_t   s_tl_last_seq;
static bool       s_tl_last_online;

/* Paint the whole tile to reflect confirmed/preview state.
 * The tile itself carries the glow; the LED dot is a secondary cue that
 * reads at a glance even when the tile is partially scrolled off-screen. */
static void paint_tile_state(int i)
{
    if (i < 0 || i >= s_tl_count || !s_tl_items) return;
    lv_obj_t *tile = s_tl_tiles[i];
    if (!tile) return;
    const ctrl_toggle_t *it = &s_tl_items[i];

    /* While a Modbus write is pending, show the user's intended state
     * so the visual responds instantly; once the worker confirms and
     * bumps state_seq, the sync timer repaints with the real flag. */
    bool show_on = it->pending ? s_tl_preview[i] : it->on;

    /* Tile background: accent-tinted surface when ON, neutral when OFF. */
    lv_obj_set_style_bg_color(tile,
        hex(show_on ? UI_COLOR_SURFACE_HI : UI_COLOR_SURFACE_LO), 0);

    /* Border colour tracks three states: pending/confirmed-on/confirmed-off. */
    uint32_t border;
    if (it->pending)   border = UI_COLOR_WARN;    /* amber while in flight */
    else if (show_on)  border = UI_COLOR_TAB;     /* confirmed ON */
    else               border = UI_COLOR_STROKE;  /* confirmed OFF */
    lv_obj_set_style_border_color(tile, hex(border), 0);
    lv_obj_set_style_border_width(tile, 2, 0);

    /* Outer glow halo -- LVGL shadow bleeds outside the object's bounds
     * which is exactly the "button is lit" look we want. Zero opacity
     * when OFF removes the halo completely. */
    lv_obj_set_style_shadow_color(tile, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_shadow_width(tile, show_on ? 24 : 0, 0);
    lv_obj_set_style_shadow_spread(tile, show_on ? 1 : 0, 0);
    lv_obj_set_style_shadow_opa(tile, show_on ? LV_OPA_60 : LV_OPA_TRANSP, 0);

    /* Name label: dims to muted when OFF for extra contrast. */
    lv_obj_t *name = s_tl_names[i];
    if (name) lv_obj_set_style_text_color(name,
        hex(show_on ? UI_COLOR_TEXT : UI_COLOR_MUTED), 0);

    /* LED dot. Accent-coloured with its own local glow when ON,
     * dead-stroke grey when OFF. */
    lv_obj_t *led = s_tl_leds[i];
    if (led) {
        lv_obj_set_style_bg_color(led,
            hex(show_on ? UI_COLOR_TAB : UI_COLOR_STROKE), 0);
        lv_obj_set_style_shadow_color(led, hex(UI_COLOR_TAB), 0);
        lv_obj_set_style_shadow_width(led, show_on ? 14 : 0, 0);
        lv_obj_set_style_shadow_opa(led,
            show_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void paint_link_pill(bool online)
{
    if (!s_link_pill || !s_link_pill_lbl) return;
    lv_obj_set_style_bg_color(s_link_pill,
        hex(online ? UI_COLOR_ON : UI_COLOR_HOT), 0);
    lv_label_set_text(s_link_pill_lbl, online ? "ONLINE" : "OFFLINE");
}

/* True iff every mapped (coil != NONE) item is currently ON. Unmapped
 * items don't count -- master bar is only created for mapped tabs. */
static bool tl_all_mapped_on(void)
{
    if (!s_tl_items) return false;
    bool any = false;
    for (int i = 0; i < s_tl_count; i++) {
        if (s_tl_items[i].coil == CTRL_COIL_NONE) continue;
        any = true;
        if (!s_tl_items[i].on) return false;
    }
    return any;
}

/* Fires 5x per second while the toggle-list detail is visible, so
 * changes from the Modbus worker (confirmations + external relay flips)
 * get reflected in the UI without the worker ever touching LVGL. */
static void tl_sync_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_tl_items || s_tl_count == 0) {
        ESP_LOGD(TAG, "TL sync_cb skipped: items=%p count=%d", (void*)s_tl_items, s_tl_count);
        return;
    }

    bool online = modbus_task_link_online();
    if (online != s_tl_last_online) {
        paint_link_pill(online);
        s_tl_last_online = online;
    }

    uint32_t seq = modbus_task_state_seq();
    if (seq == s_tl_last_seq) return;
    s_tl_last_seq = seq;

    for (int i = 0; i < s_tl_count; i++) {
        if (!s_tl_tiles[i]) continue;
        /* Sync timer fires whenever state_seq changes -- that means a
         * confirmation landed, so the pending preview is now irrelevant.
         * paint_tile_state() already handles pending vs confirmed. */
        paint_tile_state(i);
    }

    /* Keep master "All ..." switch in sync with the aggregate: ON iff
     * every mapped coil is confirmed ON. */
    if (s_master_sw) {
        bool all_on = tl_all_mapped_on();
        bool m_checked = lv_obj_has_state(s_master_sw, LV_STATE_CHECKED);
        if (m_checked != all_on) {
            if (all_on) lv_obj_add_state(s_master_sw, LV_STATE_CHECKED);
            else        lv_obj_clear_state(s_master_sw, LV_STATE_CHECKED);
        }
    }
}

static void tl_destroy_cb(lv_event_t *e)
{
    lv_obj_t *src = e ? lv_event_get_target(e) : NULL;
    ESP_LOGW(TAG, "TL destroy_cb fired src=%p (link_pill=%p bar?=N/A grid?) timer=%p items=%p",
             (void*)src, (void*)s_link_pill, (void*)s_tl_timer, (void*)s_tl_items);
    if (s_tl_timer) {
        ESP_LOGW(TAG, "TL killing sync timer %p", (void*)s_tl_timer);
        lv_timer_del(s_tl_timer);
        s_tl_timer = NULL;
    }
    for (int i = 0; i < 16; i++) {
        s_tl_tiles[i]   = NULL;
        s_tl_leds[i]    = NULL;
        s_tl_names[i]   = NULL;
        s_tl_preview[i] = false;
    }
    s_tl_items       = NULL;
    s_tl_count       = 0;
    s_link_pill      = NULL;
    s_link_pill_lbl  = NULL;
    s_master_sw      = NULL;
    ESP_LOGW(TAG, "TL destroy_cb done");
}

/* User-data carries the tile index encoded as a pointer so no heap
 * allocation is required. This avoids the previous
 *   lv_obj_add_event_cb(sw, (lv_event_cb_t)lv_mem_free, LV_EVENT_DELETE, ctx)
 * pattern, which was UB: LVGL calls event callbacks as cb(event), not
 * cb(user_data), so that code was freeing the lv_event_t object and
 * silently corrupting the LVGL heap -- the corruption only blew up
 * later when navigating away and lv_obj_clean recursed through the
 * (already-corrupted) switch siblings. */
static void on_tile_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_tl_items || idx < 0 || idx >= s_tl_count) return;
    ctrl_toggle_t *item = &s_tl_items[idx];

    /* Tapping the tile toggles it: pick the opposite of whatever the
     * user currently sees (preview while pending, confirmed otherwise). */
    bool current = item->pending ? s_tl_preview[idx] : item->on;
    bool want    = !current;
    s_tl_preview[idx] = want;

    if (item->coil != CTRL_COIL_NONE) {
        /* Leave item->on unchanged until the worker confirms; mark
         * pending and enqueue. paint_tile_state() will use the preview
         * while pending=true and the amber border indicates in-flight. */
        item->pending = true;
        modbus_task_request(item->coil, want);
    } else {
        /* Unmapped item (e.g. Switches tab) -- UI-only toggle. */
        item->on = want;
    }
    paint_tile_state(idx);
}

/* Master switch: queues an individual write to every mapped Reading Light
 * coil so only the wired relays (1..8) flip. Using the Waveshare broadcast
 * register 0x00FF would also flip relays 9..16 which are not in use here.
 * The modbus worker drains the queue back-to-back so all 8 frames go out
 * within ~250 ms. */
static void on_master_change(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool want = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!s_tl_items) return;
    for (int i = 0; i < s_tl_count; i++) {
        if (s_tl_items[i].coil == CTRL_COIL_NONE) continue;
        s_tl_preview[i] = want;
        s_tl_items[i].pending = true;
        if (s_tl_tiles[i]) paint_tile_state(i);
        modbus_task_request(s_tl_items[i].coil, want);
    }
}

static void build_toggle_list(lv_obj_t *p, const char *title,
                              ctrl_toggle_t *items, int count)
{
    detail_header(p, title);

    /* Reset per-build state. */
    for (int i = 0; i < 16; i++) {
        s_tl_tiles[i]   = NULL;
        s_tl_leds[i]    = NULL;
        s_tl_names[i]   = NULL;
        s_tl_preview[i] = false;
    }
    s_tl_items        = items;
    s_tl_count        = count;
    s_tl_last_seq     = 0;
    s_tl_last_online  = !modbus_task_link_online();   /* force 1st paint */
    s_link_pill       = NULL;
    s_link_pill_lbl   = NULL;
    s_master_sw       = NULL;

    /* Link-status pill at the top-right (Reading Lights only -- Switches
     * are currently unmapped, so the pill would be misleading there). */
    bool any_mapped = false;
    for (int i = 0; i < count; i++) {
        if (items[i].coil != CTRL_COIL_NONE) { any_mapped = true; break; }
    }
    if (any_mapped) {
        s_link_pill = lv_obj_create(p);
        lv_obj_remove_style_all(s_link_pill);
        lv_obj_set_size(s_link_pill, 86, 26);
        lv_obj_set_pos(s_link_pill, 430, 18);
        lv_obj_set_style_radius(s_link_pill, 13, 0);
        lv_obj_set_style_bg_opa(s_link_pill, LV_OPA_COVER, 0);
        s_link_pill_lbl = lv_label_create(s_link_pill);
        lv_obj_set_style_text_font(s_link_pill_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_link_pill_lbl, hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_letter_space(s_link_pill_lbl, 2, 0);
        lv_obj_center(s_link_pill_lbl);
        paint_link_pill(modbus_task_link_online());
        /* Kill the sync timer on the FIRST sibling deletion so it can't
         * fire against freed link-pill memory while lv_obj_clean(s_detail)
         * walks the children. tl_destroy_cb is idempotent. */
        lv_obj_add_event_cb(s_link_pill, tl_destroy_cb, LV_EVENT_DELETE, NULL);
    }

    /* Master "All ON / All OFF" bar -- Reading Lights only (any_mapped). */
    int grid_y = 60;
    int grid_h = 260;
    if (any_mapped) {
        lv_obj_t *bar = lv_obj_create(p);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 496, 44);
        lv_obj_set_pos(bar, 20, 60);
        lv_obj_set_style_radius(bar, 14, 0);
        lv_obj_set_style_bg_color(bar, hex(UI_COLOR_SURFACE_LO), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(bar, hex(UI_COLOR_TAB), 0);
        lv_obj_set_style_border_width(bar, 2, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(bar);
        lv_label_set_text(lbl, "All Reading Lights");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, hex(UI_COLOR_TEXT), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t *msw = lv_switch_create(bar);
        lv_obj_set_size(msw, 56, 30);
        lv_obj_align(msw, LV_ALIGN_RIGHT_MID, -16, 0);
        style_theme_switch(msw);
        /* Pre-check if all mapped items are already ON so the master
         * reflects reality on page load rather than always starting OFF. */
        s_master_sw = msw;
        if (tl_all_mapped_on()) lv_obj_add_state(msw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(msw, on_master_change,
                            LV_EVENT_VALUE_CHANGED, NULL);
        /* Belt & suspenders: same cleanup hook on the master bar in case
         * the link pill was not created (e.g. unmapped tab). */
        lv_obj_add_event_cb(bar, tl_destroy_cb, LV_EVENT_DELETE, NULL);

        grid_y = 60 + 44 + 8;   /* push grid below the master bar */
        grid_h = 260 - 44 - 8;
    }

    /* 3-column square-tile grid.
     * Inner padding 0, column+row gap 8 px -> 3 * 152 + 2 * 8 = 472 px
     * horizontal footprint, centred in the 496 px content width. */
    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 496, grid_h);
    lv_obj_set_pos(grid, 20, grid_y);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);

    /* Destroying the grid tears down the sync timer. */
    lv_obj_add_event_cb(grid, tl_destroy_cb, LV_EVENT_DELETE, NULL);

    /* One tl_cb_ctx_t per tile so callbacks get (item, idx). LVGL does
     * not free user-data, so we lean on the grid delete to release us --
     * we allocate with lv_mem_alloc and free in a per-tile delete cb. */
    for (int i = 0; i < count && i < 16; i++) {
        /* Square tile: 152 x 152. The entire tile is the tap target --
         * no inner switch widget, so the glow IS the affordance. */
        lv_obj_t *tile = lv_obj_create(grid);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, 152, 152);
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        /* Tile index lives directly in user_data -- no alloc, no free. */
        lv_obj_add_event_cb(tile, on_tile_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_tl_tiles[i] = tile;

        /* Name label: top area, wraps onto 2 lines for long items. */
        lv_obj_t *name = lv_label_create(tile);
        lv_label_set_text(name, items[i].name);
        lv_obj_set_width(name, 128);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 22);
        /* Block touch on the label so dragging/tapping text still
         * reaches the tile's CLICKED handler. */
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
        s_tl_names[i] = name;

        /* LED dot in the lower half. Pure circular object with its own
         * shadow so the "lit bulb" look reads even from across the room. */
        lv_obj_t *led = lv_obj_create(tile);
        lv_obj_remove_style_all(led);
        lv_obj_set_size(led, 26, 26);
        lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
        lv_obj_align(led, LV_ALIGN_BOTTOM_MID, 0, -24);
        lv_obj_clear_flag(led, LV_OBJ_FLAG_CLICKABLE);
        s_tl_leds[i] = led;

        /* Seed the preview so a tap works correctly even before the
         * user has ever interacted with this tile. */
        s_tl_preview[i] = items[i].on;
        paint_tile_state(i);
    }

    /* 200 ms sync timer. Cleaned up via the grid's LV_EVENT_DELETE. */
    s_tl_timer = lv_timer_create(tl_sync_cb, 200, NULL);
}

static void detail_reading(lv_obj_t *p)
{
    int n = 0;
    ctrl_toggle_t *items = ctrl_reading_lights(&n);
    build_toggle_list(p, "Reading Lights", items, n);
}

static void detail_switches(lv_obj_t *p)
{
    int n = 0;
    ctrl_toggle_t *items = ctrl_switches(&n);
    build_toggle_list(p, "Switches", items, n);
}

/* =======================================================================
 *  Star Roof detail view -- master on/off + intensity slider.
 * ======================================================================= */

static lv_obj_t        *s_star_int_slider;
static lv_obj_t        *s_star_int_value;
static power_pill_ctx_t s_star_pp;

static void star_int_label_refresh(void)
{
    if (!s_star_int_value) return;
    char buf[12];
    snprintf(buf, sizeof(buf), "%u %%", (unsigned)ctrl_star_roof()->intensity);
    lv_label_set_text(s_star_int_value, buf);
}

/* Apply a star-roof change.
 *
 * SECONDARY has no bus access, so it mutates local state and pushes one
 * intent for the primary to replay. PRIMARY owns the relay board and
 * drives CTRL_STAR_COIL directly. Only the on/off state reaches the
 * hardware -- the intensity slider is cosmetic on a plain relay. */
static void star_apply(void)
{
    const ctrl_star_t *st = ctrl_star_roof();

    if (hmi_role_is_secondary()) {
        hmi_sync_push_intent(HMI_CMD_STAR_SET,
                             st->on ? 1u : 0u,
                             (uint16_t)st->intensity,
                             0u);
        return;
    }
    modbus_task_request(CTRL_STAR_COIL, st->on);
}

static void on_star_int_change(lv_event_t *e)
{
    (void)e;
    ctrl_star_roof()->intensity = (uint8_t)lv_slider_get_value(s_star_int_slider);
    star_int_label_refresh();
    star_apply();
}

static void detail_star(lv_obj_t *p)
{
    ctrl_star_t *st = ctrl_star_roof();
    detail_header(p, "Star Roof");

    s_star_pp = (power_pill_ctx_t){ .target = &st->on, .after = star_apply };
    build_power_pill(p, &s_star_pp, 444, 14);

    lv_obj_t *lbl = lv_label_create(p);
    lv_label_set_text(lbl, "INTENSITY");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_obj_set_pos(lbl, 20, 90);

    s_star_int_value = lv_label_create(p);
    lv_obj_set_style_text_font(s_star_int_value, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_star_int_value, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_star_int_value, 20, 112);
    star_int_label_refresh();

    s_star_int_slider = lv_slider_create(p);
    lv_obj_set_size(s_star_int_slider, 496, 8);
    lv_obj_set_pos(s_star_int_slider, 20, 186);
    lv_slider_set_range(s_star_int_slider, 0, 100);
    lv_slider_set_value(s_star_int_slider, st->intensity, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_star_int_slider, hex(UI_COLOR_SURFACE_LO), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_star_int_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_star_int_slider, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_star_int_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_star_int_slider, hex(UI_COLOR_TAB), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_star_int_slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_star_int_slider, hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_star_int_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_star_int_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s_star_int_slider, hex(UI_COLOR_TAB), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s_star_int_slider, LV_OPA_40, LV_PART_KNOB);
    lv_obj_add_event_cb(s_star_int_slider, on_star_int_change,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *hint = lv_label_create(p);
    lv_label_set_text(hint, "Fibre-optic ambient star field");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(hint, 20, 298);
}

/* =======================================================================
 *  Entry point -- called by ui_shell when user selects the Control tab.
 * ======================================================================= */

void ui_tab_control_build(lv_obj_t *content)
{
    /* reset state for a fresh build (shell calls lv_obj_clean first) */
    memset(s_list_rows, 0, sizeof(s_list_rows));
    s_detail       = NULL;
    s_active       = -1;
    s_rgb_wheel    = NULL;
    s_rgb_brt_slider = NULL;
    s_rgb_brt_value  = NULL;
    s_rgb_target     = NULL;
    s_star_int_slider = NULL;
    s_star_int_value  = NULL;

    build_nav_list(content);

    /* right-hand detail card that each detail_*() builder paints into */
    s_detail = panel(content, 248, 16, 536, 336);

    switch_detail(0);   /* open first entry by default */
    ESP_LOGI(TAG, "control tab built");
}
