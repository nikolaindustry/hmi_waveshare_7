#include "hds_inject.h"
#include <stdbool.h>
#include <string.h>

/* Positional "" placeholders handled per params object. */
#define HDS_FORM_MAX_SLOTS 32

/* Replace `key` in `obj` only where it is already present. */
static void overwrite(cJSON *obj, const char *key, const cJSON *value)
{
    if (!cJSON_IsObject(obj) || !cJSON_GetObjectItem(obj, key)) {
        return;
    }
    cJSON *copy = cJSON_Duplicate((cJSON *)value, true);
    if (copy) {
        cJSON_ReplaceItemInObject(obj, key, copy);
    }
}

/* Run `fn` over each of the four objects the builder treats as an
 * injection site. Keeping the walk in one place is what stops a depth
 * from being forgotten when a new widget type is added. */
static void for_each_site(cJSON *payload,
                          void (*fn)(cJSON *site, void *ctx), void *ctx)
{
    if (!cJSON_IsObject(payload)) {
        return;
    }

    /* 1. the payload itself */
    fn(payload, ctx);

    /* 2. payload.parameters */
    cJSON *params = cJSON_GetObjectItem(payload, "parameters");
    if (cJSON_IsObject(params)) {
        fn(params, ctx);
    }

    /* 3. payload.commands[].actions[].params */
    cJSON *commands = cJSON_GetObjectItem(payload, "commands");
    if (cJSON_IsArray(commands)) {
        cJSON *cmd = NULL;
        cJSON_ArrayForEach(cmd, commands) {
            cJSON *actions = cJSON_GetObjectItem(cmd, "actions");
            if (!cJSON_IsArray(actions)) {
                continue;
            }
            cJSON *act = NULL;
            cJSON_ArrayForEach(act, actions) {
                cJSON *p = cJSON_GetObjectItem(act, "params");
                if (cJSON_IsObject(p)) {
                    fn(p, ctx);
                }
            }
        }
    }

    /* 4. payload.actionParameters */
    cJSON *ap = cJSON_GetObjectItem(payload, "actionParameters");
    if (cJSON_IsObject(ap)) {
        fn(ap, ctx);
    }
}

/* ------------------------------------------------------------ inject */

typedef struct {
    const char *const *keys;
    const cJSON       *value;
} key_ctx_t;

static void apply_keys(cJSON *site, void *ctx)
{
    const key_ctx_t *k = ctx;
    for (int i = 0; k->keys[i]; i++) {
        overwrite(site, k->keys[i], k->value);
    }
}

void hds_inject_keys(cJSON *payload, const char *const *keys,
                     const cJSON *value)
{
    if (!payload || !keys || !value) {
        return;
    }
    key_ctx_t ctx = { .keys = keys, .value = value };
    for_each_site(payload, apply_keys, &ctx);
}

void hds_inject(cJSON *payload, const char *key, const cJSON *value)
{
    const char *keys[] = { key, NULL };
    hds_inject_keys(payload, keys, value);
}

/* -------------------------------------------------------------- form */

static void apply_form(cJSON *site, void *ctx)
{
    const cJSON *fields = ctx;

    overwrite(site, "formData", fields);
    overwrite(site, "data",     fields);

    /* Positional fill: the author leaves "" placeholders and they take
     * the field values in order. Done before the merge below so an
     * explicitly-named key still wins.
     *
     * Collected first, replaced second: cJSON_ReplaceItemInObject frees
     * the node it replaces, so replacing during the walk would leave
     * the iterator pointing at freed memory. */
    const char *slots[HDS_FORM_MAX_SLOTS];
    int n_slots = 0;

    cJSON *slot = NULL;
    cJSON_ArrayForEach(slot, site) {
        if (n_slots >= HDS_FORM_MAX_SLOTS) {
            break;
        }
        if (slot->string && cJSON_IsString(slot) && slot->valuestring &&
            slot->valuestring[0] == '\0') {
            slots[n_slots++] = slot->string;
        }
    }

    const cJSON *field = fields->child;
    for (int i = 0; i < n_slots && field; i++, field = field->next) {
        cJSON *copy = cJSON_Duplicate((cJSON *)field, true);
        if (copy) {
            cJSON_ReplaceItemInObject(site, slots[i], copy);
        }
    }

    /* Merge every field in under its own label. Forms are the one place
     * the builder adds keys rather than only overwriting: the labels
     * ("sensorIndex", "targetTemp") are the parameter names the device
     * handler expects. */
    const cJSON *f = NULL;
    cJSON_ArrayForEach(f, fields) {
        if (!f->string) {
            continue;
        }
        cJSON *copy = cJSON_Duplicate((cJSON *)f, true);
        if (!copy) {
            continue;
        }
        if (cJSON_GetObjectItem(site, f->string)) {
            cJSON_ReplaceItemInObject(site, f->string, copy);
        } else {
            cJSON_AddItemToObject(site, f->string, copy);
        }
    }
}

void hds_inject_form(cJSON *payload, const cJSON *fields)
{
    if (!payload || !cJSON_IsObject(fields)) {
        return;
    }
    /* The top-level payload is not a parameter bag -- merging field
     * labels into it would corrupt `commands`. Only the three nested
     * sites take the merge. */
    cJSON *params = cJSON_GetObjectItem(payload, "parameters");
    if (cJSON_IsObject(params)) {
        apply_form(params, (void *)fields);
    }

    cJSON *commands = cJSON_GetObjectItem(payload, "commands");
    if (cJSON_IsArray(commands)) {
        cJSON *cmd = NULL;
        cJSON_ArrayForEach(cmd, commands) {
            cJSON *actions = cJSON_GetObjectItem(cmd, "actions");
            if (!cJSON_IsArray(actions)) {
                continue;
            }
            cJSON *act = NULL;
            cJSON_ArrayForEach(act, actions) {
                cJSON *p = cJSON_GetObjectItem(act, "params");
                if (cJSON_IsObject(p)) {
                    apply_form(p, (void *)fields);
                }
            }
        }
    }

    cJSON *ap = cJSON_GetObjectItem(payload, "actionParameters");
    if (cJSON_IsObject(ap)) {
        apply_form(ap, (void *)fields);
    }

    /* Top level still takes the plain overwrites. */
    overwrite(payload, "formData", fields);
    overwrite(payload, "data",     fields);
}

/* ------------------------------------------------- action -> event(s) */

/* Ported from actionToEventMap in IoTEnhancedWidgetRenderer.tsx. Keep
 * the two in step: an action missing a row here is a widget that looks
 * wired up in the builder and does nothing on the panel. */
static const struct {
    const char *action;
    const char *events[4];
} k_action_events[] = {
    { "press",         { "push", "click", NULL } },
    { "release",       { "release", NULL } },
    { "click",         { "click", NULL } },
    { "toggle",        { "toggle", "change", NULL } },
    { "valueChange",   { "change", "slide", "slideEnd", NULL } },
    { "colorChange",   { "change", NULL } },
    { "positionChange",{ "change", NULL } },
    { "on",            { "on", NULL } },
    { "off",           { "off", NULL } },
    { "submit",        { "submit", "change", NULL } },
    { "clear",         { "clear", NULL } },
    { "complete",      { "complete", NULL } },
    { "start",         { "start", NULL } },
    { "pause",         { "pause", NULL } },
    { "reset",         { "reset", NULL } },
};

bool hds_action_matches(const char *action_id, const char *event_type)
{
    if (!action_id || !event_type) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_action_events) / sizeof(k_action_events[0]); i++) {
        if (strcmp(k_action_events[i].action, action_id) != 0) {
            continue;
        }
        for (int j = 0; k_action_events[i].events[j]; j++) {
            if (strcmp(k_action_events[i].events[j], event_type) == 0) {
                return true;
            }
        }
        return false;
    }
    /* An action with no row falls back to an exact name match, so a new
     * widget type works before its row is added. */
    return strcmp(action_id, event_type) == 0;
}
