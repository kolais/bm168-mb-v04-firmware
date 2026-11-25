#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The owner's CarPlay icon in the form the stock one has: 120 x 120, 8-bit RGBA.

  tools/design_icon.py IN.png OUT.png

The design's file (design/assets/carplay-icon@2x.png) is RGB; stock's
/etc/icon_120x120.png, which carplay hands to the phone, is RGBA. Only the
alpha channel is added (opaque): no scaling, no other change.
"""
import struct
import sys
import zlib


def chunks(data):
    at = 8
    while at < len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        yield kind, data[at + 8:at + 8 + n]
        at += 12 + n


def unfilter(raw, w, h, bpp):
    out, prev = [], bytes(w * bpp)
    for y in range(h):
        f, line = raw[y * (w * bpp + 1)], bytearray(raw[y * (w * bpp + 1) + 1:(y + 1) * (w * bpp + 1)])
        for i in range(len(line)):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(line))
        prev = line
    return out


def main():
    data = open(sys.argv[1], "rb").read()
    parts = list(chunks(data))
    w, h, depth, kind, _, _, interlace = struct.unpack(">IIBBBBB", parts[0][1])
    if (w, h, depth, interlace) != (120, 120, 8, 0) or kind not in (2, 6):
        sys.exit(f"{sys.argv[1]}: want 120 x 120, 8-bit RGB or RGBA, not interlaced")
    bpp = 3 if kind == 2 else 4
    lines = unfilter(zlib.decompress(b"".join(d for k, d in parts if k == b"IDAT")), w, h, bpp)
    raw = bytearray()
    for line in lines:
        raw.append(0)
        for x in range(w):
            raw += line[x * bpp:x * bpp + 3] + (b"\xff" if bpp == 3 else line[x * bpp + 3:x * bpp + 4])

    def chunk(k, d):
        return struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d) & 0xffffffff)

    out = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    open(sys.argv[2], "wb").write(out)
    print(f"wrote {sys.argv[2]} ({len(out)} bytes, {w} x {h} RGBA)")


if __name__ == "__main__":
    main()
