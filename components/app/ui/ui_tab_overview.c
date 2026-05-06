#include "ui_tabs.h"
#include "ui_theme.h"
#include "owner_name.h"
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
    lv_obj_set_pos(eyebrow, greet_x, 120);

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
    lv_obj_set_pos(s_greet_name_lbl, greet_x, 134);
    /* clear our pointer if the label is destroyed (tab switch) */
    lv_obj_add_event_cb(s_greet_name_lbl, on_greet_name_deleted,
                        LV_EVENT_DELETE, NULL);

    /* subscribe for live updates when maintenance tab renames */
    owner_name_listen(on_owner_changed);

    ESP_LOGI(TAG, "overview built, owner=\"%s\"", owner_name_get());
}
