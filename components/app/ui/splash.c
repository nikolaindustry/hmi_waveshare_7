#include "splash.h"
#include "lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "splash";

#define SPLASH_BG_HEX        0x121212
#define SPLASH_TAGLINE_HEX   0x9E9E9E   /* muted grey for "powered by" line */

static splash_done_cb_t s_on_done = NULL;
static lv_obj_t        *s_scr     = NULL;

/* Timer callback: build the main UI on a fresh screen, then animate
 * the swap (the old splash screen is deleted by auto_del=true).
 * Runs inside lv_task_handler(), so the LVGL port mutex is already held. */
static void splash_timer_cb(lv_timer_t *t)
{
    splash_done_cb_t cb = s_on_done;
    s_on_done = NULL;

    /* 1. Create the NEW screen that the main UI will live on */
    lv_obj_t *next_scr = lv_obj_create(NULL);

    /* 2. Let the app build its widgets on that new screen FIRST,
     *    while the old splash screen is still active. */
    if (cb) {
        cb(next_scr);
    }

    /* 3. Now swap in the fully-built screen with a fade-in anim,
     *    and auto-delete the splash (which is lv_scr_act() right now). */
    lv_scr_load_anim(next_scr, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, true);
    s_scr = NULL;

    lv_timer_del(t);
}

void splash_show(uint32_t duration_ms, splash_done_cb_t on_done)
{
    s_on_done = on_done;

    /* Use the currently active screen so we don't need an extra load */
    lv_obj_t *scr = lv_scr_act();
    s_scr = scr;

    /* Dark background */
    lv_obj_set_style_bg_color(scr, lv_color_hex(SPLASH_BG_HEX), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered logo */
    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, &logo_chintamani);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -20);

    /* "powered by nikolaindustry" footer */
    lv_obj_t *tag = lv_label_create(scr);
    lv_label_set_text(tag, "powered by nikolaindustry");
    lv_obj_set_style_text_color(tag, lv_color_hex(SPLASH_TAGLINE_HEX), 0);
    lv_obj_set_style_text_font(tag, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(tag, 2, 0);
    lv_obj_align(tag, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* Schedule the dismiss */
    lv_timer_t *t = lv_timer_create(splash_timer_cb, duration_ms, NULL);
    lv_timer_set_repeat_count(t, 1);

    ESP_LOGI(TAG, "splash shown for %lu ms", (unsigned long)duration_ms);
}
