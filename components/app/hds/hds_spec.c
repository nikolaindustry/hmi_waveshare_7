#include "hds_spec.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hds_spec";

/* ---------------------------------------------------------------- utils */

static const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool bool_of(const cJSON *o, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static int int_of(const cJSON *o, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : dflt;
}

/* "#rrggbb" -> 0xRRGGBB. Returns false when the key is absent, which is
 * how the transcoder says "this was a CSS variable, use the theme". */
static bool colour_of(const cJSON *o, const char *key, uint32_t *out)
{
    const char *s = str_of(o, key);
    if (!s || s[0] != '#') {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(s + 1, &end, 16);
    if (end != s + 7) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void copy_id(char *dst, const cJSON *o, const char *key)
{
    const char *s = str_of(o, key);
    if (s) {
        snprintf(dst, HDS_ID_MAX, "%s", s);
    } else {
        dst[0] = '\0';
    }
}

/* ---------------------------------------------------------------- types */

static const struct { const char *name; hds_wtype_t type; } k_types[] = {
    { "label",         HDS_W_LABEL  },
    { "switch",        HDS_W_SWITCH },
    { "slider",        HDS_W_SLIDER },
    { "image",         HDS_W_IMAGE  },
    { "navigate-page", HDS_W_NAV    },
    { "form",          HDS_W_FORM   },
};

static hds_wtype_t wtype_of(const char *s)
{
    if (s) {
        for (size_t i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
            if (strcmp(k_types[i].name, s) == 0) {
                return k_types[i].type;
            }
        }
    }
    return HDS_W_UNKNOWN;
}

const char *hds_wtype_name(hds_wtype_t t)
{
    for (size_t i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
        if (k_types[i].type == t) {
            return k_types[i].name;
        }
    }
    return "unknown";
}

/* ------------------------------------------------------------- children */

static void parse_events(hds_widget_t *w, const cJSON *jw)
{
    const cJSON *je = cJSON_GetObjectItem(jw, "e");
    int n = cJSON_IsArray(je) ? cJSON_GetArraySize(je) : 0;
    if (n <= 0) {
        return;
    }

    w->events = calloc(n, sizeof(hds_event_t));
    if (!w->events) {
        return;
    }

    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(je, i);
        hds_event_t *ev = &w->events[w->n_events];
        ev->on      = str_of(e, "on");
        ev->target  = str_of(e, "to");
        ev->payload = cJSON_GetObjectItem(e, "pl");
        if (ev->on && cJSON_IsObject(ev->payload)) {
            w->n_events++;
        }
    }
}

static void parse_fields(hds_widget_t *w, const cJSON *js)
{
    const cJSON *jf = cJSON_GetObjectItem(js, "fields");
    int n = cJSON_IsArray(jf) ? cJSON_GetArraySize(jf) : 0;
    if (n <= 0) {
        return;
    }

    w->fields = calloc(n, sizeof(hds_field_t));
    if (!w->fields) {
        return;
    }

    for (int i = 0; i < n; i++) {
        cJSON *f = cJSON_GetArrayItem(jf, i);
        hds_field_t *fd = &w->fields[w->n_fields++];
        fd->id          = str_of(f, "id");
        fd->label       = str_of(f, "l");
        fd->type        = str_of(f, "ty");
        fd->placeholder = str_of(f, "ph");
        fd->def         = str_of(f, "d");
    }
}

static bool parse_widget(hds_widget_t *w, const cJSON *jw)
{
    w->type = wtype_of(str_of(jw, "t"));
    if (w->type == HDS_W_UNKNOWN) {
        ESP_LOGW(TAG, "widget %s: unknown type '%s', skipped",
                 str_of(jw, "i") ? str_of(jw, "i") : "?",
                 str_of(jw, "t") ? str_of(jw, "t") : "?");
        return false;
    }

    copy_id(w->id, jw, "i");
    w->name = str_of(jw, "n");

    const cJSON *r = cJSON_GetObjectItem(jw, "r");
    if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) != 4) {
        ESP_LOGW(TAG, "widget %s: bad rect, skipped", w->id);
        return false;
    }
    w->x = (lv_coord_t)cJSON_GetArrayItem(r, 0)->valuedouble;
    w->y = (lv_coord_t)cJSON_GetArrayItem(r, 1)->valuedouble;
    w->w = (lv_coord_t)cJSON_GetArrayItem(r, 2)->valuedouble;
    w->h = (lv_coord_t)cJSON_GetArrayItem(r, 3)->valuedouble;

    /* The builder only draws a card behind the widgets that need one.
     * Labels, images and navigation hit areas sit directly on the page,
     * so the default depends on the type; an explicit "box" in the spec
     * always wins. */
    bool box_dflt = (w->type == HDS_W_SWITCH ||
                     w->type == HDS_W_SLIDER ||
                     w->type == HDS_W_FORM);
    w->box = bool_of(jw, "box", box_dflt);
    w->z   = int_of(jw, "z", 0);
    w->go  = str_of(jw, "g");

    /* Sensible defaults before the style block narrows them. */
    w->font = 14;
    w->vmin = 0;
    w->vmax = 100;

    const cJSON *s = cJSON_GetObjectItem(jw, "s");
    if (cJSON_IsObject(s)) {
        w->has_fg = colour_of(s, "fg", &w->fg);
        w->has_bg = colour_of(s, "bg", &w->bg);
        w->font   = (uint16_t)int_of(s, "fs", w->font);
        w->vmin   = int_of(s, "min", w->vmin);
        w->vmax   = int_of(s, "max", w->vmax);
        w->vdef   = int_of(s, "val", 0);
        w->track  = (uint16_t)int_of(s, "th", 0);
        w->showval = bool_of(s, "showval", false);
        w->icon    = bool_of(s, "icon", false);
        w->asset   = str_of(s, "src");
        parse_fields(w, s);
    }

    /* A slider whose range is inverted or empty would make lv_slider
     * misbehave; clamp it here rather than in every call site. */
    if (w->vmax <= w->vmin) {
        w->vmax = w->vmin + 1;
    }

    parse_events(w, jw);
    return true;
}

static bool parse_page(hds_page_t *p, const cJSON *jp)
{
    copy_id(p->id, jp, "id");
    if (!p->id[0]) {
        ESP_LOGW(TAG, "page without an id, skipped");
        return false;
    }
    p->name   = str_of(jp, "name");
    p->has_bg = colour_of(jp, "bg", &p->bg);

    const cJSON *jws = cJSON_GetObjectItem(jp, "w");
    int n = cJSON_IsArray(jws) ? cJSON_GetArraySize(jws) : 0;
    if (n <= 0) {
        return true;                    /* an empty page is still a page */
    }

    p->widgets = calloc(n, sizeof(hds_widget_t));
    if (!p->widgets) {
        ESP_LOGE(TAG, "out of memory for %d widgets", n);
        return false;
    }

    for (int i = 0; i < n; i++) {
        if (parse_widget(&p->widgets[p->n_widgets], cJSON_GetArrayItem(jws, i))) {
            p->n_widgets++;
        } else {
            memset(&p->widgets[p->n_widgets], 0, sizeof(hds_widget_t));
        }
    }
    return true;
}

/* ---------------------------------------------------------------- public */

hds_spec_t *hds_spec_parse(const char *json, size_t len)
{
    if (!json || !len) {
        ESP_LOGE(TAG, "empty spec");
        return NULL;
    }

    cJSON *doc = cJSON_ParseWithLength(json, len);
    if (!doc) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse failed near: %.32s", err ? err : "?");
        return NULL;
    }

    int v = int_of(doc, "v", 0);
    if (v != HDS_SPEC_VERSION) {
        ESP_LOGE(TAG, "spec version %d, expected %d -- re-run "
                      "tools/hds_transcode.py", v, HDS_SPEC_VERSION);
        cJSON_Delete(doc);
        return NULL;
    }

    hds_spec_t *spec = calloc(1, sizeof(hds_spec_t));
    if (!spec) {
        cJSON_Delete(doc);
        return NULL;
    }
    spec->doc    = doc;
    spec->name   = str_of(doc, "name");
    spec->target = str_of(doc, "target");
    spec->home   = str_of(doc, "home");
    spec->has_bg = colour_of(doc, "bg", &spec->bg);

    const cJSON *jps = cJSON_GetObjectItem(doc, "pages");
    int n = cJSON_IsArray(jps) ? cJSON_GetArraySize(jps) : 0;
    if (n <= 0) {
        ESP_LOGE(TAG, "spec has no pages");
        hds_spec_free(spec);
        return NULL;
    }

    spec->pages = calloc(n, sizeof(hds_page_t));
    if (!spec->pages) {
        hds_spec_free(spec);
        return NULL;
    }

    for (int i = 0; i < n; i++) {
        if (parse_page(&spec->pages[spec->n_pages], cJSON_GetArrayItem(jps, i))) {
            spec->n_pages++;
        }
    }

    if (!spec->n_pages) {
        ESP_LOGE(TAG, "no usable pages");
        hds_spec_free(spec);
        return NULL;
    }

    int total = 0;
    for (int i = 0; i < spec->n_pages; i++) {
        total += spec->pages[i].n_widgets;
    }
    ESP_LOGI(TAG, "'%s': %d page(s), %d widget(s), target=%s",
             spec->name ? spec->name : "?", spec->n_pages, total,
             spec->target ? spec->target : "(none)");

    return spec;
}

hds_spec_t *hds_spec_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* A transcoded spec is single-digit KB. Anything far past that is a
     * raw dashboard export that was never run through the transcoder --
     * say so instead of trying to parse a megabyte of base64 fonts. */
    if (len <= 0 || len > 512 * 1024) {
        ESP_LOGE(TAG, "%s is %ld bytes -- did you run "
                      "tools/hds_transcode.py on it?", path, len);
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';

    hds_spec_t *spec = hds_spec_parse(buf, got);
    free(buf);
    return spec;
}

void hds_spec_free(hds_spec_t *spec)
{
    if (!spec) {
        return;
    }
    for (int i = 0; i < spec->n_pages; i++) {
        hds_page_t *p = &spec->pages[i];
        for (int j = 0; j < p->n_widgets; j++) {
            free(p->widgets[j].events);
            free(p->widgets[j].fields);
        }
        free(p->widgets);
    }
    free(spec->pages);
    cJSON_Delete(spec->doc);
    free(spec);
}

hds_page_t *hds_spec_page(hds_spec_t *spec, const char *page_id)
{
    if (!spec || !page_id) {
        return NULL;
    }
    for (int i = 0; i < spec->n_pages; i++) {
        if (strcmp(spec->pages[i].id, page_id) == 0) {
            return &spec->pages[i];
        }
    }
    return NULL;
}

hds_widget_t *hds_spec_widget(hds_spec_t *spec, const char *widget_id)
{
    if (!spec || !widget_id) {
        return NULL;
    }
    for (int i = 0; i < spec->n_pages; i++) {
        hds_page_t *p = &spec->pages[i];
        for (int j = 0; j < p->n_widgets; j++) {
            if (strcmp(p->widgets[j].id, widget_id) == 0) {
                return &p->widgets[j];
            }
        }
    }
    return NULL;
}
