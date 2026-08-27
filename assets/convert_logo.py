# Convert a PNG to an LVGL v8 C array.
#
#   python convert_logo.py <src.png> <dst.c> <symbol> [options]
#
# Options:
#   --width N     scale so the output is N px wide (aspect preserved)
#   --crop        trim surrounding white/transparent margin first
#   --no-alpha    emit CF_TRUE_COLOR (RGB565, 2 B/px) instead of
#                 CF_TRUE_COLOR_ALPHA (3 B/px). Use when the image sits
#                 on a known solid background -- saves a third of the
#                 flash, which matters: these arrays are linked into the
#                 app partition, and a full-size photo can easily exceed
#                 the free space.
#   --bg RRGGBB   background to flatten onto when --no-alpha (default fff)
#
# Examples:
#   python convert_logo.py logo.png ../components/app/ui/logo_x.c logo_x
#   python convert_logo.py brand.png out.c brand --crop --width 560 --no-alpha

import sys
from PIL import Image, ImageChops


def autocrop(im):
    """Trim uniform white/transparent border, keeping only real content."""
    flat = Image.alpha_composite(
        Image.new("RGBA", im.size, (255, 255, 255, 255)), im).convert("RGB")
    bbox = ImageChops.difference(
        flat, Image.new("RGB", im.size, (255, 255, 255))).getbbox()
    return im.crop(bbox) if bbox else im


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
    args = sys.argv[4:]

    want_crop = "--crop" in args
    use_alpha = "--no-alpha" not in args
    width = None
    if "--width" in args:
        width = int(args[args.index("--width") + 1])
    bg = (255, 255, 255)
    if "--bg" in args:
        h = args[args.index("--bg") + 1].lstrip("#")
        bg = tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))

    im = Image.open(src).convert("RGBA")
    if want_crop:
        im = autocrop(im)
    if width:
        h = max(1, round(im.height * width / im.width))
        im = im.resize((width, h), Image.LANCZOS)
    if not use_alpha:
        # Flatten onto the known background; anti-aliased edges stay
        # correct because they blend against the colour they'll sit on.
        im = Image.alpha_composite(
            Image.new("RGBA", im.size, bg + (255,)), im)

    w, h = im.size
    px = im.load()

    data = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data.append(rgb565 & 0xFF)
            data.append((rgb565 >> 8) & 0xFF)
            if use_alpha:
                data.append(a)

    cf = "LV_IMG_CF_TRUE_COLOR_ALPHA" if use_alpha else "LV_IMG_CF_TRUE_COLOR"
    size_expr = (f"{w*h} * LV_IMG_PX_SIZE_ALPHA_BYTE" if use_alpha
                 else f"{w*h} * 2")

    out = ['#include "lvgl.h"', "",
           "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
           "#define LV_ATTRIBUTE_MEM_ALIGN",
           "#endif", "",
           f"static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_map[] = {{"]
    for i in range(0, len(data), 16):
        out.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    out += ["};", "",
            f"const lv_img_dsc_t {name} = {{",
            f"    .header.cf = {cf},",
            "    .header.always_zero = 0,",
            "    .header.reserved = 0,",
            f"    .header.w = {w},",
            f"    .header.h = {h},",
            f"    .data_size = {size_expr},",
            f"    .data = {name}_map,",
            "};", ""]

    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print(f"Wrote {dst}: {w}x{h}, {len(data)} bytes "
          f"({'RGB565+A' if use_alpha else 'RGB565'}) = {len(data)/1024:.0f} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
