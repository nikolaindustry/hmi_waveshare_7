#pragma once

/* =====================================================================
 *  hds -- render a Hyperwisor dashboard from a declarative spec.
 *
 *  A dashboard authored in the Hyperwisor builder is exported as JSON,
 *  run through tools/hds_transcode.py on the host, and dropped next to
 *  the firmware as a ~6 KB spec plus raw-RGB565 image blobs. This module
 *  turns that spec into a live LVGL UI and routes widget interactions
 *  back out as Hyperwisor commands.
 *
 *  Usage:
 *      hds_set_emit(my_sink, NULL);
 *      hds_init("assets/dashboard/dashboard.json",
 *               "assets/dashboard/assets");
 *      hds_build(lv_scr_act());
 *
 *  The spec's events are already in the firmware's own wire format --
 *  payload.commands[].actions[].params, the shape
 *  hyperwisor_cmd_find_params() walks -- so the sink can hand `params`
 *  straight to hyperwisor_emit() with no translation.
 *
 *  What this module deliberately does NOT do: run the export's
 *  JavaScript `script` block. There is no JS engine on the device and
 *  the transcoder drops it, reporting the line count. Behaviour that
 *  lived in that script belongs in a firmware command handler.
 * ===================================================================== */

#include "hds_spec.h"
#include "esp_err.h"

/* Called when a widget is actuated.
 *
 * `payload` is the authored payload with live values already injected --
 * the same object the web dashboard would put on the wire. It is
 * borrowed; copy it if you need it past the call. `target` is the
 * widget's own target when it has one, otherwise the spec-wide default
 * (possibly NULL).
 *
 * A sink that wants to route a command locally digs out the command and
 * action with hds_payload_command() / hds_payload_action(). */
typedef void (*hds_emit_fn)(const char *target,
                            const cJSON *payload,
                            void *user);

/* Convenience accessors for payload.commands[0] -- the shape every
 * dashboard the builder produces actually uses. Return NULL when the
 * payload is some other shape. */
const char *hds_payload_command(const cJSON *payload);
const char *hds_payload_action(const cJSON *payload);
cJSON      *hds_payload_params(const cJSON *payload);

/* Install the command sink. Safe to call before or after hds_init();
 * with no sink installed, actuations are logged and dropped. */
void hds_set_emit(hds_emit_fn fn, void *user);

/* Load and parse a spec, and point the asset loader at `asset_dir`.
 * Returns ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_ARG on a bad spec. */
esp_err_t hds_init(const char *spec_path, const char *asset_dir);

/* Paint the spec's home page into `parent` (typically lv_scr_act(), but
 * any container works -- a tab's content area, for instance). The
 * renderer owns a root container inside `parent` and rebuilds it on
 * navigation. Returns false when no spec is loaded. */
bool hds_build(lv_obj_t *parent);

/* Switch pages by id. Rebuilds the root container in place. */
bool hds_goto(const char *page_id);

/* The page currently on screen, or NULL. */
const hds_page_t *hds_current_page(void);

/* The loaded spec, for callers that want to introspect it. */
hds_spec_t *hds_spec(void);

/* Drive a widget from an inbound cloud update -- the read-back half of
 * the loop, keyed by the widget id the dashboard already uses. Accepts
 * a bool (switch), number (slider) or string (label). No-op when the id
 * is not on any page. */
void hds_apply_value(const char *widget_id, const cJSON *value);

/* Outline every interactive hit area. Navigation widgets in a real
 * dashboard are often transparent regions sitting behind artwork, which
 * makes them invisible while you are laying a page out. */
void hds_set_debug_hitboxes(bool on);
bool hds_get_debug_hitboxes(void);

void hds_deinit(void);
