#include "hds_widget.h"
#include "hds.h"
#include "hds_assets.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hds_widget";

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

/* =======================================================================
 *  Shared helpers
 * ======================================================================= */

const lv_font_t *hds_font(uint16_t px)
{
    /* Must stay in step with the faces enabled in sdkconfig
     * (CONFIG_LV_FONT_MONTSERRAT_*) and sim/lv_conf.h, and with
     * FONT_SIZES in tools/hds_transcode.py. */
    if (px >= 40) return &lv_font_montserrat_40;
    if (px >= 24) return &lv_font_montserrat_24;
    if (px >= 20) return &lv_font_montserrat_20;
    if (px >= 16) return &lv_font_montserrat_16;
    if (px >= 14) return &lv_font_montserrat_14;
    return &lv_font_montserrat_12;
}

static hds_palette_t s_pal;

const hds_palette_t *hds_palette(void) { return &s_pal; }

void hds_palette_set(uint32_t bg, bool has_bg)
{
    /* No background in the spec: the dashboard inherits the cabin
     * theme, same as the hand-built tabs. */
    if (!has_bg) {
        s_pal = (hds_palette_t){
            .surface    = UI_COLOR_SURFACE,
            .surface_lo = UI_COLOR_SURFACE_LO,
            .stroke     = UI_COLOR_STROKE,
            .text       = UI_COLOR_TEXT,
            .muted      = UI_COLOR_MUTED,
            .accent     = UI_COLOR_ACCENT,
        };
        return;
    }

    /* Rec.601 luma -- good enough to tell a light canvas from a dark
     * one, which is the only decision being made here. */
    uint32_t r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    uint32_t luma = (r * 299 + g * 587 + b * 114) / 1000;

    if (luma >= 140) {
        s_pal = (hds_palette_t){
            .surface    = 0xF2F4F7,
            .surface_lo = 0xE3E7ED,
            .stroke     = 0xD4D9E2,
            .text       = 0x1B1F24,
            .muted      = 0x6B7280,
            .accent     = UI_COLOR_ACCENT,
        };
    } else {
        s_pal = (hds_palette_t){
            .surface    = 0x242933,
            .surface_lo = 0x1A1E26,
            .stroke     = 0x343B47,
            .text       = 0xF2F4F7,
            .muted      = 0x9AA2B1,
            .accent     = UI_COLOR_ACCENT,
        };
    }
}

void hds_style_container(lv_obj_t *obj, bool boxed)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    /* Containers are inert: the interactive part of a widget is its
     * child (the toggle, the knob, the button). Dashboards routinely
     * lay artwork over a transparent navigation region, and an lv_obj
     * is clickable by default, so leaving this set makes a logo eat the
     * tap meant for the hit area underneath it. Navigation widgets add
     * the flag back for themselves. */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    if (!boxed) {
        return;
    }
    lv_obj_set_style_bg_color(obj, hex(s_pal.surface), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, UI_CARD_RADIUS / 2, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, hex(s_pal.stroke), 0);
    lv_obj_set_style_pad_all(obj, UI_PAD_S, 0);
}

/* A widget's own colour wins; otherwise fall back to the app theme so a
 * dashboard authored against CSS variables still looks deliberate. */
static lv_color_t accent_of(const hds_widget_t *w)
{
    return hex(w->has_fg ? w->fg : s_pal.accent);
}

static lv_color_t text_of(const hds_widget_t *w)
{
    return hex(w->has_fg ? w->fg : s_pal.text);
}

/* =======================================================================
 *  label
 * ======================================================================= */

static lv_obj_t *create_label(lv_obj_t *parent, hds_widget_t *w)
{
    lv_obj_t *root = lv_obj_create(parent);
    hds_style_container(root, w->box);
    lv_obj_set_pos(root, w->x, w->y);
    lv_obj_set_size(root, w->w, w->h);

    lv_obj_t *lbl = lv_label_create(root);
    lv_label_set_text(lbl, w->name ? w->name : "");
    lv_obj_set_style_text_font(lbl, hds_font(w->font), 0);
    lv_obj_set_style_text_color(lbl, text_of(w), 0);
    /* The box came from a web layout, so the text may legitimately need
     * more than one line; wrap inside it rather than clipping. */
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    w->aux = lbl;
    return root;
}

static void label_set_value(hds_widget_t *w, const cJSON *v)
{
    if (!w->aux) {
        return;
    }
    if (cJSON_IsString(v)) {
        lv_label_set_text(w->aux, v->valuestring);
    } else if (cJSON_IsNumber(v)) {
        char buf[32];
        double d = v->valuedouble;
        /* Integers read better without a trailing ".00" on a panel. */
        if (d == (double)(long)d) {
            snprintf(buf, sizeof(buf), "%ld", (long)d);
        } else {
            snprintf(buf, sizeof(buf), "%.2f", d);
        }
        lv_label_set_text(w->aux, buf);
    } else if (cJSON_IsBool(v)) {
        lv_label_set_text(w->aux, cJSON_IsTrue(v) ? "ON" : "OFF");
    }
}

/* =======================================================================
 *  switch
 * ======================================================================= */

static void switch_cb(lv_event_t *e)
{
    hds_widget_t *w = lv_event_get_user_data(e);
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    /* The builder splits a toggle into separate "on" and "off" actions
     * rather than one event carrying a boolean, so the state is already
     * expressed by which one fires. */
    hds_widget_fire(w, on ? "on" : "off", NULL);
}

static lv_obj_t *create_switch(lv_obj_t *parent, hds_widget_t *w)
{
    lv_obj_t *root = lv_obj_create(parent);
    hds_style_container(root, w->box);
    lv_obj_set_pos(root, w->x, w->y);
    lv_obj_set_size(root, w->w, w->h);

    /* Toggle sized off the row height so it stays proportional whatever
     * scale the transcoder picked, clamped to something touchable. */
    lv_coord_t sw_h = LV_CLAMP(18, w->h - 14, 30);
    lv_coord_t sw_w = sw_h * 2;

    lv_obj_t *sw = lv_switch_create(root);
    lv_obj_set_size(sw, sw_w, sw_h);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, accent_of(w), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, hex(s_pal.surface_lo), LV_PART_MAIN);
    if (w->vdef) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, w);

    if (w->name && w->name[0]) {
        lv_obj_t *lbl = lv_label_create(root);
        lv_label_set_text(lbl, w->name);
        lv_obj_set_style_text_font(lbl, hds_font(w->font), 0);
        lv_obj_set_style_text_color(lbl, hex(s_pal.text), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, w->w - sw_w - UI_PAD_S * 4);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }

    w->aux = sw;
    return root;
}

static void switch_set_value(hds_widget_t *w, const cJSON *v)
{
    if (!w->aux) {
        return;
    }
    bool on = cJSON_IsBool(v) ? cJSON_IsTrue(v)
            : cJSON_IsNumber(v) ? (v->valuedouble != 0)
            : false;
    /* Reflect the device's actual state without echoing a command back
     * out -- this is the read-back half of the loop. */
    if (on) {
        lv_obj_add_state(w->aux, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(w->aux, LV_STATE_CHECKED);
    }
}

/* =======================================================================
 *  slider
 * ======================================================================= */

static void slider_cb(lv_event_t *e)
{
    hds_widget_t *w = lv_event_get_user_data(e);
    int32_t v = lv_slider_get_value(lv_event_get_target(e));

    if (w->aux) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)v);
        lv_label_set_text(w->aux, buf);
    }

    /* "valueChange" is the action; it matches events the author wrote
     * as "change", "slide" or "slideEnd". The position is injected over
     * whichever placeholder key the payload uses. */
    hds_widget_fire(w, "valueChange", cJSON_CreateNumber(v));
}

static lv_obj_t *create_slider(lv_obj_t *parent, hds_widget_t *w)
{
    lv_obj_t *root = lv_obj_create(parent);
    hds_style_container(root, w->box);
    lv_obj_set_pos(root, w->x, w->y);
    lv_obj_set_size(root, w->w, w->h);

    lv_coord_t val_w = w->showval ? 44 : 0;

    lv_obj_t *sl = lv_slider_create(root);
    lv_slider_set_range(sl, w->vmin, w->vmax);
    lv_slider_set_value(sl, w->vdef, LV_ANIM_OFF);
    lv_obj_set_width(sl, w->w - val_w - (w->box ? UI_PAD_S * 4 : UI_PAD_S));
    lv_obj_set_height(sl, w->track ? w->track : 8);
    lv_obj_align(sl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_set_style_bg_color(sl, hex(s_pal.surface_lo), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, accent_of(w), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, accent_of(w), LV_PART_KNOB);
    /* Grow the knob past the track so a thin web-styled slider is still
     * draggable with a fingertip. */
    lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);

    if (w->showval) {
        lv_obj_t *lbl = lv_label_create(root);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)w->vdef);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, hds_font(12), 0);
        lv_obj_set_style_text_color(lbl, hex(s_pal.muted), 0);
        lv_obj_align(lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        w->aux = lbl;
    }

    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_RELEASED, w);
    return root;
}

static void slider_set_value(hds_widget_t *w, const cJSON *v)
{
    if (!cJSON_IsNumber(v) || !w->obj) {
        return;
    }
    /* w->obj is the container; the slider is its first child. */
    lv_obj_t *sl = lv_obj_get_child(w->obj, 0);
    if (sl) {
        lv_slider_set_value(sl, (int32_t)v->valuedouble, LV_ANIM_ON);
    }
    if (w->aux) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)v->valuedouble);
        lv_label_set_text(w->aux, buf);
    }
}

/* =======================================================================
 *  image
 * ======================================================================= */

static lv_obj_t *create_image(lv_obj_t *parent, hds_widget_t *w)
{
    const lv_img_dsc_t *dsc = hds_assets_get(w->asset);

    lv_obj_t *root = lv_obj_create(parent);
    hds_style_container(root, w->box);
    lv_obj_set_pos(root, w->x, w->y);
    lv_obj_set_size(root, w->w, w->h);

    if (!dsc) {
        /* Draw the gap rather than leaving a hole -- a missing asset
         * should be obvious when laying a dashboard out, not silent. */
        lv_obj_set_style_border_width(root, 1, 0);
        lv_obj_set_style_border_color(root, hex(UI_COLOR_WARN), 0);
        lv_obj_t *lbl = lv_label_create(root);
        lv_label_set_text_fmt(lbl, "%s?", w->asset ? w->asset : "no image");
        lv_obj_set_style_text_font(lbl, hds_font(12), 0);
        lv_obj_set_style_text_color(lbl, hex(UI_COLOR_WARN), 0);
        lv_obj_center(lbl);
        ESP_LOGW(TAG, "widget %s: asset '%s' unavailable",
                 w->id, w->asset ? w->asset : "(none)");
        return root;
    }

    lv_obj_t *img = lv_img_create(root);
    lv_img_set_src(img, dsc);
    lv_obj_center(img);
    w->aux = img;
    return root;
}

/* =======================================================================
 *  navigate-page
 * ======================================================================= */

static void nav_cb(lv_event_t *e)
{
    hds_widget_t *w = lv_event_get_user_data(e);
    hds_widget_fire(w, "click", NULL);
    if (w->go) {
        hds_goto(w->go);
    }
}

static lv_obj_t *create_nav(lv_obj_t *parent, hds_widget_t *w)
{
    lv_obj_t *root = lv_obj_create(parent);
    hds_style_container(root, false);
    lv_obj_set_pos(root, w->x, w->y);
    lv_obj_set_size(root, w->w, w->h);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);

    if (w->box && w->has_bg) {
        lv_obj_set_style_bg_color(root, hex(w->bg), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(root, UI_CARD_RADIUS / 2, 0);
    }

    /* Dashboards routinely use a transparent region sitting behind
     * artwork as the tap target, so a nav widget often has nothing to
     * draw. Label it only when the author gave it a real title. */
    bool named = w->name && w->name[0] &&
                 strncmp(w->name, "New ", 4) != 0;
    if (named || w->icon) {
        lv_obj_t *lbl = lv_label_create(root);
        lv_label_set_text_fmt(lbl, "%s%s",
                              w->icon ? LV_SYMBOL_RIGHT " " : "",
                              named ? w->name : "");
        lv_obj_set_style_text_font(lbl, hds_font(w->font), 0);
        lv_obj_set_style_text_color(lbl, text_of(w), 0);
        lv_obj_center(lbl);
    }

    if (hds_get_debug_hitboxes()) {
        lv_obj_set_style_border_width(root, 1, 0);
        lv_obj_set_style_border_color(root, hex(s_pal.accent), 0);
        lv_obj_set_style_border_opa(root, LV_OPA_50, 0);
    }

    lv_obj_add_event_cb(root, nav_cb, LV_EVENT_CLICKED, w);
    return root;
}

/* =======================================================================
 *  form
 *
 *  One shared keyboard is parented to the page root and raised when a
 *  field takes focus, so a form works on a panel with no hardware keys.
 * ======================================================================= */

static lv_obj_t *s_kb;

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb, NULL);
    }
}

static void field_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!s_kb) {
        return;
    }
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void field_defocus_cb(lv_event_t *e)
{
    (void)e;
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb, NULL);
    }
}

static void submit_cb(lv_event_t *e)
{
    hds_widget_t *w = lv_event_get_user_data(e);

    /* Collect the fields into the outgoing params. The builder's field
     * labels are the parameter names the firmware handler expects
     * ("sensorIndex", "targetTemp", ...), so they are used as keys; a
     * field left blank falls back to its placeholder. */
    cJSON *fields = cJSON_CreateObject();

    /* The textareas sit in creation order under the scroll body, which
     * is w->aux. Walk them alongside the field list. */
    if (w->aux) {
        uint32_t n = lv_obj_get_child_cnt(w->aux);
        int fi = 0;
        for (uint32_t c = 0; c < n && fi < w->n_fields; c++) {
            lv_obj_t *child = lv_obj_get_child(w->aux, c);
            if (!lv_obj_check_type(child, &lv_textarea_class)) {
                continue;
            }
            const char *txt = lv_textarea_get_text(child);
            const hds_field_t *f = &w->fields[fi++];
            const char *key = (f->label && f->label[0]) ? f->label : f->id;
            if (!key) {
                continue;
            }
            if (!txt || !txt[0]) {
                txt = f->placeholder;
            }
            if (txt && txt[0]) {
                cJSON_AddStringToObject(fields, key, txt);
            }
        }
    }

    hds_widget_fire(w, "submit", fields);
}

static lv_obj_t *create_form(lv_obj_t *parent, hds_widget_t *w)
{
    lv_obj_t *root = lv_obj_create(parent);
    hds_style_container(root, w->box);
    lv_obj_set_pos(root, w->x, w->y);
    lv_obj_set_size(root, w->w, w->h);

    lv_coord_t y = 0;

    if (w->name && w->name[0]) {
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, w->name);
        lv_obj_set_style_text_font(title, hds_font(16), 0);
        lv_obj_set_style_text_color(title, hex(s_pal.text), 0);
        lv_obj_set_pos(title, 0, 0);
        y = 24;
    }

    const lv_coord_t submit_h = 34;
    lv_coord_t inner_w = w->w - (w->box ? UI_PAD_S * 2 : 0);
    lv_coord_t body_h  = w->h - y - submit_h - UI_PAD_S * 2
                       - (w->box ? UI_PAD_S * 2 : 0);

    /* Eight fields rarely fit a 7" panel at the scale a web canvas was
     * authored at, so the field list scrolls and the submit button
     * stays pinned below it. */
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, y);
    lv_obj_set_size(body, inner_w, body_h > 40 ? body_h : 40);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 4, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < w->n_fields; i++) {
        const hds_field_t *f = &w->fields[i];

        lv_obj_t *lbl = lv_label_create(body);
        lv_label_set_text(lbl, f->label ? f->label : "");
        lv_obj_set_style_text_font(lbl, hds_font(12), 0);
        lv_obj_set_style_text_color(lbl, hex(s_pal.muted), 0);

        lv_obj_t *ta = lv_textarea_create(body);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, f->placeholder ? f->placeholder : "");
        if (f->def && f->def[0]) {
            lv_textarea_set_text(ta, f->def);
        }
        lv_obj_set_width(ta, lv_pct(96));
        lv_obj_set_style_text_font(ta, hds_font(14), 0);
        lv_obj_set_style_bg_color(ta, hex(s_pal.surface_lo), 0);
        lv_obj_set_style_border_color(ta, hex(s_pal.stroke), 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_text_color(ta, hex(s_pal.text), 0);
        lv_obj_add_event_cb(ta, field_focus_cb, LV_EVENT_FOCUSED, w);
        lv_obj_add_event_cb(ta, field_defocus_cb, LV_EVENT_DEFOCUSED, w);
        /* A touch panel has no focus ring to tab through, so a click is
         * what actually raises the keyboard. */
        lv_obj_add_event_cb(ta, field_focus_cb, LV_EVENT_CLICKED, w);
    }

    lv_obj_t *btn = lv_btn_create(root);
    lv_obj_set_size(btn, inner_w, submit_h);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(btn, hex(s_pal.accent), 0);
    lv_obj_set_style_radius(btn, submit_h / 2, 0);
    lv_obj_add_event_cb(btn, submit_cb, LV_EVENT_CLICKED, w);

    lv_obj_t *blbl = lv_label_create(btn);
    lv_label_set_text(blbl, LV_SYMBOL_OK "  Apply");
    lv_obj_set_style_text_font(blbl, hds_font(14), 0);
    lv_obj_center(blbl);

    w->aux = body;

    /* Build the shared keyboard once, on the page root rather than the
     * form, so it can overlay the whole page. */
    if (!s_kb || lv_obj_get_parent(s_kb) != parent) {
        s_kb = lv_keyboard_create(parent);
        lv_obj_set_size(s_kb, lv_pct(100), lv_pct(45));
        lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
    }

    return root;
}

/* =======================================================================
 *  The table
 * ======================================================================= */

static const hds_vtable_t s_vtables[] = {
    { HDS_W_LABEL,  create_label,  label_set_value  },
    { HDS_W_SWITCH, create_switch, switch_set_value },
    { HDS_W_SLIDER, create_slider, slider_set_value },
    { HDS_W_IMAGE,  create_image,  NULL             },
    { HDS_W_NAV,    create_nav,    NULL             },
    { HDS_W_FORM,   create_form,   NULL             },
};

const hds_vtable_t *hds_vtable_for(hds_wtype_t type)
{
    for (size_t i = 0; i < sizeof(s_vtables) / sizeof(s_vtables[0]); i++) {
        if (s_vtables[i].type == type) {
            return &s_vtables[i];
        }
    }
    return NULL;
}

/* The keyboard lives on the page root, which the renderer deletes on
 * navigation; drop our handle so the next form builds a fresh one. */
void hds_widget_forget_keyboard(void) { s_kb = NULL; }
