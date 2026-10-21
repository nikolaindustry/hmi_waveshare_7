#!/usr/bin/env python3
"""
hds_transcode.py -- Hyperwisor dashboard JSON  ->  slim on-device spec.

The dashboard builder exports a web-oriented document: absolute pixel
positions on a desktop canvas, one base64 TTF copy per styled widget,
image URLs, and a JavaScript `script` block. A 2.2 MB export of a
26-widget dashboard is ~19 KB of actual structure and ~1 MB of
duplicated font.

This tool does the heavy lifting on the host so the firmware only ever
sees something it can parse in a few milliseconds:

  * layout mapped to the panel's 800x480 (see --layout)
  * font sizes snapped to the sizes LVGL actually has built in
  * CSS variables (hsl(var(--foo))) dropped so the device falls back to
    its own theme palette
  * images fetched / base64-decoded, downscaled to the widget box and
    written out as raw RGB565 blobs the device mmaps straight into an
    lv_img_dsc_t
  * fonts de-duplicated into a manifest (the device maps them onto its
    built-in faces; see docs in hds_spec.h)
  * the `script` block reported but not emitted -- there is no JS engine
    on the device, by design

Usage:
    tools/hds_transcode.py "La Vitis.json" -o main/dashboard
    tools/hds_transcode.py in.json -o out/ --layout hmi --verbose

Output:
    <out>/dashboard.json      slim spec
    <out>/assets/<key>.bin    raw RGB565(+A8) image blobs
"""

import argparse
import base64
import hashlib
import json
import os
import re
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hds_png

SPEC_VERSION = 2

PANEL_W = 800
PANEL_H = 480

# Faces compiled into both the firmware (sdkconfig CONFIG_LV_FONT_MONTSERRAT_*)
# and the simulator (sim/lv_conf.h). Keep the two in sync if you change this.
FONT_SIZES = (12, 14, 16, 20, 24, 40)

# Widget types the firmware renderer has a vtable for. Keep this exactly
# in step with s_vtables[] in components/app/hds/hds_widget.c.
#
# Anything else is left out of the spec, so that area of the panel is
# simply blank -- the rest of the dashboard still works. Every omission
# is listed at the end of a run so it is a decision, not a surprise.
SUPPORTED = {
    "label", "switch", "slider", "image", "navigate-page", "form",
}

CSS_VAR = re.compile(r"(hsl|rgb|var)\s*\(")


# ----------------------------------------------------------------- colour

def colour(v):
    """'#046734' -> '#046734'; CSS variables / junk -> None (use theme)."""
    if not isinstance(v, str):
        return None
    v = v.strip()
    if CSS_VAR.search(v):
        return None
    if re.fullmatch(r"#[0-9a-fA-F]{6}", v):
        return v.lower()
    if re.fullmatch(r"#[0-9a-fA-F]{3}", v):
        return "#" + "".join(c * 2 for c in v[1:]).lower()
    return None


def snap_font(px):
    """Nearest built-in face to a scaled web font size."""
    return min(FONT_SIZES, key=lambda f: abs(f - px))


# ----------------------------------------------------------------- assets

class AssetStore:
    """Content-addressed store of raw RGB565 blobs."""

    def __init__(self, outdir, verbose=False):
        self.dir = os.path.join(outdir, "assets")
        os.makedirs(self.dir, exist_ok=True)
        self.verbose = verbose
        self.by_url = {}      # (url, w, h) -> key
        self.written = {}     # key -> bytes written

    def _fetch(self, url):
        if url.startswith("data:"):
            head, _, b64 = url.partition(",")
            if ";base64" not in head:
                raise ValueError("only base64 data: URIs are supported")
            return base64.b64decode(b64)
        if url.startswith(("http://", "https://")):
            req = urllib.request.Request(url, headers={"User-Agent": "hds-transcode"})
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        with open(url, "rb") as f:
            return f.read()

    def add(self, url, box_w, box_h):
        """Fetch, fit into box_w x box_h, store. Returns an asset key."""
        ck = (url, box_w, box_h)
        if ck in self.by_url:
            return self.by_url[ck]

        try:
            raw = self._fetch(url)
        except Exception as e:                       # noqa: BLE001
            warn("image fetch failed (%s): %s" % (short(url), e))
            return None

        try:
            w, h, rgba = hds_png.decode(raw)
        except Exception as e:                       # noqa: BLE001
            warn("image decode failed (%s): %s" % (short(url), e))
            return None

        # Fit inside the widget box, preserving aspect. Never upscale --
        # a blurry 3x logo costs PSRAM for nothing.
        scale = min(box_w / w, box_h / h, 1.0)
        tw, th = max(1, round(w * scale)), max(1, round(h * scale))
        rgba = hds_png.resize(w, h, rgba, tw, th)

        has_alpha = any(rgba[i] != 255 for i in range(3, len(rgba), 4))
        blob = pack_rgb565(tw, th, rgba, has_alpha)

        key = hashlib.sha1(blob).hexdigest()[:12]
        path = os.path.join(self.dir, key + ".bin")
        if key not in self.written:
            with open(path, "wb") as f:
                f.write(blob)
            self.written[key] = len(blob)
            if self.verbose:
                info("asset %s  %dx%d -> %dx%d  %s  %.1f KB"
                     % (key, w, h, tw, th,
                        "RGB565+A8" if has_alpha else "RGB565",
                        len(blob) / 1024))

        self.by_url[ck] = key
        return key


def pack_rgb565(w, h, rgba, has_alpha):
    """HDSI blob: 12-byte header + native-endian RGB565 (+ A8 per pixel).

    Matches LV_COLOR_DEPTH 16 with LV_COLOR_16_SWAP 0, i.e. what both
    sim/lv_conf.h and sdkconfig are set to.
    """
    cf = 5 if has_alpha else 4          # LV_IMG_CF_TRUE_COLOR{,_ALPHA}
    out = bytearray(b"HDSI")
    out += w.to_bytes(2, "little") + h.to_bytes(2, "little")
    out += bytes((cf, 0, 0, 0))
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
        px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += px.to_bytes(2, "little")
        if has_alpha:
            out.append(a)
    return bytes(out)


# ----------------------------------------------------------------- layout

def page_extent(widgets, key):
    """Bounding box of a page in one of the export's layout spaces."""
    x2 = y2 = 0
    for w in widgets:
        pos, size = widget_box(w, key)
        if pos is None:
            continue
        x2 = max(x2, pos[0] + size[0])
        y2 = max(y2, pos[1] + size[1])
    return x2, y2


def widget_box(w, key):
    """(x, y), (w, h) in the requested layout space, or (None, None)."""
    if key == "desktop":
        pos, size = w.get("position"), w.get("size")
    else:
        layout = w.get(key) or {}
        pos, size = layout.get("position"), layout.get("size")
    if not pos or not size:
        return None, None
    return ((pos.get("x", 0), pos.get("y", 0)),
            (size.get("width", 0), size.get("height", 0)))


def resolve_layout(page, mode):
    """Pick a layout space and a scale that lands the page on the panel.

    'hmi'     -- use the hmiLayout block authored at 800x480 (1:1, no
                 scaling). This is the right answer long term: add the
                 breakpoint to the dashboard builder next to the
                 mobileLayout it already emits.
    'desktop' -- scale the desktop canvas uniformly to fit and centre it.
                 Preserves the designer's composition exactly, at the
                 cost of shrinking everything when the canvas is wide.

    Returns (layout_key, scale, dx, dy).
    """
    widgets = page.get("widgets", [])

    if mode == "hmi":
        if any(w.get("hmiLayout") for w in widgets):
            return "hmiLayout", 1.0, 0, 0
        warn("page %r has no hmiLayout; falling back to the desktop canvas"
             % page.get("name"))

    ew, eh = page_extent(widgets, "desktop")
    if ew <= 0 or eh <= 0:
        return "desktop", 1.0, 0, 0

    scale = min(PANEL_W / ew, PANEL_H / eh, 1.0)
    dx = round((PANEL_W - ew * scale) / 2)
    dy = round((PANEL_H - eh * scale) / 2)
    return "desktop", scale, dx, dy


# ----------------------------------------------------------------- events

def strip_private(obj):
    """Drop the builder's "_rawJson" bookkeeping mirrors, recursively.

    Every params object is duplicated back as a pretty-printed string
    under "_rawJson". On a 7 KB spec that duplication is most of the
    file, and the device never reads it.
    """
    if isinstance(obj, dict):
        return {k: strip_private(v) for k, v in obj.items()
                if not k.startswith("_")}
    if isinstance(obj, list):
        return [strip_private(v) for v in obj]
    return obj


def extract_events(cfg):
    """widgetEvents[] -> [{on, to, pl}], payload kept whole.

    The payload is NOT flattened. At runtime the web renderer fills in
    live values by overwriting keys that already exist in it, and it
    does so at four different depths (top level, `parameters`,
    `commands[].actions[].params`, `actionParameters`). Flattening to a
    single command would throw away three of them, so the device gets
    the payload exactly as authored and injects into it the same way.
    """
    out = []
    for ev in cfg.get("widgetEvents") or []:
        when = ev.get("eventType")
        if not when:
            continue
        for tgt in ev.get("targets") or []:
            payload = tgt.get("payload")
            if not isinstance(payload, dict):
                continue
            entry = {"on": when, "pl": strip_private(payload)}
            if tgt.get("targetId"):
                entry["to"] = tgt["targetId"]
            out.append(entry)
    return out


# ----------------------------------------------------------------- widgets

def convert_widget(w, ctx):
    """One export widget -> one slim spec widget, or None to skip it."""
    wtype = w.get("type")
    cfg = w.get("config") or {}
    key, scale, dx, dy = ctx["layout"]

    pos, size = widget_box(w, key)
    if pos is None:
        warn("widget %s (%s) has no %s box; skipped"
             % (w.get("id"), wtype, key))
        return None

    rect = [
        round(pos[0] * scale) + dx,
        round(pos[1] * scale) + dy,
        max(1, round(size[0] * scale)),
        max(1, round(size[1] * scale)),
    ]

    out = {
        "i": w.get("id"),
        "t": wtype,
        "n": w.get("title") or "",
        "r": rect,
    }

    style = w.get("style") or {}
    if style.get("showContainer") is False or cfg.get("showContainer") is False:
        out["box"] = False
    if isinstance(style.get("zIndex"), int) and style["zIndex"]:
        out["z"] = style["zIndex"]

    s = {}

    if wtype == "label":
        if c := colour(cfg.get("textColor")):
            s["fg"] = c
        if (fs := cfg.get("fontSize")):
            s["fs"] = snap_font(fs * scale)
            ctx["font_scaled"].append((out["n"], fs, s["fs"]))

    elif wtype == "switch":
        if c := colour(cfg.get("toggleActiveColor") or cfg.get("switchColor")):
            s["fg"] = c

    elif wtype == "slider":
        if c := colour(cfg.get("sliderColor")):
            s["fg"] = c
        s["min"] = int(cfg.get("sliderMin", 0))
        s["max"] = int(cfg.get("sliderMax", 100))
        s["val"] = int(cfg.get("defaultValue", cfg.get("sliderValue", 0)))
        if (th := cfg.get("sliderTrackHeight")):
            s["th"] = max(2, round(th * scale))
        if cfg.get("showSliderValue"):
            s["showval"] = True

    elif wtype == "image":
        url = cfg.get("imageUrl")
        if not url:
            warn("image widget %s has no imageUrl; skipped" % w.get("id"))
            return None
        akey = ctx["assets"].add(url, rect[2], rect[3])
        if not akey:
            return None
        s["src"] = akey

    elif wtype == "navigate-page":
        tgt = cfg.get("targetPageId")
        if not tgt:
            warn("navigate-page %s has no target; skipped" % w.get("id"))
            return None
        out["g"] = tgt
        if c := colour(cfg.get("buttonBackgroundColor")):
            s["bg"] = c
        if c := colour(cfg.get("buttonTextColor")):
            s["fg"] = c
        if cfg.get("showIcon"):
            s["icon"] = True

    elif wtype == "form":
        fields = []
        for f in cfg.get("formFields") or []:
            fields.append({
                "id": f.get("id"),
                "l": f.get("label") or "",
                "ty": f.get("type") or "text",
                "ph": f.get("placeholder") or "",
                "d": f.get("defaultValue") or "",
            })
        if not fields:
            warn("form %s has no fields; skipped" % w.get("id"))
            return None
        s["fields"] = fields

    else:
        ctx["unsupported"].add(wtype)
        return None

    if s:
        out["s"] = s

    if events := extract_events(cfg):
        out["e"] = events

    # Fonts are reported, never embedded -- the device maps onto its
    # built-in faces.
    for fk in ("customFontName", "switchTitleCustomFontName"):
        if cfg.get(fk):
            ctx["fonts"].add(cfg[fk])

    return out


# ----------------------------------------------------------------- driver

_warnings = []


def warn(msg):
    _warnings.append(msg)
    print("  warn: " + msg, file=sys.stderr)


def info(msg):
    print("  " + msg)


def short(s, n=58):
    return s if len(s) <= n else s[:n - 3] + "..."


def transcode(src, outdir, mode="desktop", verbose=False):
    with open(src, "r", encoding="utf-8") as f:
        doc = json.load(f)

    os.makedirs(outdir, exist_ok=True)
    assets = AssetStore(outdir, verbose)

    ctx = {
        "assets": assets,
        "fonts": set(),
        "unsupported": set(),
        "font_scaled": [],
    }

    # Every event in the export points at the same device; hoist it to
    # the top of the spec so widgets only carry it when they differ.
    targets = set(re.findall(r'"targetId":\s*"([^"]+)"', json.dumps(doc)))
    default_target = next(iter(targets)) if len(targets) == 1 else None

    pages = []
    for p in doc.get("pages", []):
        ctx["layout"] = resolve_layout(p, mode)
        lkey, scale, dx, dy = ctx["layout"]

        widgets = []
        for w in p.get("widgets", []):
            if cw := convert_widget(w, ctx):
                if default_target:
                    for e in cw.get("e", []):
                        if e.get("to") == default_target:
                            e.pop("to")   # falls back to spec["target"]
                widgets.append(cw)

        # zIndex < 0 widgets are hit areas sitting behind their artwork;
        # LVGL paints in creation order, so sort rather than z-fight.
        widgets.sort(key=lambda x: x.get("z", 0))

        page = {
            "id": p.get("id"),
            "name": p.get("name") or "",
            "w": widgets,
        }
        # A multi-page dashboard is one design, but the builder only
        # writes a background on the pages where it was set explicitly.
        # Inherit rather than letting a page drop through to the
        # firmware's own cabin theme halfway through a flow.
        bg = (colour((p.get("layout") or {}).get("backgroundColor"))
              or colour((doc.get("theme") or {}).get("backgroundColor"))
              or (pages[0].get("bg") if pages else None))
        if bg:
            page["bg"] = bg
        pages.append(page)

        print("page %-22s %2d widgets  layout=%s scale=%.3f offset=%d,%d"
              % (repr(p.get("name")), len(widgets), lkey, scale, dx, dy))

    spec = {"v": SPEC_VERSION, "name": doc.get("name") or "", "pages": pages}
    if default_target:
        spec["target"] = default_target
    if pages:
        spec["home"] = pages[0]["id"]
    if bg := colour((doc.get("theme") or {}).get("backgroundColor")):
        spec["bg"] = bg

    path = os.path.join(outdir, "dashboard.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(spec, f, separators=(",", ":"))

    # ---- report ----
    src_sz = os.path.getsize(src)
    spec_sz = os.path.getsize(path)
    asset_sz = sum(assets.written.values())

    print()
    print("input        %9s  %s" % (kb(src_sz), os.path.basename(src)))
    print("spec         %9s  dashboard.json  (%.0fx smaller)"
          % (kb(spec_sz), src_sz / max(spec_sz, 1)))
    print("assets       %9s  %d blob(s)" % (kb(asset_sz), len(assets.written)))
    print("on device    %9s  spec + assets" % kb(spec_sz + asset_sz))

    if ctx["fonts"]:
        print("\nfonts dropped (mapped to built-in Montserrat):")
        for f in sorted(ctx["fonts"]):
            print("  - %s" % f)
    if verbose and ctx["font_scaled"]:
        print("\nfont sizes snapped to built-in faces:")
        for name, was, now in ctx["font_scaled"]:
            print("  %-24s %gpx -> %dpx" % (short(name, 24), was, now))
    if ctx["unsupported"]:
        print("\nunsupported widget types (skipped, no vtable yet):")
        for t in sorted(ctx["unsupported"]):
            print("  - %s" % t)
    if doc.get("script", "").strip():
        n = len(doc["script"].splitlines())
        print("\nscript block NOT emitted: %d lines of JavaScript." % n)
        print("  There is no JS engine on the device. Port any behaviour you")
        print("  need to a declarative rule or a firmware command handler.")
    if _warnings:
        print("\n%d warning(s) above." % len(_warnings))

    print("\nwrote %s" % path)
    return spec


def kb(n):
    return "%.1f KB" % (n / 1024) if n < 1024 * 1024 else "%.2f MB" % (n / 1048576)


def main():
    ap = argparse.ArgumentParser(
        description="Hyperwisor dashboard JSON -> slim on-device spec")
    ap.add_argument("input", help="dashboard export (.json)")
    ap.add_argument("-o", "--out", required=True, help="output directory")
    ap.add_argument("--layout", choices=("desktop", "hmi"), default="desktop",
                    help="layout space to map from (default: desktop, scaled "
                         "to fit; 'hmi' uses an 800x480 hmiLayout block)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    transcode(a.input, a.out, a.layout, a.verbose)
    return 1 if _warnings else 0


if __name__ == "__main__":
    sys.exit(main())
