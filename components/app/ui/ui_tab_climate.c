#include "ui_tabs.h"
#include "ui_theme.h"
#include "ac_state.h"
#include "bmp280.h"
#include "hvac_controller.h"
#include "hmi_role.h"
#include "hmi_sync.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ui_tab_climate";

/* Big vent image shown on the left of the climate tab. */
LV_IMG_DECLARE(vent_img);

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

/* widget handles we need for live updates */
static lv_obj_t *s_power_pill;
static lv_obj_t *s_power_lbl;
static lv_obj_t *s_auto_pill;
static lv_obj_t *s_fan_slider;
static lv_obj_t *s_temp_slider;
static lv_obj_t *s_temp_lbl;

/* live BMP280 readout widgets (may stay empty if no sensor attached) */
static lv_obj_t *s_cabin_card;
static lv_obj_t *s_cabin_temp_lbl;
static lv_obj_t *s_cabin_press_lbl;
static lv_timer_t *s_cabin_timer;

/* live HVAC status strip (bottom of the controls card) */
static lv_obj_t *s_hvac_toggle;      /* clickable "[+] Details" header  */
static lv_obj_t *s_hvac_divider;     /* thin line above the detail block */
static lv_obj_t *s_hvac_mode_lbl;
static lv_obj_t *s_hvac_fan_lbl;
static lv_obj_t *s_hvac_ac_lbl;
static lv_obj_t *s_hvac_delta_lbl;
static bool      s_hvac_expanded;    /* false = collapsed (default)     */

/* forward decl: defined after build_hero but referenced inside it */
static void on_tab_delete(lv_event_t *e);

/* ===== visual refresh from ac_state ===== */
static void refresh_power(void)
{
    ac_state_t *st = ac_state();
    lv_color_t bg = st->power ? hex(UI_COLOR_TAB) : hex(UI_COLOR_SURFACE_HI);
    lv_color_t fg = st->power ? hex(0xFFFFFF)      : hex(UI_COLOR_MUTED);
    lv_obj_set_style_bg_color(s_power_pill, bg, 0);
    lv_obj_set_style_text_color(s_power_lbl, fg, 0);
    lv_label_set_text(s_power_lbl, st->power ? "ON" : "OFF");
}
static void refresh_auto(void)
{
    ac_state_t *st = ac_state();
    lv_color_t bg = st->auto_mode ? hex(UI_COLOR_TAB) : hex(UI_COLOR_SURFACE_HI);
    lv_obj_set_style_bg_color(s_auto_pill, bg, 0);
    lv_obj_t *lbl = lv_obj_get_child(s_auto_pill, 0);
    lv_obj_set_style_text_color(lbl, st->auto_mode ? hex(0xFFFFFF) : hex(UI_COLOR_MUTED), 0);
}
static void refresh_fan(void)
{
    ac_state_t *st = ac_state();
    if (lv_slider_get_value(s_fan_slider) != st->fan_level) {
        lv_slider_set_value(s_fan_slider, st->fan_level, LV_ANIM_OFF);
    }
}
static void refresh_temp(void)
{
    ac_state_t *st = ac_state();
    if (lv_slider_get_value(s_temp_slider) != st->temp_c) {
        lv_slider_set_value(s_temp_slider, st->temp_c, LV_ANIM_OFF);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", st->temp_c);
    lv_label_set_text(s_temp_lbl, buf);
}
static void refresh_all(void)
{
    refresh_power(); refresh_auto(); refresh_fan(); refresh_temp();
}

/* ===== event handlers ===== */
/* Secondary HMI pushes each state change as an intent so the primary
 * (which owns the HVAC controller) can apply it authoritatively. The
 * local mutation stays so the UI repaints instantly; primary's mirror
 * push (~500 ms) confirms or corrects. */
static inline void ac_push_secondary(hmi_ac_field_t field, uint16_t value)
{
    if (hmi_role_is_secondary()) {
        hmi_sync_push_intent(HMI_CMD_AC_SET, (uint16_t)field, value, 0);
    }
}

static void on_power(lv_event_t *e)
{
    (void)e;
    ac_state()->power = !ac_state()->power;
    refresh_power();
    ac_push_secondary(HMI_AC_FIELD_POWER, ac_state()->power ? 1 : 0);
}
static void on_auto(lv_event_t *e)
{
    (void)e;
    ac_state()->auto_mode = !ac_state()->auto_mode;
    refresh_auto();
    ac_push_secondary(HMI_AC_FIELD_AUTO, ac_state()->auto_mode ? 1 : 0);
}
static void on_fan_change(lv_event_t *e)
{
    (void)e;
    ac_state()->fan_level = lv_slider_get_value(s_fan_slider);
    ac_push_secondary(HMI_AC_FIELD_FAN_LEVEL, ac_state()->fan_level);
}
static void on_temp_change(lv_event_t *e)
{
    (void)e;
    ac_state()->temp_c = lv_slider_get_value(s_temp_slider);
    char buf[8]; snprintf(buf, sizeof(buf), "%d", ac_state()->temp_c);
    lv_label_set_text(s_temp_lbl, buf);
    ac_push_secondary(HMI_AC_FIELD_TEMP, ac_state()->temp_c);
}

/* ===== small widget factories ===== */
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, UI_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, hex(UI_COLOR_STROKE), 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 70, 30);
    lv_obj_set_style_bg_color(b, hex(UI_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 15, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, hex(UI_COLOR_MUTED), 0);
    lv_obj_center(l);
    return b;
}

static void section_label(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(l, x, y);
}

/* ===== LEFT: hero card with vent image ===== */
static void build_hero(lv_obj_t *content)
{
    lv_obj_t *card = panel(content, 16, 16, 376, 336);

    /* soft blue glow behind the vent */
    lv_obj_set_style_shadow_width(card, 40, 0);
    lv_obj_set_style_shadow_color(card, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_spread(card, 0, 0);

    /* vent image (280x277) centered inside the 376x336 card */
    lv_obj_t *img = lv_img_create(card);
    lv_img_set_src(img, &vent_img);
    lv_obj_center(img);

    /* ----- Cabin sensor overlay (top-left of hero) ----- */
    s_cabin_card = lv_obj_create(card);
    lv_obj_remove_style_all(s_cabin_card);
    lv_obj_set_size(s_cabin_card, 168, 84);
    lv_obj_set_pos(s_cabin_card, 12, 12);
    lv_obj_set_style_bg_color(s_cabin_card, hex(UI_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(s_cabin_card, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_cabin_card, 12, 0);
    lv_obj_set_style_border_width(s_cabin_card, 1, 0);
    lv_obj_set_style_border_color(s_cabin_card, hex(UI_COLOR_STROKE), 0);
    lv_obj_set_style_pad_all(s_cabin_card, 10, 0);
    lv_obj_clear_flag(s_cabin_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(s_cabin_card);
    lv_label_set_text(cap, "CABIN");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(cap, 0, 0);

    s_cabin_temp_lbl = lv_label_create(s_cabin_card);
    lv_label_set_text(s_cabin_temp_lbl, "-- \xC2\xB0" "C");
    lv_obj_set_style_text_font(s_cabin_temp_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_cabin_temp_lbl, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_cabin_temp_lbl, 0, 18);

    s_cabin_press_lbl = lv_label_create(s_cabin_card);
    lv_label_set_text(s_cabin_press_lbl, "-- hPa");
    lv_obj_set_style_text_font(s_cabin_press_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cabin_press_lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(s_cabin_press_lbl, 0, 52);

    /* Tear the timer down when THIS card is destroyed. The shell's
     * content container persists across tab switches (lv_obj_clean()
     * only drops the children), so attaching LV_EVENT_DELETE to the
     * container would never fire. The cabin card IS a child of the
     * container, so it gets cleaned -> our callback fires -> timer
     * is killed before the next tick touches freed widgets. */
    lv_obj_add_event_cb(s_cabin_card, on_tab_delete, LV_EVENT_DELETE, NULL);
}

/* LVGL-thread timer: pulls the latest BMP280 sample and updates labels. */
static void cabin_tick(lv_timer_t *t)
{
    (void)t;
    if (s_cabin_temp_lbl) {
        float tc = 0.0f, ph = 0.0f;
        if (bmp280_get(&tc, &ph)) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%.1f \xC2\xB0" "C", tc);
            lv_label_set_text(s_cabin_temp_lbl, buf);
            snprintf(buf, sizeof(buf), "%d hPa", (int)(ph + 0.5f));
            lv_label_set_text(s_cabin_press_lbl, buf);
        }
    }

    /* ---- HVAC status strip ---- */
    if (!s_hvac_mode_lbl) return;
    hvac_status_t h;
    hvac_controller_get_status(&h);

    /* line 1: mode + wait reason (pure ASCII, font has no Greek/arrows) */
    const char *mode_s = "OFF";
    switch (h.mode) {
        case HVAC_MODE_AUTO:   mode_s = "AUTO";   break;
        case HVAC_MODE_MANUAL: mode_s = "MANUAL"; break;
        case HVAC_MODE_PURGE:  mode_s = "PURGE";  break;
        case HVAC_MODE_OFF:
        default:               mode_s = "OFF";    break;
    }
    char line1[64];
    int secs = (h.wait_ms_left + 999) / 1000;
    switch (h.wait) {
        case HVAC_WAIT_PRESPIN:
            snprintf(line1, sizeof(line1), "MODE: %s | blower pre-spin %ds", mode_s, secs);
            break;
        case HVAC_WAIT_LOCKOUT:
            snprintf(line1, sizeof(line1), "MODE: %s | compressor rest %ds", mode_s, secs);
            break;
        case HVAC_WAIT_FAN_OFF:
            snprintf(line1, sizeof(line1), "MODE: %s | fan off (no cooling)", mode_s);
            break;
        case HVAC_WAIT_NO_SENSOR:
            snprintf(line1, sizeof(line1), "MODE: %s | no cabin sensor", mode_s);
            break;
        case HVAC_WAIT_PURGE:
            snprintf(line1, sizeof(line1), "MODE: %s | evap purge %ds", mode_s, secs);
            break;
        case HVAC_WAIT_NONE:
        default:
            snprintf(line1, sizeof(line1), "MODE: %s | running", mode_s);
            break;
    }
    lv_label_set_text(s_hvac_mode_lbl, line1);

    /* line 2: fan step + which CH is driving it */
    const char *fan_names[] = { "OFF", "LOW", "MED", "HIGH", "MAX" };
    char line2[64];
    if (h.fan_step <= 0 || h.fan_coil < 0) {
        snprintf(line2, sizeof(line2), "FAN: OFF");
    } else {
        /* relay board labels channels 1..16, our coils are 0..15 */
        snprintf(line2, sizeof(line2), "FAN: %s  (CH%d)",
                 fan_names[h.fan_step <= 4 ? h.fan_step : 4],
                 h.fan_coil + 1);
    }
    lv_label_set_text(s_hvac_fan_lbl, line2);

    /* line 3: compressor clutch */
    char line3[64];
    if (h.clutch_coil < 0) {
        snprintf(line3, sizeof(line3), "A/C: not wired");
    } else if (h.clutch_on) {
        snprintf(line3, sizeof(line3), "A/C: COOLING  (CH%d ON)", h.clutch_coil + 1);
    } else {
        snprintf(line3, sizeof(line3), "A/C: idle  (CH%d OFF)", h.clutch_coil + 1);
    }
    lv_label_set_text(s_hvac_ac_lbl, line3);

    /* line 4: delta + hint (ASCII: "dT" instead of Greek delta, "->" for arrow) */
    char line4[96];
    if (!h.cabin_valid) {
        snprintf(line4, sizeof(line4), "dT: --   target %.0fC", h.target_c);
    } else {
        const char *hint = "in range";
        if (h.delta_c >=  3.0f) hint = "very hot -> HIGH";
        else if (h.delta_c >=  1.5f) hint = "warm -> MED";
        else if (h.delta_c >=  0.3f) hint = "a bit warm -> LOW";
        else if (h.delta_c <= -0.3f) hint = "at/below target";
        snprintf(line4, sizeof(line4),
                 "dT: %+.1fC   (%.1f -> %.0f)   %s",
                 h.delta_c, h.cabin_c, h.target_c, hint);
    }
    lv_label_set_text(s_hvac_delta_lbl, line4);

    /* Header mirrors the MOST IMPORTANT status even when collapsed -- so
     * the user sees "running / pre-spin / cooling / locked out" at a glance
     * without having to expand. */
    const char *short_state;
    if (h.mode == HVAC_MODE_OFF)                         short_state = "idle";
    else if (h.mode == HVAC_MODE_PURGE)                  short_state = "purge";
    else if (h.wait == HVAC_WAIT_PRESPIN)                short_state = "pre-spin";
    else if (h.wait == HVAC_WAIT_LOCKOUT)                short_state = "A/C rest";
    else if (h.wait == HVAC_WAIT_NO_SENSOR)              short_state = "no sensor";
    else if (h.wait == HVAC_WAIT_FAN_OFF)                short_state = "fan off";
    else if (h.clutch_on)                                short_state = "cooling";
    else if (h.fan_step > 0)                             short_state = "fan only";
    else                                                 short_state = "ready";

    char head[48];
    snprintf(head, sizeof(head), "%s HVAC details  -  %s",
             s_hvac_expanded ? "[-]" : "[+]", short_state);
    lv_label_set_text(s_hvac_toggle, head);

    /* colour hint on the header: green=running, amber=waiting, grey=idle */
    lv_color_t c = hex(UI_COLOR_MUTED);
    if (h.mode == HVAC_MODE_AUTO || h.mode == HVAC_MODE_MANUAL) {
        c = (h.wait == HVAC_WAIT_NONE) ? hex(0x4ECB71) : hex(0xE0A84C);
    } else if (h.mode == HVAC_MODE_PURGE) {
        c = hex(0xE0A84C);
    }
    lv_obj_set_style_text_color(s_hvac_toggle, c, 0);
}

/* Toggle handler: flip the expanded state, show/hide the detail rows. */
static void on_hvac_toggle(lv_event_t *e)
{
    (void)e;
    if (!s_hvac_mode_lbl) return;
    s_hvac_expanded = !s_hvac_expanded;
    lv_obj_t *rows[] = { s_hvac_divider, s_hvac_mode_lbl, s_hvac_fan_lbl,
                         s_hvac_ac_lbl,  s_hvac_delta_lbl };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        if (!rows[i]) continue;
        if (s_hvac_expanded) lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_add_flag(rows[i],  LV_OBJ_FLAG_HIDDEN);
    }
    /* Repaint header immediately so the [+]/[-] flips without waiting for
     * the next 1 Hz tick. */
    cabin_tick(NULL);
}

static void on_tab_delete(lv_event_t *e)
{
    (void)e;
    if (s_cabin_timer) {
        lv_timer_del(s_cabin_timer);
        s_cabin_timer = NULL;
    }
    s_cabin_card      = NULL;
    s_cabin_temp_lbl  = NULL;
    s_cabin_press_lbl = NULL;
    s_hvac_toggle     = NULL;
    s_hvac_divider    = NULL;
    s_hvac_mode_lbl   = NULL;
    s_hvac_fan_lbl    = NULL;
    s_hvac_ac_lbl     = NULL;
    s_hvac_delta_lbl  = NULL;
    s_hvac_expanded   = false;
}

/* ===== RIGHT: control card ===== */
static void build_controls(lv_obj_t *content)
{
    lv_obj_t *card = panel(content, 408, 16, 376, 336);
    lv_obj_set_style_pad_all(card, 20, 0);
    /* Reserve only 4 px at the bottom so the HVAC details block fits
     * inside the card without being clipped (default 20 px would cut
     * off the last ~2 lines of the expanded detail view). */
    lv_obj_set_style_pad_bottom(card, 4, 0);

    /* --- header: A/C title + OFF pill + AUTO pill --- */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "A/C");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(title, 0, 0);

    s_power_pill = pill(card, "OFF", on_power);
    lv_obj_set_pos(s_power_pill, 60, 4);
    s_power_lbl  = lv_obj_get_child(s_power_pill, 0);

    s_auto_pill = pill(card, "AUTO", on_auto);
    lv_obj_set_pos(s_auto_pill, 266, 4);

    /* --- Fan --- */
    section_label(card, "Fan", 0, 68);
    lv_obj_t *fan_icon = lv_label_create(card);
    lv_label_set_text(fan_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(fan_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(fan_icon, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(fan_icon, 0, 92);

    /* smooth 1..5 fan slider, matches temp slider visual language */
    s_fan_slider = lv_slider_create(card);
    lv_obj_set_size(s_fan_slider, 260, 8);
    lv_obj_set_pos(s_fan_slider, 44, 100);
    lv_slider_set_range(s_fan_slider, 1, 5);
    lv_slider_set_value(s_fan_slider, 3, LV_ANIM_OFF);
    /* recessed dark track */
    lv_obj_set_style_bg_color(s_fan_slider, hex(UI_COLOR_SURFACE_LO), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_fan_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_fan_slider, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_fan_slider, 0, LV_PART_MAIN);
    /* filled portion in brand blue */
    lv_obj_set_style_bg_color(s_fan_slider, hex(UI_COLOR_TAB), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_fan_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_fan_slider, 4, LV_PART_INDICATOR);
    /* white knob with blue glow, matches temp */
    lv_obj_set_style_bg_color(s_fan_slider, hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_fan_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_fan_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_fan_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s_fan_slider, hex(UI_COLOR_TAB), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s_fan_slider, LV_OPA_40, LV_PART_KNOB);
    lv_obj_add_event_cb(s_fan_slider, on_fan_change, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Temperature --- */
    section_label(card, "Temperature", 0, 168);

    s_temp_lbl = lv_label_create(card);
    lv_label_set_text(s_temp_lbl, "20");
    lv_obj_set_style_text_font(s_temp_lbl, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_temp_lbl, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_temp_lbl, 0, 186);

    lv_obj_t *unit = lv_label_create(card);
    lv_label_set_text(unit, "C");
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(unit, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(unit, 56, 222);

    s_temp_slider = lv_slider_create(card);
    lv_obj_set_size(s_temp_slider, 250, 8);
    lv_obj_set_pos(s_temp_slider, 84, 214);
    lv_slider_set_range(s_temp_slider, 16, 30);
    lv_slider_set_value(s_temp_slider, 20, LV_ANIM_OFF);
    /* track = horizontal blue (cool) -> red (hot) gradient */
    lv_obj_set_style_bg_color(s_temp_slider, hex(UI_COLOR_COOL), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_temp_slider, hex(UI_COLOR_HOT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_temp_slider, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_temp_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_temp_slider, 4, LV_PART_MAIN);
    /* hide filled portion, keep gradient consistent */
    lv_obj_set_style_bg_opa(s_temp_slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    /* white knob with soft shadow */
    lv_obj_set_style_bg_color(s_temp_slider, hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_temp_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_temp_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_temp_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s_temp_slider, hex(UI_COLOR_TAB), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s_temp_slider, LV_OPA_40, LV_PART_KNOB);
    lv_obj_add_event_cb(s_temp_slider, on_temp_change, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Live HVAC status strip (COLLAPSIBLE) ---
     * Default: collapsed. Only a single tiny header is visible, which
     * always shows a 1-word summary of the controller state with a
     * coloured dot (green=running, amber=waiting, grey=idle). Tap the
     * header to expand and reveal the 4-line detailed view; tap again
     * to collapse. Row spacing is 14 px (tight) so all 4 detail rows
     * fit inside the card after the pad_bottom=4 override above. */
    s_hvac_divider = lv_obj_create(card);
    lv_obj_remove_style_all(s_hvac_divider);
    lv_obj_set_size(s_hvac_divider, 336, 1);
    lv_obj_set_pos(s_hvac_divider, 0, 228);
    lv_obj_set_style_bg_color(s_hvac_divider, hex(UI_COLOR_STROKE), 0);
    lv_obj_set_style_bg_opa(s_hvac_divider, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_hvac_divider, LV_OBJ_FLAG_HIDDEN);

    s_hvac_toggle = lv_label_create(card);
    lv_label_set_text(s_hvac_toggle, "[+] HVAC details  -  idle");
    lv_obj_set_style_text_font(s_hvac_toggle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hvac_toggle, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(s_hvac_toggle, 0, 232);
    lv_obj_add_flag(s_hvac_toggle, LV_OBJ_FLAG_CLICKABLE);
    /* horizontal tap-padding only; vertical pad would push the text
     * down and push the whole block past the card bottom again. */
    lv_obj_set_style_pad_hor(s_hvac_toggle, 4, 0);
    lv_obj_add_event_cb(s_hvac_toggle, on_hvac_toggle, LV_EVENT_CLICKED, NULL);

    s_hvac_mode_lbl = lv_label_create(card);
    lv_label_set_text(s_hvac_mode_lbl, "MODE: --");
    lv_obj_set_style_text_font(s_hvac_mode_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hvac_mode_lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(s_hvac_mode_lbl, 0, 250);
    lv_obj_add_flag(s_hvac_mode_lbl, LV_OBJ_FLAG_HIDDEN);

    s_hvac_fan_lbl = lv_label_create(card);
    lv_label_set_text(s_hvac_fan_lbl, "FAN: --");
    lv_obj_set_style_text_font(s_hvac_fan_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hvac_fan_lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(s_hvac_fan_lbl, 0, 264);
    lv_obj_add_flag(s_hvac_fan_lbl, LV_OBJ_FLAG_HIDDEN);

    s_hvac_ac_lbl = lv_label_create(card);
    lv_label_set_text(s_hvac_ac_lbl, "A/C: --");
    lv_obj_set_style_text_font(s_hvac_ac_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hvac_ac_lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(s_hvac_ac_lbl, 0, 278);
    lv_obj_add_flag(s_hvac_ac_lbl, LV_OBJ_FLAG_HIDDEN);

    s_hvac_delta_lbl = lv_label_create(card);
    lv_label_set_text(s_hvac_delta_lbl, "dT: --");
    lv_obj_set_style_text_font(s_hvac_delta_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hvac_delta_lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(s_hvac_delta_lbl, 0, 292);
    lv_obj_add_flag(s_hvac_delta_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* ===== entry ===== */
void ui_tab_climate_build(lv_obj_t *content)
{
    build_hero(content);
    build_controls(content);
    refresh_all();

    /* Delete callback is attached to s_cabin_card in build_hero(),
     * NOT here -- `content` is the shell's persistent container and
     * never receives LV_EVENT_DELETE on a tab switch. */

    /* Kick off the live cabin reading. Sensor may not be ready yet on
     * first tick -- the labels just stay at "--" until bmp280_get()
     * returns true. */
    if (!s_cabin_timer) {
        s_cabin_timer = lv_timer_create(cabin_tick, 1000, NULL);
        cabin_tick(NULL);   /* immediate first paint if sensor is ready */
    }
    ESP_LOGI(TAG, "climate tab built");
}
