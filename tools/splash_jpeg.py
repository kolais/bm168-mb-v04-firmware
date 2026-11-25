#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A picture as the unit's boot picture: the JPEG U-Boot reads from the bootlogo partition.

  tools/splash_jpeg.py IN.png OUT.jpg [--max 188530] [--quality 99]

U-Boot reads `bootlogo_size` bytes (0x2e072 = 188,530 in the unit's stored
environment) from the partition at 0x7860000 and decodes them: the stock
picture is a baseline JPEG, 1280 x 480, three components, none subsampled.
This makes one of the same kind and no larger, so the environment stays as
it is; a shorter file is right as it is, U-Boot's decoder stops at the
picture's end marker (proven with a 27,311-byte picture on 2026-11-18,
docs/splash-screens.md). Nothing is added to the encoder's output.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

W, H = 1280, 480


def segments(d):
    """(marker, offset, length) of each segment up to the scan."""
    out, i = [], 2
    while d[i] == 0xff and d[i + 1] != 0xda:
        n = int.from_bytes(d[i + 2:i + 4], 'big')
        out.append((d[i + 1], i, n))
        i += 2 + n
    return out, i


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('png', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--max', type=int, default=0x2e072)
    ap.add_argument('--quality', type=int, default=99)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        jpg = Path(tmp) / 'p.jpg'
        subprocess.run(['magick', str(args.png), '-strip', '-colorspace', 'sRGB', '-sampling-factor', '1x1',
                        '-interlace', 'none', '-quality', str(args.quality), str(jpg)], check=True)
        d = jpg.read_bytes()
    segs, scan = segments(d)
    sof = [(m, at) for m, at, _ in segs if 0xc0 <= m <= 0xcf and m not in (0xc4, 0xc8, 0xcc)]
    if d[:2] != b'\xff\xd8' or d[-2:] != b'\xff\xd9' or len(sof) != 1 or sof[0][0] != 0xc0:
        sys.exit('not a baseline JPEG with one frame')
    at = sof[0][1]
    h, w, comps = int.from_bytes(d[at + 5:at + 7], 'big'), int.from_bytes(d[at + 7:at + 9], 'big'), d[at + 9]
    if (w, h, comps) != (W, H, 3) or any(d[at + 11 + 3 * k] != 0x11 for k in range(3)):
        sys.exit(f'the picture must be {W} x {H}, three components, none subsampled (got {w} x {h}, {comps})')
    if any(m == 0xdd for m, _, _ in segs):
        sys.exit('restart intervals are not wanted')
    if len(d) > args.max:
        sys.exit(f'{len(d)} bytes at quality {args.quality}: larger than {args.max}; lower the quality')
    args.out.write_bytes(d)
    print(f'{args.out}: {len(d)} bytes at quality {args.quality} (at most {args.max}), baseline, {W} x {H}, 4:4:4')


if __name__ == '__main__':
    main()
