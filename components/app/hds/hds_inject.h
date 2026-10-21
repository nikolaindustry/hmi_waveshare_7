#pragma once

/* =====================================================================
 *  hds_inject -- fill the live value into an authored payload.
 *
 *  A dashboard author writes the message their device should receive
 *  and leaves placeholders in it for whatever the widget is worth at
 *  the moment it is used:
 *
 *      { "commands": [ { "command": "FAN",
 *                        "actions": [ { "action": "SET",
 *                                       "params": { "channel": 3,
 *                                                   "value": 0 } } ] } ] }
 *
 *  `"value": 0` is the blank. Drag the slider to 75 and the web
 *  dashboard sends 75; it finds every key named `value` that ALREADY
 *  EXISTS and overwrites it.
 *
 *  Two rules matter, and both are easy to get backwards:
 *
 *    1. Overwrite, never add. A key the author did not write is a key
 *       the device is not expecting. (Forms are the documented
 *       exception -- see hds_inject_form.)
 *    2. Four depths. The builder looks at the payload itself, at
 *       `parameters`, at `commands[].actions[].params`, and at
 *       `actionParameters`. Miss one and a dashboard authored in that
 *       shape silently ships the placeholder.
 *
 *  Mirrors IoTEnhancedWidgetRenderer.tsx in the Hyperwisor-v4 builder.
 * ===================================================================== */

#include "cJSON.h"
#include <stdbool.h>

/* Overwrite every pre-existing occurrence of `key`, at all four depths,
 * with a copy of `value`. Does nothing where the key is absent.
 * `value` stays owned by the caller. */
void hds_inject(cJSON *payload, const char *key, const cJSON *value);

/* Same, for a NULL-terminated list of interchangeable key names -- a
 * slider fills `value`, `speed`, `level` or `intensity`, whichever the
 * author happened to use. */
void hds_inject_keys(cJSON *payload, const char *const *keys,
                     const cJSON *value);

/* Form submit, which is the one case that adds keys.
 *
 * `fields` is an object of {label: "typed text"}. Beyond the usual
 * overwrite of `formData` / `data`, the builder also:
 *   - fills empty-string "" placeholders in params positionally, in
 *     field order, and
 *   - merges every field in under its own label.
 */
void hds_inject_form(cJSON *payload, const cJSON *fields);

/* ---- action -> event-type matching ----
 *
 * The builder does not compare the event name directly. A slider
 * release is action "valueChange", which fires events authored as
 * "change", "slide" OR "slideEnd". Comparing names directly means a
 * dashboard authored with "slideEnd" does nothing on the panel while
 * working perfectly in the browser.
 *
 * True when `event_type` is one of the types `action_id` triggers. */
bool hds_action_matches(const char *action_id, const char *event_type);
