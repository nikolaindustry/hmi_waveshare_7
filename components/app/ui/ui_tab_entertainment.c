/* ui_tab_entertainment.c
 *
 * Mocked Bluetooth media player for the Entertainment tab.
 *
 *   +-------------------------+   NOW PLAYING
 *   |                         |   Midnight Drive
 *   |        [music]          |   The Highway Lights
 *   |                         |   |========-------|
 *   |       BLUETOOTH         |   1:23        4:05
 *   +-------------------------+
 *    [BT ] [USB] [FM ] [AUX ]         [<<] ( > ) [>>]
 *                                 volume |========-----|  60%
 *
 * All state lives in module statics. A 1 Hz LVGL timer advances the
 * elapsed counter while "playing". Cleanup on tab switch is driven
 * by an LV_EVENT_DELETE on the title label (the content container is
 * cleaned by the shell before each build).
 */

#include "ui_tabs.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "ui_tab_ent";

/* ---- Mock track list -------------------------------------------- */
typedef struct { const char *title; const char *artist; int seconds; } track_t;
static const track_t TRACKS[] = {
    {"Midnight Drive",    "The Highway Lights", 245},
    {"Leather & Lace",    "Saddle Road",        198},
    {"Desert Horizon",    "Atlas Bloom",        272},
    {"Coffee & Gasoline", "Night Ferry",        215},
};
#define N_TRACKS ((int)(sizeof(TRACKS)/sizeof(TRACKS[0])))

static const char *SRC_LABELS[] = {"BLUETOOTH", "USB", "FM", "AUX"};
static const char *SRC_PILLS[]  = {"BT", "USB", "FM", "AUX"};

/* ---- Persistent (across tab switches) state --------------------- */
static int  s_track_idx = 0;
static int  s_elapsed   = 0;
static bool s_playing   = false;
static int  s_src_idx   = 0;
static int  s_volume    = 60;

/* ---- Widget handles (rebuilt on every tab entry) ---------------- */
static lv_obj_t *s_title_lbl      = NULL;
static lv_obj_t *s_artist_lbl     = NULL;
static lv_obj_t *s_time_cur       = NULL;
static lv_obj_t *s_time_end       = NULL;
static lv_obj_t *s_progress       = NULL;
static lv_obj_t *s_play_icon      = NULL;
static lv_obj_t *s_vol_slider     = NULL;
static lv_obj_t *s_vol_val        = NULL;
static lv_obj_t *s_src_pills[4]   = {0};
static lv_obj_t *s_src_pill_lbls[4] = {0};
static lv_obj_t *s_art_src_lbl    = NULL;
static lv_timer_t *s_tick         = NULL;

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

static void fmt_mmss(int secs, char *buf, size_t n) {
    int m = secs / 60, s = secs % 60;
    snprintf(buf, n, "%d:%02d", m, s);
}

/* ---- Painters --------------------------------------------------- */
static void paint_track(void) {
    if (!s_title_lbl) return;
    const track_t *t = &TRACKS[s_track_idx];
    lv_label_set_text(s_title_lbl, t->title);
    lv_label_set_text(s_artist_lbl, t->artist);
    lv_slider_set_range(s_progress, 0, t->seconds);
    lv_slider_set_value(s_progress, s_elapsed, LV_ANIM_OFF);
    char b[16];
    fmt_mmss(s_elapsed, b, sizeof(b));       lv_label_set_text(s_time_cur, b);
    fmt_mmss(t->seconds, b, sizeof(b));      lv_label_set_text(s_time_end, b);
}

static void paint_sources(void) {
    for (int i = 0; i < 4; i++) {
        bool on = (i == s_src_idx);
        if (!s_src_pills[i]) continue;
        lv_obj_set_style_bg_color(s_src_pills[i],
            hex(on ? UI_COLOR_TAB : UI_COLOR_SURFACE_LO), 0);
        lv_obj_set_style_text_color(s_src_pill_lbls[i],
            hex(on ? UI_COLOR_SHELL : UI_COLOR_MUTED), 0);
    }
    if (s_art_src_lbl) lv_label_set_text(s_art_src_lbl, SRC_LABELS[s_src_idx]);
}

static void paint_play_icon(void) {
    if (s_play_icon)
        lv_label_set_text(s_play_icon,
            s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

/* ---- Timer / event callbacks ------------------------------------ */
static void tick_cb(lv_timer_t *t) {
    (void)t;
    if (!s_playing || !s_title_lbl) return;
    s_elapsed++;
    if (s_elapsed >= TRACKS[s_track_idx].seconds) {
        s_elapsed = 0;
        s_track_idx = (s_track_idx + 1) % N_TRACKS;
        paint_track();
    } else {
        lv_slider_set_value(s_progress, s_elapsed, LV_ANIM_OFF);
        char b[16]; fmt_mmss(s_elapsed, b, sizeof(b));
        lv_label_set_text(s_time_cur, b);
    }
}

static void on_play_pause(lv_event_t *e) {
    (void)e;
    s_playing = !s_playing;
    paint_play_icon();
    ESP_LOGI(TAG, "%s", s_playing ? "play" : "pause");
}

static void on_prev(lv_event_t *e) {
    (void)e;
    s_elapsed = 0;
    s_track_idx = (s_track_idx + N_TRACKS - 1) % N_TRACKS;
    paint_track();
}

static void on_next(lv_event_t *e) {
    (void)e;
    s_elapsed = 0;
    s_track_idx = (s_track_idx + 1) % N_TRACKS;
    paint_track();
}

static void on_src_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_src_idx = idx;
    paint_sources();
    ESP_LOGI(TAG, "source -> %s", SRC_LABELS[idx]);
}

static void on_progress_change(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    s_elapsed = lv_slider_get_value(sl);
    char b[16]; fmt_mmss(s_elapsed, b, sizeof(b));
    if (s_time_cur) lv_label_set_text(s_time_cur, b);
}

static void on_volume_change(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    s_volume = lv_slider_get_value(sl);
    char b[8]; snprintf(b, sizeof(b), "%d%%", s_volume);
    if (s_vol_val) lv_label_set_text(s_vol_val, b);
}

static void on_destroy(lv_event_t *e) {
    (void)e;
    if (s_tick) { lv_timer_del(s_tick); s_tick = NULL; }
    s_title_lbl = s_artist_lbl = s_time_cur = s_time_end = NULL;
    s_progress = s_play_icon = s_vol_slider = s_vol_val = NULL;
    s_art_src_lbl = NULL;
    for (int i = 0; i < 4; i++) {
        s_src_pills[i] = NULL;
        s_src_pill_lbls[i] = NULL;
    }
}

/* ---- Widget builders -------------------------------------------- */
static lv_obj_t *make_pill(lv_obj_t *parent, int w, int h,
                           const char *text, lv_obj_t **out_lbl) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, hex(UI_COLOR_SURFACE_LO), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, h/2, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, hex(UI_COLOR_STROKE), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_center(lbl);
    if (out_lbl) *out_lbl = lbl;
    return btn;
}

static lv_obj_t *make_round_btn(lv_obj_t *parent, int size, const char *glyph,
                                const lv_font_t *font, lv_obj_t **out_lbl) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_bg_color(btn, hex(UI_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, size/2, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, hex(UI_COLOR_STROKE), 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, glyph);
    lv_obj_set_style_text_color(lbl, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(lbl, font ? font : &lv_font_montserrat_24, 0);
    lv_obj_center(lbl);
    if (out_lbl) *out_lbl = lbl;
    return btn;
}

/* ---- Entry point ------------------------------------------------ */
void ui_tab_entertainment_build(lv_obj_t *content)
{
    /* ========================= LEFT: ALBUM ART CARD ========================= */
    lv_obj_t *art = lv_obj_create(content);
    lv_obj_remove_style_all(art);
    lv_obj_set_size(art, 296, 260);
    lv_obj_set_pos(art, 16, 16);
    lv_obj_set_style_bg_color(art, hex(UI_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(art, UI_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(art, 1, 0);
    lv_obj_set_style_border_color(art, hex(UI_COLOR_STROKE), 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);

    /* Large centred music glyph (brand accent) */
    lv_obj_t *g = lv_label_create(art);
    lv_label_set_text(g, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(g, hex(UI_COLOR_ACCENT), 0);
    lv_obj_align(g, LV_ALIGN_CENTER, 0, -24);

    /* Source name directly under the glyph */
    s_art_src_lbl = lv_label_create(art);
    lv_label_set_text(s_art_src_lbl, SRC_LABELS[s_src_idx]);
    lv_obj_set_style_text_font(s_art_src_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_art_src_lbl, hex(UI_COLOR_TAB), 0);
    lv_obj_set_style_text_letter_space(s_art_src_lbl, 4, 0);
    lv_obj_align(s_art_src_lbl, LV_ALIGN_CENTER, 0, 36);

    /* "CHINTAMANI SOUND" eyebrow at top of card */
    lv_obj_t *brand = lv_label_create(art);
    lv_label_set_text(brand, "CHINTAMANI SOUND");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(brand, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(brand, 5, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 18);

    /* ========================= SOURCE PILL BAR =========================== */
    const int pw = 68, ph = 40, gap = 8, py = 292;
    for (int i = 0; i < 4; i++) {
        s_src_pills[i] = make_pill(content, pw, ph, SRC_PILLS[i],
                                   &s_src_pill_lbls[i]);
        lv_obj_set_pos(s_src_pills[i], 16 + i*(pw+gap), py);
        lv_obj_add_event_cb(s_src_pills[i], on_src_clicked,
                            LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    /* ========================= RIGHT: NOW PLAYING ======================= */
    const int rx = 336;

    lv_obj_t *eyebrow = lv_label_create(content);
    lv_label_set_text(eyebrow, "NOW PLAYING");
    lv_obj_set_style_text_font(eyebrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(eyebrow, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(eyebrow, 6, 0);
    lv_obj_set_pos(eyebrow, rx, 18);

    s_title_lbl = lv_label_create(content);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_title_lbl, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_title_lbl, rx, 36);
    /* cleanup hook - runs when shell cleans the content container */
    lv_obj_add_event_cb(s_title_lbl, on_destroy, LV_EVENT_DELETE, NULL);

    s_artist_lbl = lv_label_create(content);
    lv_obj_set_style_text_font(s_artist_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_artist_lbl, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(s_artist_lbl, rx, 72);

    /* Progress slider */
    s_progress = lv_slider_create(content);
    lv_obj_set_size(s_progress, 432, 8);
    lv_obj_set_pos(s_progress, rx, 112);
    lv_obj_set_style_bg_color(s_progress, hex(UI_COLOR_SURFACE_LO), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress, hex(UI_COLOR_TAB), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress, hex(UI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_progress, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(s_progress, on_progress_change, LV_EVENT_VALUE_CHANGED, NULL);

    s_time_cur = lv_label_create(content);
    lv_obj_set_style_text_font(s_time_cur, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time_cur, hex(UI_COLOR_MUTED), 0);

    s_time_end = lv_label_create(content);
    lv_obj_set_style_text_font(s_time_end, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time_end, hex(UI_COLOR_MUTED), 0);

    /* Anchor the time labels to the progress bar ends */
    lv_obj_align_to(s_time_cur, s_progress, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    lv_obj_align_to(s_time_end, s_progress, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 6);

    /* Transport row - prev / PLAY / next */
    const int ty = 168;
    lv_obj_t *prev_b = make_round_btn(content, 56, LV_SYMBOL_PREV,
                                      &lv_font_montserrat_16, NULL);
    lv_obj_set_pos(prev_b, rx + 112, ty + 12);
    lv_obj_add_event_cb(prev_b, on_prev, LV_EVENT_CLICKED, NULL);

    lv_obj_t *play_b = make_round_btn(content, 80, LV_SYMBOL_PLAY,
                                      &lv_font_montserrat_24, &s_play_icon);
    lv_obj_set_style_bg_color(play_b, hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_text_color(s_play_icon, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(play_b, rx + 188, ty);
    lv_obj_add_event_cb(play_b, on_play_pause, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next_b = make_round_btn(content, 56, LV_SYMBOL_NEXT,
                                      &lv_font_montserrat_16, NULL);
    lv_obj_set_pos(next_b, rx + 288, ty + 12);
    lv_obj_add_event_cb(next_b, on_next, LV_EVENT_CLICKED, NULL);

    /* Volume row */
    const int vy = 292;
    lv_obj_t *vicon = lv_label_create(content);
    lv_label_set_text(vicon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(vicon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(vicon, hex(UI_COLOR_MUTED), 0);
    lv_obj_set_pos(vicon, rx, vy + 2);

    s_vol_slider = lv_slider_create(content);
    lv_obj_set_size(s_vol_slider, 340, 8);
    lv_obj_set_pos(s_vol_slider, rx + 30, vy + 10);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_slider_set_value(s_vol_slider, s_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol_slider, hex(UI_COLOR_SURFACE_LO), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, hex(UI_COLOR_TAB), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, hex(UI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_slider, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, on_volume_change, LV_EVENT_VALUE_CHANGED, NULL);

    s_vol_val = lv_label_create(content);
    lv_obj_set_style_text_font(s_vol_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_vol_val, hex(UI_COLOR_TEXT), 0);
    lv_obj_set_pos(s_vol_val, rx + 380, vy + 2);
    char vb[8]; snprintf(vb, sizeof(vb), "%d%%", s_volume);
    lv_label_set_text(s_vol_val, vb);

    /* Paint initial state */
    paint_track();
    paint_sources();
    paint_play_icon();

    /* 1 Hz tick - advances elapsed counter while playing */
    if (!s_tick) s_tick = lv_timer_create(tick_cb, 1000, NULL);

    ESP_LOGI(TAG, "entertainment built, src=%s track=\"%s\"",
             SRC_LABELS[s_src_idx], TRACKS[s_track_idx].title);
}
