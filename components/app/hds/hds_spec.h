#pragma once

/* =====================================================================
 *  hds_spec -- in-memory model of a slim dashboard spec.
 *
 *  The spec is produced by tools/hds_transcode.py from a Hyperwisor
 *  dashboard export. Everything expensive has already happened on the
 *  host: layout is mapped to 800x480, font sizes are snapped to the
 *  faces LVGL has built in, colours are plain 0xRRGGBB, and images are
 *  raw RGB565 blobs beside the spec. What lands here is ~6 KB of JSON
 *  for a 26-widget dashboard.
 *
 *  Shape (keys are short because they are on the wire):
 *
 *    { "v":1, "name":"La Vitis", "target":"<device uuid>",
 *      "home":"page-1", "bg":"#ffffff",
 *      "pages":[
 *        { "id":"page-1", "name":"Main", "bg":"#ffffff",
 *          "w":[
 *            { "i":"iot-...", "t":"switch", "n":"NFT Pump",
 *              "r":[183,216,204,38], "box":false, "z":-1,
 *              "s":{ "fg":"#046734", "fs":24, ... },
 *              "g":"page-2",
 *              "e":[ { "on":"on", "cmd":"RELAY_CONTROL",
 *                      "act":"TURN_ON", "p":{"channel":0} } ] } ] } ] }
 *
 *  Ownership: the parsed cJSON document is kept alive for the lifetime
 *  of the spec and every `const char *` below borrows from it. Free the
 *  whole thing with hds_spec_free() and not a field at a time.
 * ===================================================================== */

#include "lvgl.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#define HDS_SPEC_VERSION   2
#define HDS_ID_MAX         48

typedef enum {
    HDS_W_UNKNOWN = 0,
    HDS_W_LABEL,
    HDS_W_SWITCH,
    HDS_W_SLIDER,
    HDS_W_IMAGE,
    HDS_W_NAV,
    HDS_W_FORM,
    HDS_W_TYPE_COUNT
} hds_wtype_t;

/* One authored event. The payload is kept WHOLE rather than flattened
 * to a single command.
 *
 * The dashboard author leaves placeholders in it -- `"value": 0` on a
 * slider, `""` on a form field -- and the live value overwrites them at
 * send time. The builder does that at four different depths (top level,
 * `parameters`, `commands[].actions[].params`, `actionParameters`), so
 * flattening would throw three of them away and the device would ship
 * the designer's placeholder instead of the real reading. */
typedef struct {
    const char *on;         /* event type exactly as authored       */
    const char *target;     /* NULL => the spec-wide default target */
    cJSON      *payload;    /* borrowed from the document           */
} hds_event_t;

typedef struct {
    const char *id;
    const char *label;
    const char *type;       /* "text" | "number" | ...  */
    const char *placeholder;
    const char *def;
} hds_field_t;

typedef struct {
    char        id[HDS_ID_MAX];
    hds_wtype_t type;
    const char *name;

    lv_coord_t  x, y, w, h;
    bool        box;        /* draw the card container behind the widget */
    int         z;

    /* --- style: which members matter depends on `type` --- */
    uint32_t    fg;  bool has_fg;
    uint32_t    bg;  bool has_bg;
    uint16_t    font;       /* px; one of the built-in Montserrat sizes */

    int32_t     vmin, vmax, vdef;
    uint16_t    track;      /* slider track height */
    bool        showval;
    bool        icon;

    const char *asset;      /* image: asset key, i.e. assets/<key>.bin  */
    const char *go;         /* nav:   target page id                    */

    hds_field_t *fields;  int n_fields;
    hds_event_t *events;  int n_events;

    lv_obj_t   *obj;        /* live LVGL object, set by the renderer    */
    lv_obj_t   *aux;        /* secondary object (value label, etc.)     */
} hds_widget_t;

typedef struct {
    char          id[HDS_ID_MAX];
    const char   *name;
    uint32_t      bg;  bool has_bg;
    hds_widget_t *widgets;  int n_widgets;
    lv_obj_t     *scr;      /* lazily built screen */
} hds_page_t;

typedef struct {
    cJSON       *doc;       /* owns every borrowed string in here */
    const char  *name;
    const char  *target;    /* default device target id           */
    const char  *home;      /* page id to show first              */
    uint32_t     bg;  bool has_bg;
    hds_page_t  *pages;  int n_pages;
} hds_spec_t;

/* Parse a slim spec. Returns NULL on malformed input or a version
 * mismatch, logging the reason. */
hds_spec_t *hds_spec_parse(const char *json, size_t len);

/* Read and parse a spec file (sim: a path on disk; device: /spiffs/...). */
hds_spec_t *hds_spec_load_file(const char *path);

void hds_spec_free(hds_spec_t *spec);

/* Lookups. Both are linear -- a dashboard has tens of widgets, not
 * thousands, and this keeps the model allocation-free. */
hds_page_t   *hds_spec_page(hds_spec_t *spec, const char *page_id);
hds_widget_t *hds_spec_widget(hds_spec_t *spec, const char *widget_id);

const char *hds_wtype_name(hds_wtype_t t);
