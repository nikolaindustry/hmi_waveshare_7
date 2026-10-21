#include "hds.h"
#include "hds_widget.h"
#include "hds_inject.h"
#include "hds_assets.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hds";

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

static struct {
    hds_spec_t  *spec;
    hds_page_t  *page;      /* page currently on screen */
    lv_obj_t    *parent;    /* what hds_build() was handed */
    lv_obj_t    *root;      /* container we own inside `parent` */
    hds_emit_fn  emit;
    void        *emit_user;
    bool         debug_hitboxes;
} s;

/* =======================================================================
 *  Command egress
 * ======================================================================= */

void hds_set_emit(hds_emit_fn fn, void *user)
{
    s.emit = fn;
    s.emit_user = user;
}

/* Slider-ish widgets fill whichever of these names the author used. */
static const char *const k_value_keys[] = {
    "value", "speed", "level", "intensity", NULL
};

void hds_widget_fire(hds_widget_t *w, const char *action_id, cJSON *value)
{
    int fired = 0;

    for (int i = 0; i < w->n_events; i++) {
        hds_event_t *ev = &w->events[i];
        if (!hds_action_matches(action_id, ev->on)) {
            continue;
        }

        /* Work on a copy: the authored payload is the template and has
         * to survive to be filled in again on the next press. */
        cJSON *payload = cJSON_Duplicate(ev->payload, true);
        if (!payload) {
            continue;
        }

        if (value) {
            switch (w->type) {
            case HDS_W_SLIDER:
                hds_inject_keys(payload, k_value_keys, value);
                break;
            case HDS_W_FORM:
                hds_inject_form(payload, value);
                break;
            default:
                /* Overwrite a plain "value" placeholder if the author
                 * left one; anything more specific needs its own case. */
                hds_inject(payload, "value", value);
                break;
            }
        }

        const char *target = ev->target ? ev->target
                           : (s.spec ? s.spec->target : NULL);

        if (s.emit) {
            s.emit(target, payload, s.emit_user);
        } else {
            char *txt = cJSON_PrintUnformatted(payload);
            ESP_LOGW(TAG, "no sink: %s (drop)", txt ? txt : "{}");
            free(txt);
        }

        cJSON_Delete(payload);
        fired++;
    }

    cJSON_Delete(value);

    if (!fired && w->n_events) {
        ESP_LOGD(TAG, "widget %s: no event matches action '%s'",
                 w->id, action_id);
    }
}

/* ---- payload accessors, for sinks that route locally ---- */

static cJSON *first_action(const cJSON *payload)
{
    cJSON *commands = cJSON_GetObjectItem(payload, "commands");
    cJSON *cmd = cJSON_IsArray(commands) ? cJSON_GetArrayItem(commands, 0) : NULL;
    cJSON *actions = cmd ? cJSON_GetObjectItem(cmd, "actions") : NULL;
    return cJSON_IsArray(actions) ? cJSON_GetArrayItem(actions, 0) : NULL;
}

const char *hds_payload_command(const cJSON *payload)
{
    cJSON *commands = cJSON_GetObjectItem(payload, "commands");
    cJSON *cmd = cJSON_IsArray(commands) ? cJSON_GetArrayItem(commands, 0) : NULL;
    cJSON *name = cmd ? cJSON_GetObjectItem(cmd, "command") : NULL;
    return cJSON_IsString(name) ? name->valuestring : NULL;
}

const char *hds_payload_action(const cJSON *payload)
{
    cJSON *act = first_action(payload);
    cJSON *name = act ? cJSON_GetObjectItem(act, "action") : NULL;
    return cJSON_IsString(name) ? name->valuestring : NULL;
}

cJSON *hds_payload_params(const cJSON *payload)
{
    cJSON *act = first_action(payload);
    return act ? cJSON_GetObjectItem(act, "params") : NULL;
}

/* =======================================================================
 *  Page build
 * ======================================================================= */

static void build_page(hds_page_t *p)
{
    /* The keyboard a form created lives on the old root; it goes with
     * it, so drop the stale handle before the delete. */
    hds_widget_forget_keyboard();

    if (s.root) {
        lv_obj_del(s.root);
        s.root = NULL;
    }

    s.root = lv_obj_create(s.parent);
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s.root, 0, 0);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);

    bool     has_bg = p->has_bg || s.spec->has_bg;
    uint32_t bg = p->has_bg ? p->bg
                : (s.spec->has_bg ? s.spec->bg : UI_COLOR_BG);

    /* Cards, text and strokes are derived from the page background, so
     * this has to happen before any widget is created. */
    hds_palette_set(bg, has_bg);

    lv_obj_set_style_bg_color(s.root, hex(bg), 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);

    int built = 0, skipped = 0;

    /* The transcoder already ordered widgets by z-index, and LVGL paints
     * in creation order, so a plain walk gets the stacking right. */
    for (int i = 0; i < p->n_widgets; i++) {
        hds_widget_t *w = &p->widgets[i];
        const hds_vtable_t *vt = hds_vtable_for(w->type);
        if (!vt || !vt->create) {
            ESP_LOGW(TAG, "no renderer for %s (widget %s)",
                     hds_wtype_name(w->type), w->id);
            skipped++;
            continue;
        }
        w->obj = vt->create(s.root, w);
        if (w->obj) {
            built++;
        } else {
            skipped++;
        }
    }

    s.page = p;
    ESP_LOGI(TAG, "page '%s': %d widget(s) built%s",
             p->name ? p->name : p->id, built,
             skipped ? ", some skipped" : "");
    if (skipped) {
        ESP_LOGW(TAG, "page '%s': %d widget(s) skipped",
                 p->name ? p->name : p->id, skipped);
    }
}

/* =======================================================================
 *  Public API
 * ======================================================================= */

esp_err_t hds_init(const char *spec_path, const char *asset_dir)
{
    hds_deinit();

    hds_assets_init(asset_dir);

    s.spec = hds_spec_load_file(spec_path);
    if (!s.spec) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool hds_build(lv_obj_t *parent)
{
    if (!s.spec || !parent) {
        ESP_LOGE(TAG, "hds_build: no spec loaded");
        return false;
    }

    s.parent = parent;

    hds_page_t *home = s.spec->home ? hds_spec_page(s.spec, s.spec->home)
                                    : NULL;
    if (!home) {
        /* A spec with a stale "home" is still usable -- start at the
         * first page rather than showing nothing. */
        if (s.spec->home) {
            ESP_LOGW(TAG, "home page '%s' not found; using '%s'",
                     s.spec->home, s.spec->pages[0].id);
        }
        home = &s.spec->pages[0];
    }

    build_page(home);
    return true;
}

bool hds_goto(const char *page_id)
{
    if (!s.spec || !s.parent) {
        return false;
    }
    hds_page_t *p = hds_spec_page(s.spec, page_id);
    if (!p) {
        ESP_LOGW(TAG, "goto: no page '%s'", page_id ? page_id : "(null)");
        return false;
    }
    if (p == s.page) {
        return true;
    }
    build_page(p);
    return true;
}

const hds_page_t *hds_current_page(void) { return s.page; }
hds_spec_t        *hds_spec(void)        { return s.spec; }

void hds_apply_value(const char *widget_id, const cJSON *value)
{
    if (!s.spec) {
        return;
    }
    hds_widget_t *w = hds_spec_widget(s.spec, widget_id);
    if (!w) {
        ESP_LOGD(TAG, "value for unknown widget %s",
                 widget_id ? widget_id : "(null)");
        return;
    }
    /* A widget on a page that is not currently built has no object to
     * drive; the value will be picked up when the page is next shown. */
    if (!w->obj) {
        return;
    }
    const hds_vtable_t *vt = hds_vtable_for(w->type);
    if (vt && vt->set_value) {
        vt->set_value(w, value);
    }
}

void hds_set_debug_hitboxes(bool on)
{
    if (s.debug_hitboxes == on) {
        return;
    }
    s.debug_hitboxes = on;
    if (s.page) {
        build_page(s.page);       /* outlines are drawn at create time */
    }
}

bool hds_get_debug_hitboxes(void) { return s.debug_hitboxes; }

void hds_deinit(void)
{
    if (s.root) {
        hds_widget_forget_keyboard();
        lv_obj_del(s.root);
    }
    hds_spec_free(s.spec);
    hds_assets_reset();

    hds_emit_fn keep_fn = s.emit;
    void       *keep_user = s.emit_user;
    bool        keep_dbg = s.debug_hitboxes;
    memset(&s, 0, sizeof(s));
    s.emit = keep_fn;
    s.emit_user = keep_user;
    s.debug_hitboxes = keep_dbg;
}
