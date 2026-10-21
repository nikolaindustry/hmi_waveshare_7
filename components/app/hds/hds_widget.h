#pragma once

/* =====================================================================
 *  hds_widget -- the per-type renderer table.
 *
 *  Adding a widget type is a create function plus one row in the table
 *  at the bottom of hds_widget.c; nothing in hds_render.c changes.
 *
 *    create()     build the LVGL object(s) for this widget inside
 *                 `parent`, already positioned and sized. Attach event
 *                 callbacks via hds_widget_bind(). Returns the root
 *                 object, or NULL to skip the widget.
 *    set_value()  apply an inbound value from the cloud. Optional.
 * ===================================================================== */

#include "hds_spec.h"

typedef struct {
    hds_wtype_t type;
    lv_obj_t *(*create)(lv_obj_t *parent, hds_widget_t *w);
    void      (*set_value)(hds_widget_t *w, const cJSON *value);
} hds_vtable_t;

const hds_vtable_t *hds_vtable_for(hds_wtype_t type);

/* ---- helpers shared by the widget implementations ---- */

/* Nearest built-in Montserrat face to `px`. The transcoder already
 * snapped sizes to this set, so this is normally an exact hit. */
const lv_font_t *hds_font(uint16_t px);

/* Apply the card treatment for the active palette (surface fill,
 * radius, hairline border) or strip all styling when `boxed` is false. */
void hds_style_container(lv_obj_t *obj, bool boxed);

/* ---- palette ----
 *
 * A dashboard authored on a white web canvas looks broken when the
 * cabin theme's dark brown surfaces are dropped onto it, and a dark
 * dashboard looks equally wrong in reverse. The renderer picks a card
 * palette from the page's own background before building it; a page
 * with no background of its own falls through to the app theme.
 */
typedef struct {
    uint32_t surface;
    uint32_t surface_lo;
    uint32_t stroke;
    uint32_t text;
    uint32_t muted;
    uint32_t accent;
} hds_palette_t;

void                hds_palette_set(uint32_t bg, bool has_bg);
const hds_palette_t *hds_palette(void);

/* Fire the widget's events for an action.
 *
 * `action_id` is the builder's action name -- "on", "off", "click",
 * "valueChange", "submit", "press", "release" -- NOT the authored event
 * name. One action can match several authored event types (a
 * "valueChange" matches "change", "slide" and "slideEnd"), which is why
 * this takes the action and does the mapping itself.
 *
 * `value` is what the widget is worth right now, and is injected into
 * the authored payload's placeholders. Pass NULL for widgets that carry
 * no value (a switch's state is already in its two separate events).
 * The call consumes `value`. */
void hds_widget_fire(hds_widget_t *w, const char *action_id, cJSON *value);

/* The shared form keyboard is parented to the page root, which the
 * renderer deletes on navigation. Called from there so the next form
 * builds a fresh one instead of touching a dangling object. */
void hds_widget_forget_keyboard(void);
