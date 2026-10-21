"""
Minimal PNG reader for the HDS transcoder.

Uses Pillow when it is installed (better resampling, more exotic PNG
flavours); falls back to a pure-stdlib decoder so the tool works on a
clean checkout with no pip install. The fallback covers what real
dashboard assets actually use: 8-bit non-interlaced greyscale, RGB,
palette and RGBA.

Both paths return (width, height, rgba_bytes) with 4 bytes per pixel.
"""

import struct
import zlib

try:
    from PIL import Image
    _HAVE_PIL = True
except ImportError:
    _HAVE_PIL = False


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _unfilter(raw, w, h, bpp):
    """Reverse the per-scanline PNG filters. Returns packed pixel rows."""
    stride = w * bpp
    out = bytearray(stride * h)
    pos = 0
    for y in range(h):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        up = out[(y - 1) * stride:y * stride] if y else bytes(stride)

        if ftype == 1:      # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + up[i]) & 0xFF
        elif ftype == 3:    # Average
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + up[i]) >> 1)) & 0xFF
        elif ftype == 4:    # Paeth
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                ul = up[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(left, up[i], ul)) & 0xFF
        elif ftype != 0:
            raise ValueError("unsupported PNG filter %d" % ftype)

        out[y * stride:(y + 1) * stride] = line
    return out


def _decode_stdlib(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos, idat, palette, trns = 8, bytearray(), None, None
    w = h = depth = ctype = interlace = 0

    while pos < len(data):
        (clen,) = struct.unpack(">I", data[pos:pos + 4])
        ctag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + clen]
        pos += 12 + clen                      # 4 len + 4 tag + body + 4 crc

        if ctag == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif ctag == b"PLTE":
            palette = body
        elif ctag == b"tRNS":
            trns = body
        elif ctag == b"IDAT":
            idat += body
        elif ctag == b"IEND":
            break

    if depth != 8:
        raise ValueError("only 8-bit PNGs are supported (got %d)" % depth)
    if interlace:
        raise ValueError("interlaced PNGs are not supported")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    rows = _unfilter(zlib.decompress(bytes(idat)), w, h, channels)

    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        s, d = i * channels, i * 4
        if ctype == 0:                                  # grey
            g = rows[s]
            rgba[d:d + 4] = bytes((g, g, g, 255))
        elif ctype == 2:                                # RGB
            rgba[d:d + 3] = rows[s:s + 3]
            rgba[d + 3] = 255
        elif ctype == 3:                                # palette
            idx = rows[s]
            rgba[d:d + 3] = palette[idx * 3:idx * 3 + 3]
            rgba[d + 3] = trns[idx] if trns and idx < len(trns) else 255
        elif ctype == 4:                                # grey + alpha
            g = rows[s]
            rgba[d:d + 4] = bytes((g, g, g, rows[s + 1]))
        else:                                           # RGBA
            rgba[d:d + 4] = rows[s:s + 4]
    return w, h, bytes(rgba)


def decode(data):
    """PNG/JPEG bytes -> (w, h, rgba)."""
    if _HAVE_PIL:
        import io
        im = Image.open(io.BytesIO(data)).convert("RGBA")
        return im.width, im.height, im.tobytes()
    return _decode_stdlib(data)


def resize(w, h, rgba, tw, th):
    """Box-filter downscale (nearest on upscale) to tw x th."""
    if (w, h) == (tw, th):
        return rgba
    if _HAVE_PIL:
        im = Image.frombytes("RGBA", (w, h), rgba)
        im = im.resize((tw, th), Image.LANCZOS)
        return im.tobytes()

    out = bytearray(tw * th * 4)
    for y in range(th):
        y0, y1 = y * h // th, max(y * h // th + 1, (y + 1) * h // th)
        for x in range(tw):
            x0, x1 = x * w // tw, max(x * w // tw + 1, (x + 1) * w // tw)
            r = g = b = a = n = 0
            for sy in range(y0, y1):
                base = sy * w * 4
                for sx in range(x0, x1):
                    s = base + sx * 4
                    # Weight colour by alpha so transparent pixels do not
                    # bleed their (undefined) RGB into the average.
                    av = rgba[s + 3]
                    r += rgba[s] * av
                    g += rgba[s + 1] * av
                    b += rgba[s + 2] * av
                    a += av
                    n += 1
            d = (y * tw + x) * 4
            if a:
                out[d:d + 4] = bytes((r // a, g // a, b // a, a // n))
            else:
                out[d:d + 4] = b"\0\0\0\0"
    return bytes(out)
