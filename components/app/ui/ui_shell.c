#include "ui_shell.h"
#include "ui_theme.h"
#include "ui_tabs.h"
#include "hyperwisor_ota.h"
#include "esp_log.h"
#include <stdio.h>

LV_IMG_DECLARE(logo_chintamani);

static const char *TAG = "ui_shell";

typedef struct {
    const char       *icon;   /* LVGL symbol glyph */
    const char       *label;
    ui_tab_builder_t  build;
} tab_def_t;

static const tab_def_t s_tabs[] = {
    { LV_SYMBOL_LIST,     "Overview",      ui_tab_overview_build     },
    { LV_SYMBOL_SETTINGS, "Control",       ui_tab_control_build      },
    { LV_SYMBOL_TINT,     "Climate",       ui_tab_climate_build      },
    { LV_SYMBOL_AUDIO,    "Entertainment", ui_tab_entertainment_build},
    { LV_SYMBOL_CHARGE,   "Maintenance",   ui_tab_maintenance_build  },
};
#define N_TABS (int)(sizeof(s_tabs)/sizeof(s_tabs[0]))
#define DEFAULT_TAB 0  /* Overview */

static lv_obj_t *s_content;
static lv_obj_t *s_tab_btns[N_TABS];
static int       s_active = -1;

/* ---------- helpers ---------- */
static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

static void paint_tab_state(int idx, bool active)
{
    lv_obj_t *b    = s_tab_btns[idx];
    lv_obj_t *icon = lv_obj_get_child(b, 0);
    lv_obj_t *lbl  = lv_obj_get_child(b, 1);
    lv_color_t c = active ? hex(UI_COLOR_TAB) : hex(UI_COLOR_MUTED);
    lv_obj_set_style_text_color(icon, c, 0);
    lv_obj_set_style_text_color(lbl,  c, 0);
}

static void switch_tab(int idx)
{
    if (idx < 0 || idx >= N_TABS) return;
    if (idx == s_active)          return;

    if (s_active >= 0) paint_tab_state(s_active, false);
    s_active = idx;
    paint_tab_state(idx, true);

    /* swap content */
    lv_obj_clean(s_content);
    if (s_tabs[idx].build) s_tabs[idx].build(s_content);
    ESP_LOGI(TAG, "tab -> %s", s_tabs[idx].label);
}

static void on_tab_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch_tab(idx);
}

/* ---------- topbar (logo-only) ---------- */
static void build_topbar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 800, UI_TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, hex(UI_COLOR_SHELL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, hex(UI_COLOR_STROKE), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* small CHINTAMANI logo image in top-left corner.
     * Source is 500x91 -> zoom 73 shrinks it to ~143x26, a compact
     * mark that hugs the corner without dominating the bar.
     * Default pivot is source-centre which throws the zoomed output
     * back toward the middle of the bar; pin pivot to (0,0) so the
     * rendered image anchors to the object's top-left, then place
     * the object at (14, 11) = 14 px from left, vertically centered
     * in the 48 px bar. */
    lv_obj_t *logo = lv_img_create(bar);
    lv_img_set_src(logo, &logo_chintamani);
    lv_img_set_pivot(logo, 0, 0);
    lv_img_set_zoom(logo, 73);
    lv_obj_set_pos(logo, 14, 11);
}

/* ---------- bottom tab bar ---------- */
static lv_obj_t *make_tab_button(lv_obj_t *parent, int idx, int x, int w)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, UI_TABBAR_H);
    lv_obj_set_pos(btn, x, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_tab_clicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, s_tabs[idx].icon);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, s_tabs[idx].label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 48, 0);

    return btn;
}

static void build_tabbar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 800, UI_TABBAR_H);
    lv_obj_set_pos(bar, 0, 480 - UI_TABBAR_H);
    lv_obj_set_style_bg_color(bar, hex(UI_COLOR_SHELL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, hex(UI_COLOR_STROKE), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    int tab_w = 800 / N_TABS;
    for (int i = 0; i < N_TABS; i++) {
        s_tab_btns[i] = make_tab_button(bar, i, i * tab_w, tab_w);
        paint_tab_state(i, false);
    }
}

/* ---------- entry ---------- */
/* ===================================================================
 *  OTA overlay
 *
 *  An update is pushed from the cloud, so it can start while the user
 *  is on any tab -- and it ends in a reboot. They need to be told what
 *  is happening and, more to the point, not to cut power halfway
 *  through. Built on the top layer so it covers the tab bar too, and
 *  polled rather than pushed so the UI owns its own repaint timing.
 * =================================================================== */
static lv_obj_t   *s_ota_overlay;
static lv_obj_t   *s_ota_pct_lbl;
static lv_obj_t   *s_ota_bar;
static lv_timer_t *s_ota_timer;

static void ota_overlay_build(void)
{
    s_ota_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ota_overlay);
    lv_obj_set_size(s_ota_overlay, 800, 480);
    lv_obj_set_pos(s_ota_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_ota_overlay, hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ota_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ota_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_ota_overlay);
    lv_label_set_text(title, "Updating firmware");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -70);

    s_ota_pct_lbl = lv_label_create(s_ota_overlay);
    lv_label_set_text(s_ota_pct_lbl, "0%");
    lv_obj_set_style_text_font(s_ota_pct_lbl, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_ota_pct_lbl, hex(UI_COLOR_TAB), 0);
    lv_obj_align(s_ota_pct_lbl, LV_ALIGN_CENTER, 0, -20);

    s_ota_bar = lv_bar_create(s_ota_overlay);
    lv_obj_set_size(s_ota_bar, 420, 8);
    lv_obj_align(s_ota_bar, LV_ALIGN_CENTER, 0, 30);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_bar_set_value(s_ota_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ota_bar, hex(UI_COLOR_SURFACE_LO), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_bar, hex(UI_COLOR_TAB), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ota_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ota_bar, 4, LV_PART_INDICATOR);

    lv_obj_t *warn = lv_label_create(s_ota_overlay);
    lv_label_set_text(warn,
        "Do not switch off the power.\n"
        "The display will restart on its own when the update finishes.");
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(warn, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(warn, LV_ALIGN_CENTER, 0, 80);
}

static void ota_overlay_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_ota_overlay) return;
    int pct = hyperwisor_ota_get_progress();

    if (pct < 0) {
        if (!lv_obj_has_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "OTA overlay down");
        }
        return;
    }

    if (lv_obj_has_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "OTA overlay up");
    }
    /* pct is 0..100 from the OTA module, but the compiler only knows it
     * is an int -- size the buffer for the full range so the truncation
     * warning stays honest rather than suppressed. */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_ota_pct_lbl, buf);
    lv_bar_set_value(s_ota_bar, pct, LV_ANIM_OFF);
}

void ui_shell_build(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_topbar(scr);
    build_tabbar(scr);

    /* content container between topbar and tabbar */
    s_content = lv_obj_create(scr);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, 800, 480 - UI_TOPBAR_H - UI_TABBAR_H);
    lv_obj_set_pos(s_content, 0, UI_TOPBAR_H);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    s_active = -1;
    switch_tab(DEFAULT_TAB);

    /* Lives for the life of the UI -- the shell is never torn down, so
     * no delete hook is needed here (unlike the per-tab timers). */
    ota_overlay_build();
    s_ota_timer = lv_timer_create(ota_overlay_tick, 500, NULL);
}
