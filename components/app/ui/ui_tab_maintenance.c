#include "ui_tabs.h"
#include "ui_theme.h"
#include "owner_name.h"
#include "tile_names.h"
#include "ctrl_state.h"
#include "hmi_role.h"
#include "hmi_link.h"
#include "bus_config.h"
#include "img_nikola_logo.h"
#include "hyperwisor.h"
#include "hyperwisor_app.h"
#include "display_brightness.h"
#include "modbus_client.h"
#include "modbus_task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_tab_maintenance";

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

/* ========================================================================
 *  MAINTENANCE TAB
 *
 *  Master-detail layout inside the 800 x 368 content area, same shape as
 *  the Control tab. Left pane is a nav list, right pane is a detail panel
 *  rebuilt whenever the selection changes.
 *
 *  +----------------+--------------------------------------------------+
 *  |  NAV LIST      |           DETAIL PANEL                           |
 *  |  (220 x 336)   |  (536 x 336, rebuilt on selection)               |
 *  |                |                                                  |
 *  |  Owner Info    |   < currently selected section renders here >    |
 *  |  Reading Lights|                                                  |
 *  |  Switches      |                                                  |
 *  |  HMI Role      |                                                  |
 *  |  Bus Setup     |                                                  |
 *  |  Theme         |                                                  |
 *  |  About         |                                                  |
 *  +----------------+--------------------------------------------------+
 *
 *  The virtual keyboard is parented to the active screen (floats above
 *  everything), hidden by default, and shown only while a textarea is
 *  focused inside a detail panel.
 * ======================================================================== */

#define TILE_MAX 8
#define MT_KB_H  190

/* ---------- forward-declared detail builders ---------- */
static void detail_owner    (lv_obj_t *p);
static void detail_reading  (lv_obj_t *p);
static void detail_switches (lv_obj_t *p);
static void detail_role     (lv_obj_t *p);
static void detail_bus      (lv_obj_t *p);
static void detail_theme    (lv_obj_t *p);
static void detail_display  (lv_obj_t *p);
static void detail_cloud    (lv_obj_t *p);
static void detail_about    (lv_obj_t *p);

typedef void (*mt_detail_fn_t)(lv_obj_t *parent);

typedef struct {
    const char     *icon;
    const char     *title;
    mt_detail_fn_t  build;
} mt_entry_t;

static const mt_entry_t s_entries[] = {
    { LV_SYMBOL_EDIT,     "Owner Info",     detail_owner    },
    { LV_SYMBOL_EYE_OPEN, "Reading Lights", detail_reading  },
    { LV_SYMBOL_POWER,    "Switches",       detail_switches },
    { LV_SYMBOL_HOME,     "HMI Role",       detail_role     },
    { LV_SYMBOL_WIFI,     "Bus Setup",      detail_bus      },
    { LV_SYMBOL_IMAGE,    "Theme",          detail_theme    },
    { LV_SYMBOL_SETTINGS, "Display",        detail_display  },
    { LV_SYMBOL_UPLOAD,   "Cloud",          detail_cloud    },
    { LV_SYMBOL_BELL,     "About",          detail_about    },
};
#define N_ENTRIES ((int)(sizeof(s_entries) / sizeof(s_entries[0])))

/* ---------- master-detail handles ---------- */
static lv_obj_t  *s_list_rows[N_ENTRIES];
static lv_obj_t  *s_detail;
static int        s_active = -1;

/* ---------- per-detail handles ---------- */
static lv_obj_t  *s_status_lbl;
static lv_timer_t *s_status_timer;
static lv_obj_t  *s_ta_owner;
static lv_obj_t  *s_ta_tiles[TILE_MAX];
static int        s_ta_tile_kind;
static int        s_ta_tile_count;
static lv_obj_t  *s_bri_slider;      /* Display -> brightness slider */
static lv_obj_t  *s_bri_value_lbl;   /* Display -> big % readout */

/* ---------- global kb (parented to active screen) ---------- */
static lv_obj_t  *s_kb;

/* =======================================================================
 *  Shared helpers
 * ======================================================================= */

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

static lv_obj_t *build_save_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                const char *label, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 160, 40);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, hex(UI_COLOR_TEXT), 0);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *build_status_lbl(lv_obj_t *parent,
                                  lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void clear_status(lv_timer_t *t)
{
    if (s_status_lbl) lv_label_set_text(s_status_lbl, "");
    lv_timer_del(t);
    s_status_timer = NULL;
}

static void show_status(const char *text, uint32_t color_hex)
{
    if (!s_status_lbl) return;
    lv_label_set_text(s_status_lbl, text);
    lv_obj_set_style_text_color(s_status_lbl, hex(color_hex), 0);
    if (s_status_timer) lv_timer_del(s_status_timer);
    s_status_timer = lv_timer_create(clear_status, 2500, NULL);
}

/* ---- keyboard show / hide ---- */

static void show_kb_for(lv_obj_t *ta)
{
    if (!s_kb || !ta) return;
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void hide_kb(void)
{
    if (!s_kb) return;
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void async_scroll_ta_into_view(void *arg)
{
    lv_obj_t *ta = (lv_obj_t *)arg;
    if (ta) lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static void on_ta_focused(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    show_kb_for(ta);
    lv_async_call(async_scroll_ta_into_view, ta);
}

static void on_kb_ready_or_cancel(lv_event_t *e)
{
    (void)e;
    hide_kb();
}

/* =======================================================================
 *  Nav list
 * ======================================================================= */

static void paint_list_row(int idx, bool active)
{
    lv_obj_t *row = s_list_rows[idx];
    if (!row) return;
    lv_obj_set_style_bg_color(row,
        hex(active ? UI_COLOR_SURFACE_HI : UI_COLOR_SURFACE_LO), 0);
    lv_obj_t *strip = lv_obj_get_child(row, 0);
    if (strip) lv_obj_set_style_bg_color(strip,
        hex(active ? UI_COLOR_TAB : UI_COLOR_SURFACE_LO), 0);
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
    hide_kb();
    if (s_active >= 0) paint_list_row(s_active, false);
    s_active = idx;
    paint_list_row(idx, true);

    /* Reset per-detail handles -- lv_obj_clean below is about to
     * destroy every widget the previous build registered. */
    if (s_status_timer) { lv_timer_del(s_status_timer); s_status_timer = NULL; }
    s_status_lbl = NULL;
    s_ta_owner   = NULL;
    s_bri_slider    = NULL;
    s_bri_value_lbl = NULL;
    for (int i = 0; i < TILE_MAX; i++) s_ta_tiles[i] = NULL;

    lv_obj_clean(s_detail);
    if (s_entries[idx].build) s_entries[idx].build(s_detail);
    ESP_LOGI(TAG, "maintenance detail -> %s", s_entries[idx].title);
}

static void on_list_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch_detail(idx);
}

static void build_nav_list(lv_obj_t *content)
{
    lv_obj_t *col = panel(content, 16, 16, 220, 336);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    /* Re-enable vertical scroll. panel() cleared SCROLLABLE which was
     * fine until the menu grew to 7 entries -- 7*42 + 6*4 = 318 px just
     * barely overflows the 320 px inner height once the 1 px border
     * is counted, clipping "About". Allow scroll as a future-proof
     * safety net even after we shrink the row height below. */
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < N_ENTRIES; i++) {
        lv_obj_t *row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 204, 38);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, hex(UI_COLOR_SURFACE_LO), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_list_row_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        s_list_rows[i] = row;

        lv_obj_t *strip = lv_obj_create(row);
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, 3, 26);
        lv_obj_align(strip, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_set_style_bg_color(strip, hex(UI_COLOR_SURFACE_LO), 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(strip, 2, 0);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, s_entries[i].icon);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(icon, hex(UI_COLOR_MUTED), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);

        lv_obj_t *title = lv_label_create(row);
        lv_label_set_text(title, s_entries[i].title);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, hex(UI_COLOR_MUTED), 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 46, 0);
    }
}

/* =======================================================================
 *  Detail: Owner Info
 * ======================================================================= */

static void on_save_owner(lv_event_t *e)
{
    (void)e;
    if (!s_ta_owner) return;
    const char *txt = lv_textarea_get_text(s_ta_owner);
    if (!txt || !txt[0]) {
        show_status("Owner name cannot be empty", UI_COLOR_HOT);
        return;
    }
    esp_err_t err = owner_name_set(txt);
    if (err != ESP_OK) {
        show_status("Save failed", UI_COLOR_HOT);
        return;
    }
    hide_kb();
    show_status("Saved", UI_COLOR_ON);
}

static void detail_owner(lv_obj_t *p)
{
    detail_header(p, "Owner Info");

    lv_obj_t *lbl = lv_label_create(p);
    lv_label_set_text(lbl, "OWNER NAME");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_obj_set_pos(lbl, 20, 72);

    s_ta_owner = lv_textarea_create(p);
    lv_textarea_set_one_line(s_ta_owner, true);
    lv_textarea_set_max_length(s_ta_owner, OWNER_NAME_MAX - 1);
    lv_textarea_set_text(s_ta_owner, owner_name_get());
    lv_obj_set_size(s_ta_owner, 496, 44);
    lv_obj_set_pos(s_ta_owner, 20, 96);
    lv_obj_set_style_bg_color(s_ta_owner, hex(UI_COLOR_SURFACE_LO), 0);
    lv_obj_set_style_text_color(s_ta_owner, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_ta_owner, hex(UI_COLOR_STROKE), 0);
    lv_obj_set_style_border_width(s_ta_owner, 1, 0);
    lv_obj_set_style_radius(s_ta_owner, 10, 0);
    lv_obj_set_style_text_font(s_ta_owner, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(s_ta_owner, on_ta_focused, LV_EVENT_FOCUSED, NULL);

    build_save_btn(p, 20, 160, LV_SYMBOL_SAVE "  Save", on_save_owner);
    s_status_lbl = build_status_lbl(p, 200, 170);

    lv_obj_t *hint = lv_label_create(p);
    lv_label_set_text(hint,
        "Shown on the top-bar and overview screen. Tap the field to\n"
        "bring up the on-screen keyboard.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(hint, 20, 222);
}

/* =======================================================================
 *  Detail: Reading Lights / Switches (tile rename)
 * ======================================================================= */

static lv_obj_t *build_rename_row(lv_obj_t *parent,
                                  const char *fixed_label,
                                  const char *current_name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, fixed_label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, CTRL_NAME_MAX - 1);
    lv_textarea_set_text(ta, current_name ? current_name : "");
    lv_obj_set_size(ta, 320, 36);
    lv_obj_align(ta, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(ta, hex(UI_COLOR_SURFACE_LO), 0);
    lv_obj_set_style_text_color(ta, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(ta, hex(UI_COLOR_STROKE), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(ta, on_ta_focused, LV_EVENT_FOCUSED, NULL);
    return ta;
}

static void on_save_tiles(lv_event_t *e)
{
    (void)e;
    bool any_err = false;
    for (int i = 0; i < s_ta_tile_count; i++) {
        if (!s_ta_tiles[i]) continue;
        const char *txt = lv_textarea_get_text(s_ta_tiles[i]);
        if (!txt || !txt[0]) continue;
        esp_err_t err = tile_names_set(s_ta_tile_kind, i, txt);
        if (err != ESP_OK) any_err = true;
    }
    hide_kb();
    show_status(any_err ? "Some items failed" : "Saved",
                any_err ? UI_COLOR_HOT : UI_COLOR_ON);
}

static void build_rename_detail(lv_obj_t *p, const char *title,
                                int kind, const char *row_prefix,
                                ctrl_toggle_t *items, int count)
{
    detail_header(p, title);

    s_ta_tile_kind  = kind;
    s_ta_tile_count = (count > TILE_MAX) ? TILE_MAX : count;

    lv_obj_t *list = lv_obj_create(p);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 496, 200);
    lv_obj_set_pos(list, 20, 60);
    lv_obj_set_style_bg_color(list, hex(UI_COLOR_SURFACE_LO), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, 10, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_border_color(list, hex(UI_COLOR_STROKE), 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < s_ta_tile_count; i++) {
        char fixed[24];
        snprintf(fixed, sizeof(fixed), "%s %d", row_prefix, i + 1);
        s_ta_tiles[i] = build_rename_row(list, fixed, items[i].name);
    }

    build_save_btn(p, 20, 274, LV_SYMBOL_SAVE "  Save All", on_save_tiles);
    s_status_lbl = build_status_lbl(p, 200, 284);
}

static void detail_reading(lv_obj_t *p)
{
    int n = 0;
    ctrl_toggle_t *items = ctrl_reading_lights(&n);
    build_rename_detail(p, "Reading Lights", TILE_KIND_READING,
                        "Light", items, n);
}

static void detail_switches(lv_obj_t *p)
{
    int n = 0;
    ctrl_toggle_t *items = ctrl_switches(&n);
    build_rename_detail(p, "Switches", TILE_KIND_SWITCH,
                        "Switch", items, n);
}

/* =======================================================================
 *  Detail: HMI Role
 * ======================================================================= */

static void on_pair_clicked(lv_event_t *e)
{
    (void)e;
    switch (hmi_role_get()) {
        case HMI_ROLE_PRIMARY:
            hmi_link_pair_begin(60);
            show_status("Pairing window open (60 s) -- tap PAIR on the wireless unit",
                        UI_COLOR_TAB);
            break;
        case HMI_ROLE_SECONDARY_WIRELESS:
            hmi_link_forget();
            show_status("Binding cleared -- searching for a primary in pairing mode",
                        UI_COLOR_TAB);
            break;
        default:
            show_status("Pairing applies to PRIMARY / WIRELESS roles only",
                        UI_COLOR_HOT);
            break;
    }
}

static void on_role_card_clicked(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id != HMI_ROLE_PRIMARY && id != HMI_ROLE_SECONDARY &&
        id != HMI_ROLE_SECONDARY_WIRELESS) return;
    if ((hmi_role_t)id == hmi_role_get()) return;
    show_status("Applying role\xE2\x80\xA6", UI_COLOR_TAB);
    hmi_role_set((hmi_role_t)id);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static void detail_role(lv_obj_t *p)
{
    detail_header(p, "HMI Role");

    lv_obj_t *desc = lv_label_create(p);
    lv_label_set_text(desc,
        "Pick which role this display runs as. The PRIMARY owns the\n"
        "RS-485 bus and drives relays + RGB + HVAC. Secondaries mirror\n"
        "the primary: SECONDARY over the bus, WIRELESS over ESP-NOW.");
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(desc, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(desc, 20, 58);

    hmi_role_t current = hmi_role_get();
    static const struct { hmi_role_t id; const char *title; const char *sub; } roles[] = {
        { HMI_ROLE_PRIMARY,            "PRIMARY",   "Drives relay, RGB, HVAC"     },
        { HMI_ROLE_SECONDARY,          "SECONDARY", "Mirrors the primary via bus" },
        { HMI_ROLE_SECONDARY_WIRELESS, "WIRELESS",  "Battery unit, radio mirror"  },
    };
    for (int i = 0; i < 3; i++) {
        bool active = (roles[i].id == current);
        lv_obj_t *card = lv_obj_create(p);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 232, 110);
        lv_obj_set_pos(card, 20 + i * 244, 118);
        lv_obj_set_style_bg_color(card,
            hex(active ? UI_COLOR_TAB : UI_COLOR_SURFACE_LO), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, active ? 3 : 1, 0);
        lv_obj_set_style_border_color(card,
            hex(active ? UI_COLOR_TAB : UI_COLOR_STROKE), 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_role_card_clicked,
                            LV_EVENT_CLICKED, (void *)(intptr_t)roles[i].id);

        lv_obj_t *title = lv_label_create(card);
        lv_label_set_text(title, roles[i].title);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_text_letter_space(title, 3, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 14);

        lv_obj_t *sub = lv_label_create(card);
        lv_label_set_text(sub, roles[i].sub);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sub,
            hex(active ? UI_COLOR_TEXT : UI_COLOR_MUTED), 0);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 14, -14);

        if (active) {
            lv_obj_t *tick = lv_label_create(card);
            lv_label_set_text(tick, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(tick, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(tick, hex(UI_COLOR_TEXT), 0);
            lv_obj_align(tick, LV_ALIGN_TOP_RIGHT, -12, 12);
        }
    }

    s_status_lbl = build_status_lbl(p, 20, 244);

    lv_obj_t *note = lv_label_create(p);
    lv_label_set_text(note, "Tap a card to save the new role and reboot.");
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(note, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(note, 20, 286);

    /* ---- Wireless pairing ----
     * PRIMARY: opens a 60 s window that accepts the next PAIR_REQ.
     * WIRELESS: forgets the binding; the link task then re-scans and
     * pairs to whichever primary has its window open. */
    lv_obj_t *pair = lv_obj_create(p);
    lv_obj_remove_style_all(pair);
    lv_obj_set_size(pair, 190, 44);
    lv_obj_set_pos(pair, 560, 236);
    lv_obj_set_style_bg_color(pair, hex(UI_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(pair, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pair, 10, 0);
    lv_obj_set_style_border_width(pair, 1, 0);
    lv_obj_set_style_border_color(pair, hex(UI_COLOR_STROKE), 0);
    lv_obj_clear_flag(pair, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pair, on_pair_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pl = lv_label_create(pair);
    lv_label_set_text(pl, LV_SYMBOL_WIFI "  PAIR WIRELESS");
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pl, hex(UI_COLOR_TEXT), 0);
    lv_obj_center(pl);

    char peer[24];
    hmi_link_peer_str(peer, sizeof(peer));
    char peer_line[48];
    snprintf(peer_line, sizeof(peer_line), "Paired peer: %s", peer);
    lv_obj_t *pinfo = lv_label_create(p);
    lv_label_set_text(pinfo, peer_line);
    lv_obj_set_style_text_font(pinfo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pinfo, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(pinfo, 560, 290);
}

/* =======================================================================
 *  Detail: Bus Setup
 * ======================================================================= */

/* Waveshare 16CH Modbus Relay baud register (holding reg 0x2000).
 * Encoded value per board datasheet:
 *   2 = 9600 bps
 *   5 = 115200 bps
 * The write must be issued at the CURRENT bus baud so every slave can
 * hear it. The board applies the new baud immediately (per observation),
 * so subsequent slaves in the same scan loop must still be addressed at
 * the OLD baud because the master UART hasn't switched yet. We achieve
 * that by keeping the master at the old speed until after the scan.   */
#define WS_BAUD_HREG    0x2000u

static uint16_t ws_baud_index(uint32_t baud)
{
    switch (baud) {
    case 9600u:   return 2;
    case 115200u: return 5;
    default:      return 0xFFFFu; /* sentinel = invalid */
    }
}

/* Issue the baud-switch write to every plausible slave address using
 * FC 0x06 (Write Single Register), the function code the Waveshare 16CH
 * Modbus RTU Relay board expects for its baud-config register 0x2000.
 * FC 0x10 is rejected by most Waveshare variants (illegal-function
 * exception) which is why an earlier FC 0x10 attempt looked like
 * "no slave replied."
 *
 * We probe 1..32 because bus addresses are user-configurable; the HMI
 * normally only polls 1..8 but a mis-configured board might sit higher.
 * If at least one slave ACKs, the switch is considered confirmed. If
 * nobody answers to unicast, we fall back to a Modbus broadcast
 * (slave=0, no reply expected) so boards that only accept broadcast
 * for baud config still migrate. Returns ESP_OK on any of those paths,
 * ESP_ERR_NOT_FOUND only if even broadcast UART write failed. */
static esp_err_t reprogram_slaves_baud(uint32_t new_baud)
{
    uint16_t idx = ws_baud_index(new_baud);
    if (idx == 0xFFFFu) return ESP_ERR_INVALID_ARG;

    ESP_LOGW(TAG, "=== BAUD REPROGRAM BEGIN: target=%lu idx=0x%04x ===",
             (unsigned long)new_baud, idx);

    int acked = 0;
    /* Scan the normal HMI bus range 1..8. Keeping it tight so the LVGL
     * thread isn't blocked for more than ~2 s. Boards at non-default
     * addresses are caught by the trailing broadcast.                 */
    for (uint8_t slave = 1; slave <= 8; ++slave) {
        ESP_LOGI(TAG, "[reprogram] probe slave=%u ...", (unsigned)slave);
        esp_err_t err = modbus_client_write_hreg_single(slave,
                                                        WS_BAUD_HREG,
                                                        idx);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "[reprogram] slave %u ACK (baud=%lu)",
                     (unsigned)slave, (unsigned long)new_baud);
            acked++;
        } else {
            ESP_LOGI(TAG, "[reprogram] slave %u no reply: %s",
                     (unsigned)slave, esp_err_to_name(err));
        }
        /* Pacing between probes. */
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    ESP_LOGW(TAG, "[reprogram] unicast phase done, acks=%d", acked);

    if (acked > 0) {
        /* Belt-and-suspenders: slaves outside 1..8 also migrate. */
        ESP_LOGW(TAG, "[reprogram] issuing belt-and-suspenders broadcast");
        (void)modbus_client_write_hreg_single(0, WS_BAUD_HREG, idx);
        ESP_LOGW(TAG, "=== BAUD REPROGRAM END: success (unicast) ===");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "[reprogram] no unicast ACK, issuing broadcast");
    esp_err_t berr = modbus_client_write_hreg_single(0, WS_BAUD_HREG, idx);
    if (berr != ESP_OK) {
        ESP_LOGE(TAG, "[reprogram] broadcast write FAILED: %s",
                 esp_err_to_name(berr));
        ESP_LOGE(TAG, "=== BAUD REPROGRAM END: FAILED ===");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "=== BAUD REPROGRAM END: success (broadcast) ===");
    return ESP_OK;
}

static void on_baud_clicked(lv_event_t *e)
{
    uint32_t new_baud = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    uint32_t cur_baud = bus_config_get_baud();
    ESP_LOGW(TAG, ">>> on_baud_clicked: cur=%lu new=%lu <<<",
             (unsigned long)cur_baud, (unsigned long)new_baud);
    if (new_baud == cur_baud) {
        ESP_LOGI(TAG, "    same baud, nothing to do");
        return;
    }

    /* Step 0: stop the modbus poller so it doesn't keep the UART lock
     * while we try to reprogram. Without this the LVGL thread waits up
     * to ~1 s per write for the poller to release the bus. */
    ESP_LOGW(TAG, "[baud] pausing modbus poller...");
    esp_err_t perr = modbus_task_pause(1500);
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "[baud] pause timed out (%s), continuing anyway",
                 esp_err_to_name(perr));
    } else {
        ESP_LOGW(TAG, "[baud] poller paused, bus is ours");
    }

    /* Step 1: tell every Waveshare slave on the bus to switch baud.
     * This MUST happen at the current baud, before we touch NVS or
     * restart, otherwise the boards stay at the old baud after reboot
     * and the bus goes silent.                                       */
    show_status("Reprogramming slaves\xE2\x80\xA6", UI_COLOR_TAB);
    lv_refr_now(NULL);

    esp_err_t err = reprogram_slaves_baud(new_baud);
    if (err == ESP_ERR_INVALID_ARG) {
        show_status("Unsupported baud", UI_COLOR_HOT);
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        /* Neither any unicast ACK nor the broadcast UART write
         * succeeded -- bus is physically broken. Abort to avoid leaving
         * host and slaves at different baud rates. */
        show_status("Bus write failed - aborted", UI_COLOR_HOT);
        ESP_LOGE(TAG, "baud switch aborted: 0x2000 write failed on unicast and broadcast");
        return;
    }

    /* Let every slave's internal UART re-lock on the new baud before we
     * tear the master down.                                           */
    vTaskDelay(pdMS_TO_TICKS(250));

    /* Step 2: persist the new host baud in NVS. */
    err = bus_config_set_baud(new_baud);
    if (err != ESP_OK) {
        show_status("Save failed", UI_COLOR_HOT);
        ESP_LOGE(TAG, "bus_config_set_baud(%lu) failed: %s",
                 (unsigned long)new_baud, esp_err_to_name(err));
        return;
    }

    /* Step 3: reboot so the master UART comes back at the new baud
     * already matched by every slave.                                 */
    ESP_LOGW(TAG, "[baud] NVS saved. Rebooting in 400ms...");
    show_status("Applying baud\xE2\x80\xA6", UI_COLOR_TAB);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static void detail_bus(lv_obj_t *p)
{
    detail_header(p, "Bus Setup");

    uint32_t current = bus_config_get_baud();
    char cur_buf[40];
    snprintf(cur_buf, sizeof(cur_buf), "Current baud:   %lu",
             (unsigned long)current);
    lv_obj_t *cur = lv_label_create(p);
    lv_label_set_text(cur, cur_buf);
    lv_obj_set_style_text_font(cur, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(cur, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(cur, 20, 62);

    lv_obj_t *hint = lv_label_create(p);
    lv_label_set_text(hint,
        "9600 is the factory default so the HMI can talk to an\n"
        "unconfigured Waveshare relay board out of the box. Only\n"
        "switch to 115200 after every device on the RS-485 bus has\n"
        "been reconfigured to match.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(hint, 20, 94);

    static const struct { uint32_t baud; const char *label; } options[] = {
        { 9600,   "Use 9600"   },
        { 115200, "Use 115200" },
    };
    for (int i = 0; i < 2; i++) {
        bool active = (options[i].baud == current);
        lv_obj_t *btn = lv_btn_create(p);
        lv_obj_set_size(btn, 180, 44);
        lv_obj_set_pos(btn, 20 + i * 196, 208);
        lv_obj_set_style_bg_color(btn,
            hex(active ? UI_COLOR_TAB : UI_COLOR_SURFACE_LO), 0);
        lv_obj_set_style_border_width(btn, active ? 0 : 1, 0);
        lv_obj_set_style_border_color(btn, hex(UI_COLOR_STROKE), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_baud_clicked,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)options[i].baud);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, options[i].label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, hex(UI_COLOR_TEXT), 0);
        lv_obj_center(lbl);
    }

    s_status_lbl = build_status_lbl(p, 20, 270);
}

/* =======================================================================
 *  Detail: Theme
 * ======================================================================= */

static void on_theme_swatch_clicked(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id < 0 || id >= UI_THEME_COUNT) return;
    show_status("Applying theme\xE2\x80\xA6", UI_COLOR_TAB);
    ui_theme_save((ui_theme_id_t)id);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static void detail_theme(lv_obj_t *p)
{
    detail_header(p, "Theme");

    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 496, 248);
    lv_obj_set_pos(grid, 20, 60);
    lv_obj_set_style_pad_all(grid, 10, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_bg_color(grid, hex(UI_COLOR_SURFACE_LO), 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grid, 10, 0);
    lv_obj_set_style_border_width(grid, 1, 0);
    lv_obj_set_style_border_color(grid, hex(UI_COLOR_STROKE), 0);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    ui_theme_id_t current = ui_theme_get();
    for (int i = 0; i < UI_THEME_COUNT; i++) {
        uint32_t primary = 0, accent = 0;
        ui_theme_swatch((ui_theme_id_t)i, &primary, &accent);

        lv_obj_t *sw = lv_obj_create(grid);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 144, 52);
        lv_obj_set_style_bg_color(sw, hex(primary), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, 8, 0);
        lv_obj_set_style_border_width(sw,
            (i == (int)current) ? 3 : 1, 0);
        lv_obj_set_style_border_color(sw,
            hex((i == (int)current) ? UI_COLOR_TAB : UI_COLOR_STROKE), 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sw, on_theme_swatch_clicked,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *dot = lv_obj_create(sw);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, 7, 0);
        lv_obj_set_style_bg_color(dot, hex(accent), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *name = lv_label_create(sw);
        lv_label_set_text(name, ui_theme_name((ui_theme_id_t)i));
        lv_obj_set_width(name, 106);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        uint8_t r = (primary >> 16) & 0xFF;
        uint8_t g = (primary >>  8) & 0xFF;
        uint8_t b =  primary        & 0xFF;
        int lum = (r * 299 + g * 587 + b * 114) / 1000;
        lv_obj_set_style_text_color(name,
            lv_color_hex(lum > 140 ? 0x151515 : 0xF5F5F5), 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 28, 0);
    }
}

/* =======================================================================
 *  Detail: Display (LCD brightness)
 *
 *  Slider range: 5..100. The hardware backlight on CH422G EXIO2 is a
 *  plain on/off pin (no PWM path), so the "dim" is actually a composited
 *  opaque-black overlay painted on lv_layer_sys(). Level 0 (backlight
 *  off) is intentionally NOT reachable from this slider -- a fully dark
 *  screen from a touch-drag would be irrecoverable.
 *
 *  Live drag -> display_brightness_preview() (visual only, no NVS write).
 *  Release   -> display_brightness_set()     (persists to NVS namespace "disp").
 * ======================================================================= */

#define MT_BRI_MIN 5
#define MT_BRI_MAX 100

static void bri_value_label_refresh(int v)
{
    if (!s_bri_value_lbl) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_bri_value_lbl, buf);
}

static void on_bri_value_changed(lv_event_t *e)
{
    (void)e;
    if (!s_bri_slider) return;
    int v = lv_slider_get_value(s_bri_slider);
    if (v < MT_BRI_MIN) v = MT_BRI_MIN;
    display_brightness_preview((uint8_t)v);
    bri_value_label_refresh(v);
}

static void on_bri_released(lv_event_t *e)
{
    (void)e;
    if (!s_bri_slider) return;
    int v = lv_slider_get_value(s_bri_slider);
    if (v < MT_BRI_MIN) v = MT_BRI_MIN;
    esp_err_t err = display_brightness_set((uint8_t)v);
    if (err == ESP_OK) {
        show_status("Saved", UI_COLOR_ON);
    } else {
        show_status("Save failed", UI_COLOR_HOT);
    }
}

static void detail_display(lv_obj_t *p)
{
    detail_header(p, "Display");

    /* Caption */
    lv_obj_t *cap = lv_label_create(p);
    lv_label_set_text(cap, "BRIGHTNESS");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_set_pos(cap, 20, 72);

    /* Big value readout (e.g. "80%") */
    s_bri_value_lbl = lv_label_create(p);
    lv_obj_set_style_text_font(s_bri_value_lbl, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_bri_value_lbl, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_bri_value_lbl, 20, 92);

    uint8_t current = display_brightness_get();
    if (current < MT_BRI_MIN) current = MT_BRI_MIN;   /* clamp stale 0..4 */
    bri_value_label_refresh(current);

    /* Slider styled to match the Control-tab RGB brightness slider so
     * the two feel identical to the user. */
    s_bri_slider = lv_slider_create(p);
    lv_obj_set_size(s_bri_slider, 480, 8);
    lv_obj_set_pos(s_bri_slider, 20, 180);
    lv_slider_set_range(s_bri_slider, MT_BRI_MIN, MT_BRI_MAX);
    lv_slider_set_value(s_bri_slider, current, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bri_slider, hex(UI_COLOR_SURFACE_LO), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bri_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bri_slider, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bri_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bri_slider, hex(UI_COLOR_TAB), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bri_slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bri_slider, hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_bri_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_bri_slider, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s_bri_slider, hex(UI_COLOR_TAB), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s_bri_slider, LV_OPA_40, LV_PART_KNOB);
    lv_obj_add_event_cb(s_bri_slider, on_bri_value_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bri_slider, on_bri_released,
                        LV_EVENT_RELEASED, NULL);

    /* Hint line */
    lv_obj_t *hint = lv_label_create(p);
    lv_label_set_text(hint,
        "Dim is a software overlay (no PWM backlight on this board).");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(hint, 20, 222);

    /* Transient status label, same 2.5 s timer as other sections */
    s_status_lbl = build_status_lbl(p, 20, 260);
}

/* =======================================================================
 *  Detail: Cloud (Hyperwisor IoT status)
 * ======================================================================= */

static lv_obj_t  *s_cloud_wifi_lbl;
static lv_obj_t  *s_cloud_ws_lbl;
static lv_obj_t  *s_cloud_bind_lbl;
static lv_obj_t  *s_cloud_dev_lbl;
static lv_obj_t  *s_cloud_user_lbl;
static lv_obj_t  *s_cloud_ssid_lbl;
static lv_obj_t  *s_cloud_pass_lbl;
static lv_obj_t  *s_cloud_mac_lbl;
static lv_obj_t  *s_cloud_clear_btn;
static lv_obj_t  *s_cloud_clear_lbl;
static lv_obj_t  *s_cloud_ta_device;
static lv_obj_t  *s_cloud_ta_user;
static lv_obj_t  *s_cloud_form_status;
static lv_timer_t *s_cloud_timer;
static lv_timer_t *s_cloud_confirm_timer;
static lv_timer_t *s_cloud_form_clear_timer;
static uint8_t     s_cloud_confirm_pending;

/* Mask a secret: keep first 2 and last 2 chars, dots in between.
 * Examples: ""          -> "(not set)"
 *           "abc"       -> "***"
 *           "abcdef"    -> "ab\xE2\x80\xA2\xE2\x80\xA2ef"
 *           "longsecret"-> "lo\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2et" */
static void mask_secret(char *out, size_t out_sz, const char *in)
{
    if (!in || in[0] == '\0') { snprintf(out, out_sz, "(not set)"); return; }
    size_t n = strlen(in);
    if (n <= 4) { snprintf(out, out_sz, "****  (len=%u)", (unsigned)n); return; }
    size_t mid = n - 4;
    if (mid > 8) mid = 8;
    char dots[24] = {0};
    for (size_t i = 0; i < mid && (i*3 + 4) < sizeof(dots); i++) {
        memcpy(dots + i*3, "\xE2\x80\xA2", 3);   /* U+2022 bullet */
    }
    snprintf(out, out_sz, "%c%c%s%c%c  (len=%u)",
             in[0], in[1], dots, in[n-2], in[n-1], (unsigned)n);
}

static void cloud_refresh(lv_timer_t *t)
{
    (void)t;
    hyperwisor_state_t *st = hyperwisor_get_state();
    if (!st) return;

    char buf[160];
    char mask[64];

    if (s_cloud_wifi_lbl) {
        if (st->ap_mode_active) {
            snprintf(buf, sizeof(buf), "WiFi:    AP mode (provisioning)");
        } else if (st->wifi_connected) {
            snprintf(buf, sizeof(buf), "WiFi:    connected to %.40s",
                     st->ssid[0] ? st->ssid : "(unknown)");
        } else if (st->ssid[0]) {
            snprintf(buf, sizeof(buf), "WiFi:    connecting to %.40s\xE2\x80\xA6",
                     st->ssid);
        } else {
            snprintf(buf, sizeof(buf), "WiFi:    not configured");
        }
        lv_label_set_text(s_cloud_wifi_lbl, buf);
    }

    if (s_cloud_ws_lbl) {
        snprintf(buf, sizeof(buf), "Cloud:   %s",
                 st->ws_connected ? "connected" : "offline");
        lv_label_set_text(s_cloud_ws_lbl, buf);
        lv_obj_set_style_text_color(s_cloud_ws_lbl,
            hex(st->ws_connected ? UI_COLOR_ON : UI_COLOR_MUTED), 0);
    }

    if (s_cloud_ssid_lbl) {
        snprintf(buf, sizeof(buf), "SSID:    %.48s",
                 st->ssid[0] ? st->ssid : "(not set)");
        lv_label_set_text(s_cloud_ssid_lbl, buf);
    }

    if (s_cloud_pass_lbl) {
        mask_secret(mask, sizeof(mask), st->password);
        snprintf(buf, sizeof(buf), "Pass:    %s", mask);
        lv_label_set_text(s_cloud_pass_lbl, buf);
    }

    if (s_cloud_dev_lbl) {
        snprintf(buf, sizeof(buf), "Device:  %.56s",
                 st->device_id[0] ? st->device_id : "(not set)");
        lv_label_set_text(s_cloud_dev_lbl, buf);
        lv_obj_set_style_text_color(s_cloud_dev_lbl,
            hex(st->device_id[0] ? UI_COLOR_TEXT : UI_COLOR_WARN), 0);
    }

    if (s_cloud_user_lbl) {
        snprintf(buf, sizeof(buf), "User:    %.56s",
                 st->user_id[0] ? st->user_id : "(not set)");
        lv_label_set_text(s_cloud_user_lbl, buf);
    }

    if (s_cloud_bind_lbl) {
        const char *tgt = hyperwisor_app_get_target_id();
        if (tgt && tgt[0]) {
            snprintf(buf, sizeof(buf), "Bind:    %.56s", tgt);
        } else {
            snprintf(buf, sizeof(buf),
                     "Bind:    waiting for 'config' command from app");
        }
        lv_label_set_text(s_cloud_bind_lbl, buf);
    }

    if (s_cloud_mac_lbl) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(buf, sizeof(buf),
                 "MAC:     %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        lv_label_set_text(s_cloud_mac_lbl, buf);
    }
}

static void cloud_confirm_reset(lv_timer_t *t)
{
    (void)t;
    s_cloud_confirm_pending = 0;
    if (s_cloud_clear_lbl) {
        lv_label_set_text(s_cloud_clear_lbl, "Clear & Reboot");
    }
    if (s_cloud_clear_btn) {
        lv_obj_set_style_bg_color(s_cloud_clear_btn, hex(UI_COLOR_TAB), 0);
    }
    if (s_cloud_confirm_timer) {
        lv_timer_del(s_cloud_confirm_timer);
        s_cloud_confirm_timer = NULL;
    }
}

static void on_cloud_clear_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_cloud_confirm_pending) {
        /* First tap -- arm the action and show confirmation prompt. */
        s_cloud_confirm_pending = 1;
        if (s_cloud_clear_lbl) {
            lv_label_set_text(s_cloud_clear_lbl, "Tap again to confirm");
        }
        if (s_cloud_clear_btn) {
            lv_obj_set_style_bg_color(s_cloud_clear_btn, hex(UI_COLOR_WARN), 0);
        }
        if (s_cloud_confirm_timer) lv_timer_del(s_cloud_confirm_timer);
        s_cloud_confirm_timer = lv_timer_create(cloud_confirm_reset, 3000, NULL);
        lv_timer_set_repeat_count(s_cloud_confirm_timer, 1);
        return;
    }

    /* Second tap within 3 s -- actually wipe and reboot. */
    if (s_cloud_clear_lbl) {
        lv_label_set_text(s_cloud_clear_lbl, "Clearing\xE2\x80\xA6");
    }
    ESP_LOGW("ui_cloud", "User requested clear credentials -- wiping NVS");
    hyperwisor_clear_credentials();
    /* Brief delay so the UI label updates and NVS commit lands. */
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static void cloud_form_status_clear(lv_timer_t *t)
{
    (void)t;
    if (s_cloud_form_status) lv_label_set_text(s_cloud_form_status, "");
    if (s_cloud_form_clear_timer) {
        lv_timer_del(s_cloud_form_clear_timer);
        s_cloud_form_clear_timer = NULL;
    }
}

static void cloud_form_show_status(const char *text, uint32_t color)
{
    if (!s_cloud_form_status) return;
    lv_label_set_text(s_cloud_form_status, text);
    lv_obj_set_style_text_color(s_cloud_form_status, hex(color), 0);
    if (s_cloud_form_clear_timer) lv_timer_del(s_cloud_form_clear_timer);
    s_cloud_form_clear_timer = lv_timer_create(cloud_form_status_clear,
                                                3000, NULL);
    lv_timer_set_repeat_count(s_cloud_form_clear_timer, 1);
}

static void on_cloud_save_ids(lv_event_t *e)
{
    (void)e;
    if (!s_cloud_ta_device || !s_cloud_ta_user) return;

    const char *dev  = lv_textarea_get_text(s_cloud_ta_device);
    const char *user = lv_textarea_get_text(s_cloud_ta_user);

    if (!dev || dev[0] == '\0') {
        cloud_form_show_status("Device ID required", UI_COLOR_HOT);
        return;
    }
    esp_err_t rd = hyperwisor_set_device_id(dev);
    if (rd != ESP_OK) {
        cloud_form_show_status("Save device failed", UI_COLOR_HOT);
        return;
    }
    /* user_id is optional -- only save if non-empty. */
    if (user && user[0] != '\0') {
        (void)hyperwisor_set_user_id(user);
    }
    ESP_LOGW("ui_cloud", "Manual IDs saved: device=[%s] user=[%s]",
             dev, user ? user : "");
    hide_kb();
    cloud_form_show_status("Saved \xE2\x80\xA2 will reconnect", UI_COLOR_ON);
}

static void on_cloud_panel_deleted(lv_event_t *e)
{
    (void)e;
    if (s_cloud_timer) { lv_timer_del(s_cloud_timer); s_cloud_timer = NULL; }
    if (s_cloud_confirm_timer) {
        lv_timer_del(s_cloud_confirm_timer);
        s_cloud_confirm_timer = NULL;
    }
    if (s_cloud_form_clear_timer) {
        lv_timer_del(s_cloud_form_clear_timer);
        s_cloud_form_clear_timer = NULL;
    }
    s_cloud_confirm_pending = 0;
    s_cloud_wifi_lbl  = NULL;
    s_cloud_ws_lbl    = NULL;
    s_cloud_bind_lbl  = NULL;
    s_cloud_dev_lbl   = NULL;
    s_cloud_user_lbl  = NULL;
    s_cloud_ssid_lbl  = NULL;
    s_cloud_pass_lbl  = NULL;
    s_cloud_mac_lbl   = NULL;
    s_cloud_clear_btn = NULL;
    s_cloud_clear_lbl = NULL;
    s_cloud_ta_device = NULL;
    s_cloud_ta_user   = NULL;
    s_cloud_form_status = NULL;
}

/* Narrow row label -- used for the 2-column status grid so we can pack
 * all 8 status rows in 4 rows plus leave room for the manual-entry form. */
static lv_obj_t *cloud_row_label_xw(lv_obj_t *p, lv_coord_t x, lv_coord_t y,
                                     lv_coord_t w, uint32_t color)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, "");
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void detail_cloud(lv_obj_t *p)
{
    detail_header(p, "Cloud");

    /* Clear & Reboot button -- top-right of the panel. Two-tap confirm so
     * an accidental brush doesn't wipe the saved provisioning. */
    s_cloud_confirm_pending = 0;
    s_cloud_clear_btn = lv_btn_create(p);
    lv_obj_set_size(s_cloud_clear_btn, 170, 34);
    lv_obj_set_pos(s_cloud_clear_btn, 346, 14);
    lv_obj_set_style_bg_color(s_cloud_clear_btn, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_radius(s_cloud_clear_btn, 8, 0);
    lv_obj_set_style_shadow_width(s_cloud_clear_btn, 0, 0);
    lv_obj_set_style_border_width(s_cloud_clear_btn, 1, 0);
    lv_obj_set_style_border_color(s_cloud_clear_btn, hex(UI_COLOR_WARN), 0);
    lv_obj_add_event_cb(s_cloud_clear_btn, on_cloud_clear_clicked,
                        LV_EVENT_CLICKED, NULL);
    s_cloud_clear_lbl = lv_label_create(s_cloud_clear_btn);
    lv_label_set_text(s_cloud_clear_lbl, "Clear & Reboot");
    lv_obj_set_style_text_font(s_cloud_clear_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cloud_clear_lbl, hex(UI_COLOR_TEXT), 0);
    lv_obj_center(s_cloud_clear_lbl);

    /* ---- Status grid: 2 columns x 4 rows, y=54..118 ---- */
    /* Left column (x=20, w=244): WiFi, Cloud, SSID, Pass.
     * Right column (x=276, w=244): Device, User, Bind, MAC. */
    s_cloud_wifi_lbl = cloud_row_label_xw(p,  20,  54, 244, UI_COLOR_TEXT);
    s_cloud_ws_lbl   = cloud_row_label_xw(p,  20,  72, 244, UI_COLOR_TEXT);
    s_cloud_ssid_lbl = cloud_row_label_xw(p,  20,  90, 244, UI_COLOR_MUTED);
    s_cloud_pass_lbl = cloud_row_label_xw(p,  20, 108, 244, UI_COLOR_MUTED);
    s_cloud_dev_lbl  = cloud_row_label_xw(p, 276,  54, 244, UI_COLOR_TEXT);
    s_cloud_user_lbl = cloud_row_label_xw(p, 276,  72, 244, UI_COLOR_MUTED);
    s_cloud_bind_lbl = cloud_row_label_xw(p, 276,  90, 244, UI_COLOR_MUTED);
    s_cloud_mac_lbl  = cloud_row_label_xw(p, 276, 108, 244, UI_COLOR_DIM);

    /* ---- Manual-entry form ---- */
    lv_obj_t *form_hdr = lv_label_create(p);
    lv_label_set_text(form_hdr, "MANUAL ENTRY");
    lv_obj_set_style_text_font(form_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(form_hdr, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_text_letter_space(form_hdr, 3, 0);
    lv_obj_set_pos(form_hdr, 20, 140);

    lv_obj_t *l_dev = lv_label_create(p);
    lv_label_set_text(l_dev, "Device ID");
    lv_obj_set_style_text_font(l_dev, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l_dev, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(l_dev, 20, 164);

    s_cloud_ta_device = lv_textarea_create(p);
    lv_textarea_set_one_line(s_cloud_ta_device, true);
    lv_textarea_set_max_length(s_cloud_ta_device, HYPERWISOR_DEVICE_ID_LEN - 1);
    lv_textarea_set_placeholder_text(s_cloud_ta_device,
                                     "paste device id (uuid) here");
    {
        hyperwisor_state_t *st = hyperwisor_get_state();
        lv_textarea_set_text(s_cloud_ta_device,
                             (st && st->device_id[0]) ? st->device_id : "");
    }
    lv_obj_set_size(s_cloud_ta_device, 390, 34);
    lv_obj_set_pos(s_cloud_ta_device, 20, 184);
    lv_obj_set_style_bg_color(s_cloud_ta_device, hex(UI_COLOR_SURFACE_LO), 0);
    lv_obj_set_style_text_color(s_cloud_ta_device, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_cloud_ta_device, hex(UI_COLOR_STROKE), 0);
    lv_obj_set_style_border_width(s_cloud_ta_device, 1, 0);
    lv_obj_set_style_radius(s_cloud_ta_device, 8, 0);
    lv_obj_set_style_text_font(s_cloud_ta_device, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(s_cloud_ta_device, on_ta_focused, LV_EVENT_FOCUSED,
                        NULL);

    lv_obj_t *l_usr = lv_label_create(p);
    lv_label_set_text(l_usr, "User ID (optional)");
    lv_obj_set_style_text_font(l_usr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l_usr, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(l_usr, 20, 224);

    s_cloud_ta_user = lv_textarea_create(p);
    lv_textarea_set_one_line(s_cloud_ta_user, true);
    lv_textarea_set_max_length(s_cloud_ta_user, HYPERWISOR_USER_ID_LEN - 1);
    lv_textarea_set_placeholder_text(s_cloud_ta_user,
                                     "paste user id (optional)");
    {
        hyperwisor_state_t *st = hyperwisor_get_state();
        lv_textarea_set_text(s_cloud_ta_user,
                             (st && st->user_id[0]) ? st->user_id : "");
    }
    lv_obj_set_size(s_cloud_ta_user, 390, 34);
    lv_obj_set_pos(s_cloud_ta_user, 20, 244);
    lv_obj_set_style_bg_color(s_cloud_ta_user, hex(UI_COLOR_SURFACE_LO), 0);
    lv_obj_set_style_text_color(s_cloud_ta_user, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_cloud_ta_user, hex(UI_COLOR_STROKE), 0);
    lv_obj_set_style_border_width(s_cloud_ta_user, 1, 0);
    lv_obj_set_style_radius(s_cloud_ta_user, 8, 0);
    lv_obj_set_style_text_font(s_cloud_ta_user, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(s_cloud_ta_user, on_ta_focused, LV_EVENT_FOCUSED,
                        NULL);

    /* Save button right of the TAs, spanning both rows. */
    lv_obj_t *save_btn = lv_btn_create(p);
    lv_obj_set_size(save_btn, 96, 94);
    lv_obj_set_pos(save_btn, 420, 184);
    lv_obj_set_style_bg_color(save_btn, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, on_cloud_save_ids, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save_btn);
    lv_label_set_text(sl, LV_SYMBOL_SAVE "\nSave");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sl, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(sl);

    /* Inline status below the form. */
    s_cloud_form_status = lv_label_create(p);
    lv_label_set_text(s_cloud_form_status, "");
    lv_obj_set_style_text_font(s_cloud_form_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cloud_form_status, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(s_cloud_form_status, 20, 286);

    lv_obj_t *hint = lv_label_create(p);
    lv_label_set_text(hint,
        "AP fallback: \"" HYPERWISOR_AP_SSID_PREFIX
        "-XXXX\" / " HYPERWISOR_AP_PASS);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(hint, 20, 312);

    /* Clean up timer when this panel is torn down.
     *
     * IMPORTANT: register DELETE on a *child* (s_cloud_wifi_lbl) rather
     * than on `p`. `p` IS s_detail, the persistent container that the
     * shell only lv_obj_clean()s -- it never gets deleted on tab switch,
     * so a DELETE handler on it would never fire and the refresh timer
     * would keep running against dangling label pointers (crash). */
    lv_obj_add_event_cb(s_cloud_wifi_lbl, on_cloud_panel_deleted,
                        LV_EVENT_DELETE, NULL);

    /* Populate immediately, then refresh every 1 s while visible. */
    cloud_refresh(NULL);
    s_cloud_timer = lv_timer_create(cloud_refresh, 1000, NULL);
}

/* =======================================================================
 *  Detail: About
 * ======================================================================= */

static void detail_about(lv_obj_t *p)
{
    detail_header(p, "About");

    lv_obj_t *line1 = lv_label_create(p);
    lv_label_set_text(line1, "System designed & powered by");
    lv_obj_set_style_text_font(line1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(line1, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(line1, 20, 70);

    lv_obj_t *logo = lv_img_create(p);
    lv_img_set_src(logo, &img_nikola_logo);
    lv_obj_set_style_img_recolor(logo, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_img_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_pos(logo, 20, 96);

    lv_obj_t *line2 = lv_label_create(p);
    lv_label_set_text(line2, "NIKOLAINDUSTRY PVT. LTD.");
    lv_obj_set_style_text_font(line2, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(line2, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_text_letter_space(line2, 2, 0);
    lv_obj_set_pos(line2, 20, 172);

    lv_obj_t *line3 = lv_label_create(p);
    lv_label_set_text(line3, LV_SYMBOL_HOME "  https://www.nikolaindustry.com/");
    lv_obj_set_style_text_font(line3, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(line3, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(line3, 20, 208);

    char buf[80];
    snprintf(buf, sizeof(buf), "Role:  %s     Bus:  %lu bps",
             hmi_role_str(hmi_role_get()),
             (unsigned long)bus_config_get_baud());
    lv_obj_t *role_line = lv_label_create(p);
    lv_label_set_text(role_line, buf);
    lv_obj_set_style_text_font(role_line, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(role_line, hex(UI_COLOR_DIM), 0);
    lv_obj_set_pos(role_line, 20, 252);
}

/* =======================================================================
 *  Tab teardown: kill keyboard + timer when the tab content is cleaned
 *  by the shell on a tab switch. We hook on s_detail (child of content)
 *  so its deletion reliably fires even though lv_obj_clean deletes only
 *  children, not the parent itself.
 * ======================================================================= */

static void on_detail_deleted(lv_event_t *e)
{
    (void)e;
    if (s_status_timer) { lv_timer_del(s_status_timer); s_status_timer = NULL; }
    s_status_lbl = NULL;
    s_ta_owner   = NULL;
    s_bri_slider    = NULL;
    s_bri_value_lbl = NULL;
    for (int i = 0; i < TILE_MAX; i++) s_ta_tiles[i] = NULL;
    for (int i = 0; i < N_ENTRIES; i++) s_list_rows[i] = NULL;
    s_detail = NULL;
    s_active = -1;
    if (s_kb) { lv_obj_del(s_kb); s_kb = NULL; }
}

/* =======================================================================
 *  Entry point
 * ======================================================================= */

void ui_tab_maintenance_build(lv_obj_t *content)
{
    /* Reset cached handles -- the previous build's widgets have been
     * destroyed by the shell. */
    memset(s_list_rows, 0, sizeof(s_list_rows));
    s_detail       = NULL;
    s_active       = -1;
    s_status_lbl   = NULL;
    s_ta_owner     = NULL;
    s_bri_slider    = NULL;
    s_bri_value_lbl = NULL;
    for (int i = 0; i < TILE_MAX; i++) s_ta_tiles[i] = NULL;

    build_nav_list(content);
    s_detail = panel(content, 248, 16, 536, 336);
    lv_obj_add_event_cb(s_detail, on_detail_deleted, LV_EVENT_DELETE, NULL);

    /* Keyboard parented to the active screen so it overlays topbar +
     * tabbar + content, hidden by default. */
    lv_obj_t *scr = lv_scr_act();
    s_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_kb, 800, MT_KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, -UI_TABBAR_H);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, on_kb_ready_or_cancel, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_kb, on_kb_ready_or_cancel, LV_EVENT_CANCEL, NULL);

    switch_detail(0);   /* open Owner Info by default */
    ESP_LOGI(TAG, "maintenance tab built (%d sections)", N_ENTRIES);
}
