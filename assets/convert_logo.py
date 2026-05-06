# Convert a PNG to LVGL v8 C array (CF_TRUE_COLOR_ALPHA, RGB565 + 1B alpha).
# Usage: python convert_logo.py logo.png logo.c logo
import sys
from PIL import Image

def main():
    src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
    im = Image.open(src).convert("RGBA")
    w, h = im.size
    px = im.load()

    out = []
    out.append('#include "lvgl.h"')
    out.append("")
    out.append("#ifndef LV_ATTRIBUTE_MEM_ALIGN")
    out.append("#define LV_ATTRIBUTE_MEM_ALIGN")
    out.append("#endif")
    out.append("")
    out.append(f"static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_map[] = {{")

    bytes_per_line = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            lo = rgb565 & 0xFF
            hi = (rgb565 >> 8) & 0xFF
            bytes_per_line.extend([lo, hi, a])

    # emit 16 bytes per line
    for i in range(0, len(bytes_per_line), 16):
        chunk = bytes_per_line[i:i+16]
        out.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    out.append("};")
    out.append("")
    out.append(f"const lv_img_dsc_t {name} = {{")
    out.append("    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,")
    out.append("    .header.always_zero = 0,")
    out.append("    .header.reserved = 0,")
    out.append(f"    .header.w = {w},")
    out.append(f"    .header.h = {h},")
    out.append(f"    .data_size = {w*h} * LV_IMG_PX_SIZE_ALPHA_BYTE,")
    out.append(f"    .data = {name}_map,")
    out.append("};")
    out.append("")

    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print(f"Wrote {dst}: {w}x{h}, {len(bytes_per_line)} bytes of pixel data")

if __name__ == "__main__":
    main()
