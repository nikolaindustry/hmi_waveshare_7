#include "ui_tabs.h"
#include "ui_theme.h"
#include "owner_name.h"
#include "ctrl_state.h"
#include "modbus_task.h"
#include "esp_log.h"

static const char *TAG = "ui_tab_overview";

/* Generated C asset from assets/van_hero.png (451x340, RGBA). */
LV_IMG_DECLARE(van_hero_img);

/* Cursive font for the owner name hero. ASCII-only, 4 bpp.
 * Generated from Amsterdam Handwriting.ttf via lv_font_conv.
 * Two sizes: the 56 px "hero" size looks great for short names
 * like "Alex" or "VM", but longer names (e.g. "VM Chavan") blow
 * past the 340 px greeting column and clip at the right edge.
 * pick_owner_font() below measures the rendered width and drops
 * to the 40 px variant when the big one would overflow. */
LV_FONT_DECLARE(font_cursive_56);
LV_FONT_DECLARE(font_cursive_40);

/* Pixel budget for the owner name. Greeting column is 800 - 460 = 340 px;
 * we keep an 8 px safety margin so the rightmost glyph's flourish stays
 * clear of the screen edge. */
#define GREET_NAME_MAX_W   332

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

/* Measure `name` rendered in `font` and return its pixel width. */
static lv_coord_t measure_width(const char *name, const lv_font_t *font)
{
    lv_point_t sz;
    lv_txt_get_size(&sz, name, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* Pick the largest cursive font whose rendered width fits inside
 * GREET_NAME_MAX_W. Falls through to the smallest font if even that
 * overflows - better to truncate one long name gracefully than to
 * show a clipped half-glyph hanging off the screen. */
static const lv_font_t *pick_owner_font(const char *name)
{
    if (measure_width(name, &font_cursive_56) <= GREET_NAME_MAX_W)
        return &font_cursive_56;
    return &font_cursive_40;
}

/* Label we need to update when the owner renames themself from the
 * Maintenance tab. Cleared to NULL by an LV_EVENT_DELETE handler so
 * the owner-name listener can safely detect a stale pointer. */
static lv_obj_t *s_greet_name_lbl = NULL;

static void on_greet_name_deleted(lv_event_t *e)
{
    (void)e;
    s_greet_name_lbl = NULL;
}

static void on_owner_changed(const char *new_name)
{
    if (s_greet_name_lbl) {
        lv_label_set_text(s_greet_name_lbl, new_name);
        /* Re-run auto-fit: a user who renames themselves from "VM"
         * to "Christopher Williams" needs the label to shrink live. */
        lv_obj_set_style_text_font(s_greet_name_lbl,
                                   pick_owner_font(new_name), 0);
    }
}

/* =======================================================================
 *  Quick buttons
 *
 *  Tiles the owner marked "HOME" in Maintenance, surfaced here so the
 *  things touched every trip do not need a detour through the Control
 *  tab. Same state model and the same press semantics as the Control
 *  tab -- latching or momentary per the tile's own flag -- so there is
 *  one behaviour to learn, not two.
 *
 *  Geometry: the greeting column is 340 px wide starting at x=460.
 *  CTRL_HOME_MAX (4) tiles of 78 px with 9 px gaps is exactly 339 px.
 *  The buttons sit at the TOP of the column (64..142) with the
 *  greeting underneath -- they are what the driver reaches for, so
 *  they get the thumb-friendly half of the screen, not the decoration.
 * ======================================================================= */

#define QB_SIZE   78
#define QB_GAP    9
#define QB_Y      64

static lv_obj_t      *s_qb_tiles[CTRL_HOME_MAX];
static ctrl_toggle_t *s_qb_items[CTRL_HOME_MAX];
static bool           s_qb_preview[CTRL_HOME_MAX];
static int            s_qb_count;
static lv_timer_t    *s_qb_timer;
static uint32_t       s_qb_last_seq;

static void qb_paint(int i)
{
    if (i < 0 || i >= s_qb_count || !s_qb_tiles[i] || !s_qb_items[i]) return;
    const ctrl_toggle_t *it = s_qb_items[i];
    bool show_on = it->pending ? s_qb_preview[i] : it->on;

    lv_obj_set_style_bg_color(s_qb_tiles[i],
        hex(show_on ? UI_COLOR_SURFACE_HI : UI_COLOR_SURFACE_LO), 0);

    uint32_t border;
    if (it->pending)  border = UI_COLOR_WARN;
    else if (show_on) border = UI_COLOR_TAB;
    else              border = UI_COLOR_STROKE;
    lv_obj_set_style_border_color(s_qb_tiles[i], hex(border), 0);

    lv_obj_set_style_shadow_color(s_qb_tiles[i], hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_shadow_width(s_qb_tiles[i], show_on ? 18 : 0, 0);
    lv_obj_set_style_shadow_spread(s_qb_tiles[i], show_on ? 1 : 0, 0);

    lv_obj_t *lbl = lv_obj_get_child(s_qb_tiles[i], 0);
    if (lbl) {
        lv_obj_set_style_text_color(lbl,
            hex(show_on ? UI_COLOR_TEXT : UI_COLOR_MUTED), 0);
    }
}

static void qb_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_qb_count || !s_qb_items[i]) return;
    ctrl_toggle_t *it = s_qb_items[i];
    if (it->momentary) return;          /* driven by press/release */

    bool current = it->pending ? s_qb_preview[i] : it->on;
    s_qb_preview[i] = !current;
    ctrl_toggle_drive(it, !current);
    qb_paint(i);
}

static void qb_press(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_qb_count || !s_qb_items[i]) return;
    ctrl_toggle_t *it = s_qb_items[i];
    if (!it->momentary || it->held) return;
    s_qb_preview[i] = true;
    ctrl_momentary_hold_begin(it);
    qb_paint(i);
}

static void qb_release(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_qb_count || !s_qb_items[i]) return;
    ctrl_toggle_t *it = s_qb_items[i];
    if (!it->momentary || !it->held) return;
    s_qb_preview[i] = false;
    ctrl_momentary_hold_end();
    qb_paint(i);
}

/* Repaint when the Modbus worker confirms a state change, exactly as
 * the Control tab's tile grid does. */
static void qb_sync_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t seq = modbus_task_state_seq();
    if (seq == s_qb_last_seq) return;
    s_qb_last_seq = seq;
    for (int i = 0; i < s_qb_count; i++) qb_paint(i);
}

static void qb_destroy_cb(lv_event_t *e)
{
    (void)e;
    /* Same rule as the Control tab: a momentary coil never outlives
     * the widget the finger was on. */
    ctrl_momentary_release_all();
    if (s_qb_timer) {
        lv_timer_del(s_qb_timer);
        s_qb_timer = NULL;
    }
    for (int i = 0; i < CTRL_HOME_MAX; i++) {
        s_qb_tiles[i]   = NULL;
        s_qb_items[i]   = NULL;
        s_qb_preview[i] = false;
    }
    s_qb_count = 0;
}

/* Collect up to CTRL_HOME_MAX flagged tiles, reading lights first. */
static void qb_collect(void)
{
    s_qb_count = 0;
    for (int k = 0; k < 2 && s_qb_count < CTRL_HOME_MAX; k++) {
        int n = 0;
        ctrl_toggle_t *arr = (k == 0) ? ctrl_reading_lights(&n)
                                      : ctrl_switches(&n);
        for (int i = 0; i < n && s_qb_count < CTRL_HOME_MAX; i++) {
            if (arr[i].home) s_qb_items[s_qb_count++] = &arr[i];
        }
    }
}

static void build_quick_buttons(lv_obj_t *content, int x)
{
    qb_collect();
    if (s_qb_count == 0) return;   /* nothing flagged -- greeting only */

    for (int i = 0; i < s_qb_count; i++) {
        lv_obj_t *tile = lv_obj_create(content);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, QB_SIZE, QB_SIZE);
        lv_obj_set_pos(tile, x + i * (QB_SIZE + QB_GAP), QB_Y);
        lv_obj_set_style_radius(tile, 12, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, qb_click,   LV_EVENT_CLICKED,    (void *)(intptr_t)i);
        lv_obj_add_event_cb(tile, qb_press,   LV_EVENT_PRESSED,    (void *)(intptr_t)i);
        lv_obj_add_event_cb(tile, qb_release, LV_EVENT_RELEASED,   (void *)(intptr_t)i);
        lv_obj_add_event_cb(tile, qb_release, LV_EVENT_PRESS_LOST, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, s_qb_items[i]->name);
        lv_obj_set_width(lbl, QB_SIZE - 10);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
        /* Let taps fall through the text to the tile underneath. */
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        /* A momentary button says so, so nobody taps it and wonders
         * why it did not stay on. */
        if (s_qb_items[i]->momentary) {
            lv_obj_t *tag = lv_label_create(tile);
            lv_label_set_text(tag, "HOLD");
            lv_obj_set_style_text_font(tag, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(tag, hex(UI_COLOR_DIM), 0);
            lv_obj_align(tag, LV_ALIGN_BOTTOM_MID, 0, -4);
            lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);
        }

        s_qb_tiles[i]   = tile;
        s_qb_preview[i] = false;
        qb_paint(i);
    }

    s_qb_last_seq = modbus_task_state_seq();
    s_qb_timer    = lv_timer_create(qb_sync_cb, 200, NULL);
    /* Hook cleanup on a tile, not on `content` -- content is reused
     * across tab switches and is cleaned rather than deleted. */
    lv_obj_add_event_cb(s_qb_tiles[0], qb_destroy_cb, LV_EVENT_DELETE, NULL);

    ESP_LOGI(TAG, "quick buttons: %d shown", s_qb_count);
}

void ui_tab_overview_build(lv_obj_t *content)
{
    /* Hero van image - flush against the LEFT edge of the screen.
     * Source is 451x340 so it sits inside the 800x368 tab content
     * with ~14 px of vertical slack which we centre. Aligning to
     * LEFT_MID with x-offset 0 keeps the image pinned to the screen
     * edge regardless of parent padding, just like the top-bar logo
     * hugs its corner. No container, no border, just the raw PNG. */
    lv_obj_t *img = lv_img_create(content);
    lv_img_set_src(img, &van_hero_img);
    lv_img_set_pivot(img, 0, 0);
    lv_obj_align(img, LV_ALIGN_LEFT_MID, 0, 0);

    /* Greeting column occupies the remaining right half of the tab.
     * Content width 800 - hero 451 = 349 px, starting at x=460 with
     * a 9 px visual gap away from the hero's right edge. */
    const int greet_x = 460;

    lv_obj_t *eyebrow = lv_label_create(content);
    lv_label_set_text(eyebrow, "WELCOME BACK");
    lv_obj_set_style_text_font(eyebrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eyebrow, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(eyebrow, 6, 0);
    lv_obj_set_pos(eyebrow, greet_x, 196);

    s_greet_name_lbl = lv_label_create(content);
    const char *name = owner_name_get();
    lv_label_set_text(s_greet_name_lbl, name);
    /* Auto-fit: start with 56 px cursive, drop to 40 px if measured
     * width would overflow the greeting column. See pick_owner_font(). */
    lv_obj_set_style_text_font(s_greet_name_lbl, pick_owner_font(name), 0);
    lv_obj_set_style_text_color(s_greet_name_lbl, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_letter_space(s_greet_name_lbl, 0, 0);
    /* Safety net: if someone types an absurdly long name that still
     * exceeds the column at 40 px, truncate with an ellipsis instead
     * of letting the label crawl off the screen. Width constraint is
     * what makes LV_LABEL_LONG_DOT actually kick in. */
    lv_obj_set_width(s_greet_name_lbl, GREET_NAME_MAX_W);
    lv_label_set_long_mode(s_greet_name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_greet_name_lbl, greet_x, 210);
    /* clear our pointer if the label is destroyed (tab switch) */
    lv_obj_add_event_cb(s_greet_name_lbl, on_greet_name_deleted,
                        LV_EVENT_DELETE, NULL);

    build_quick_buttons(content, greet_x);

    /* subscribe for live updates when maintenance tab renames */
    owner_name_listen(on_owner_changed);

    ESP_LOGI(TAG, "overview built, owner=\"%s\"", owner_name_get());
}
