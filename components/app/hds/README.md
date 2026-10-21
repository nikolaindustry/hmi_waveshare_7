# hds — JSON-driven dashboards

Render a dashboard authored in the Hyperwisor builder on the panel,
without writing any UI code for it.

```
  Hyperwisor builder                host                      device
  ──────────────────                ────                      ──────
  La Vitis.json   ──►  tools/hds_transcode.py  ──►  dashboard.json   ──►  hds_*
     2.15 MB                                            6.3 KB            LVGL
                                               +  assets/*.bin
                                                     35 KB
```

## Why a transcoder

The builder's export is a web document: absolute positions on a desktop
canvas, one base64 TTF **per styled widget**, image URLs, and a
JavaScript `script` block. For a 26-widget dashboard that is 2.15 MB, of
which ~19 KB is actual structure and ~1 MB is the same font repeated 14
times.

The host does the expensive, one-off work:

| | |
|---|---|
| Layout | mapped to 800×480 (`--layout`) |
| Fonts | dropped; sizes snapped to the built-in Montserrat faces |
| Colours | CSS variables dropped so the device uses its own theme |
| Images | fetched, downscaled to the widget box, written as raw RGB565 |
| `script` | **not emitted** — reported, then left behind |

What reaches the device is ~6 KB of JSON it parses in milliseconds with
the cJSON it already links.

## Usage

One command does everything:

```bash
./dashboard.sh "La Vitis.json"          # convert, build, flash
./dashboard.sh "La Vitis.json" --sim    # convert, preview on this Mac
./dashboard.sh "La Vitis.json" --build  # convert and build, don't flash
```

In the simulator: `H` outlines hit areas, `R` reloads the spec from
disk, `1`–`9` jump to a page. For scripted checks:

```bash
./sim/build/hmi_sim --dashboard --tap 360,235   # fire a widget, print what goes out
./sim/build/hmi_sim --dashboard --shot p1.bmp   # headless 800x480 capture
```

`tools/testdata/injection-test.json` is a small synthetic export that
exercises every filling rule below: a slider with a `value`
placeholder, one with `level` and `intensity` at two different depths
whose event is authored as `slideEnd`, and a form with a positional
`""`. Real dashboards tend to be mostly switches, which carry no
placeholders and so prove none of this:

```bash
./dashboard.sh tools/testdata/injection-test.json --sim
```

## Filling in the blanks

This is the part that is easy to get wrong, and silently.

A dashboard author writes the message their device should receive, and
leaves a placeholder for whatever the widget is worth when it is used:

```json
{ "commands": [ { "command": "FAN",
                  "actions": [ { "action": "SET",
                                 "params": { "channel": 3, "value": 0 } } ] } ] }
```

`"value": 0` is the blank. Drag to 75 and the web dashboard sends 75.
Two rules govern it, both inverted from the obvious guess:

1. **Overwrite, never add.** Only keys the author already wrote get
   filled. A key they did not write is a key the device is not
   expecting. (Forms are the one documented exception.)
2. **Four depths.** The payload itself, `parameters`,
   `commands[].actions[].params`, and `actionParameters`. Miss one and
   a dashboard authored in that shape ships the placeholder instead of
   the reading.

Which keys get filled depends on the widget:

| widget | keys |
|---|---|
| slider | `value` `speed` `level` `intensity` |
| form | `formData` `data`, **plus** every field by label, **plus** positional `""` fill |
| switch | none — the state is which of its two events fired |

This is why the payload is kept whole in the spec rather than flattened
to one command: flattening would throw away three of the four depths.

## Matching events

The builder does not compare event names directly. A slider release is
the action `valueChange`, which fires events authored as `change`,
`slide` **or** `slideEnd`. Comparing names directly means a dashboard
authored with `slideEnd` works in the browser and does nothing on the
panel.

`hds_action_matches()` ports that table. Both it and the key sets above
mirror `IoTEnhancedWidgetRenderer.tsx` in `Hyperwisor-v4` — if a widget
type is added there, these need the matching row.

## Unsupported widgets

A widget type with no renderer is left out of the spec, so that area of
the panel is blank and everything else still works. Every omission is
listed at the end of a transcoder run, so it is a decision rather than
a surprise. `SUPPORTED` in the transcoder and `s_vtables[]` in
`hds_widget.c` are the two places to keep in step.

## On the device

`assets/dashboard/` is flashed to the `storage` SPIFFS partition by the
top-level `CMakeLists.txt`, so `idf.py flash` writes the spec along with
the app. At boot `app.c` probes the partition:

- spec present and it parses → the dashboard is the UI
- no partition, no spec, or a spec that fails to parse → the hand-built
  `ui_shell` comes up exactly as before

Re-running the transcoder and reflashing is the whole update loop.

> **The dashboard replaces the shell.** With a spec flashed there is no
> on-screen route to the maintenance tab (service PIN, theme, RS-485
> scanner). Erase the `storage` partition to get it back:
> `esptool.py erase_region 0x720000 0x200000`.

## The sink

`components/app/cloud/hds_sink.c` routes actuations **locally first**: a
panel bolted to the machine it controls must not round-trip a relay
through the cloud, and must keep working with no WiFi. A command this
board can satisfy goes straight to `modbus_task_request()`; everything
else goes out over the websocket.

It also bridges three places where the builder and the firmware's own
cloud handlers disagree:

| | builder says | handlers expect |
|---|---|---|
| command | `RELAY_CONTROL` | `relay_control` (dispatched with `strcmp`) |
| coil id | `{"channel":0}` | `{"coil":0}` / `{"relay":0}` |
| state | in the action — `TURN_ON` | in params — `{"state":true}` |

HVAC-owned coils are refused: the climate controller would reassert
within a tick and the relay would chatter.

The read-back half is `hds_apply_value(widget_id, value)`, keyed by the
same widget ids the dashboard already uses.

## Wiring it up elsewhere

```c
hds_set_emit(hds_sink_emit, NULL);
hds_init("/spiffs/dashboard.json", "/spiffs/assets");
hds_build(lv_scr_act());
```

## Adding a widget type

A create function plus one row in the table at the bottom of
`hds_widget.c`. Nothing in `hds_render.c` changes.

```c
static lv_obj_t *create_gauge(lv_obj_t *parent, hds_widget_t *w) { ... }

static const hds_vtable_t s_vtables[] = {
    ...
    { HDS_W_GAUGE, create_gauge, gauge_set_value },
};
```

Add the type to `k_types[]` in `hds_spec.c` and to `SUPPORTED` in the
transcoder so it stops being reported as unsupported.

## Known limits

- **No JavaScript.** The export's `script` block does not run. In the
  dashboards seen so far it holds only the builder's commented-out
  examples, but anything real there needs a firmware command handler.
- **Layout is approximate.** `--layout desktop` scales a web canvas to
  fit, so a wide dashboard shrinks (La Vitis Main lands at 0.61). The
  real fix is an `hmiLayout` breakpoint in the builder, authored at
  800x480, beside the `mobileLayout` it already stores — then
  `--layout hmi` places 1:1.
- **6 of 50 widget types.** See the table above for what the
  transcoder accepts today.
- **Case matters.** The builder emits `RELAY_CONTROL`; `hyperwisor_cmd.c`
  dispatches with `strcmp` against the lowercase `relay_control`
  registered in `hyperwisor_app.c`. `hds_sink.c` folds the case itself;
  switching that dispatcher to `strcasecmp` would remove the whole
  class of bug.
