#!/usr/bin/env python3
"""Build a nand-restore input: window image plus manifest, from a verified capture.

    restore_manifest.py --source CAPTURE/A/mtd5.ecc --source-base 0x160000 \
        --window 0x160000:0x20000 --out DIR

Writes DIR/RESTORE.IMG (exactly the window's corrected main bytes) and
DIR/RESTORE.MAN (format read by recovery/nand-restore.c). The window rules
match recovery/bm168_nand_format.h: erase-aligned, non-empty, inside kernel
and rootfs. Host-only; it never touches a device.
"""
import argparse
import hashlib
import sys
from pathlib import Path

ERASE, PAGE = 131072, 2048
WINDOW_MIN, WINDOW_MAX = 0x160000, 0x7260000


BOOTLOGO = (0x7860000, 0x80000)   # the boot picture's partition: only for a writer built for it


def window_valid(start, size, bootlogo=False):
    if size <= 0 or start % ERASE or size % ERASE:
        return False
    if bootlogo and BOOTLOGO[0] <= start < BOOTLOGO[0] + BOOTLOGO[1] and size <= BOOTLOGO[0] + BOOTLOGO[1] - start:
        return True
    return WINDOW_MIN <= start <= WINDOW_MAX and size <= WINDOW_MAX - start


def build(source, source_base, start, size, out, note=None, max_fresh=0, bootlogo=False):
    data = Path(source).read_bytes()
    if not window_valid(start, size, bootlogo):
        raise ValueError('window must be erase-aligned and inside kernel/rootfs')
    if source_base % ERASE or start < source_base or start + size > source_base + len(data):
        raise ValueError('window is not inside the source image')
    image = data[start - source_base:start - source_base + size]
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    erased = b'\xff' * PAGE
    program = sum(image[i:i + PAGE] != erased for i in range(0, size, PAGE))
    lines = ['AUTOTOP-RESTORE v1',
             f'# source {source} sha256 {hashlib.sha256(data).hexdigest()} base 0x{source_base:x}',
             f'# pages_to_program {program}']
    if note:
        lines.append(f'# {note}')
    if not 0 <= max_fresh <= 7:
        raise ValueError('max_fresh_corrections must be 0..7')
    lines += [f'window 0x{start:x} 0x{size:x}', f'image_sha256 {hashlib.sha256(image).hexdigest()}']
    if max_fresh:
        lines.append(f'max_fresh_corrections {max_fresh}')
    lines += [f'block {i} {hashlib.sha256(image[i * ERASE:(i + 1) * ERASE]).hexdigest()}'
              for i in range(size // ERASE)]
    (out / 'RESTORE.IMG').write_bytes(image)
    (out / 'RESTORE.MAN').write_text('\n'.join(lines) + '\n')
    return out / 'RESTORE.IMG', out / 'RESTORE.MAN'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--source-base', required=True, type=lambda s: int(s, 0),
                        help='physical main-area offset of the source file')
    parser.add_argument('--window', required=True,
                        help='START:SIZE physical main-area window, hex or decimal')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--note')
    parser.add_argument('--max-fresh-corrections', type=int, default=0,
                        help='corrected bits tolerated per freshly written block (default 0)')
    parser.add_argument('--bootlogo', action='store_true',
                        help='the window may be the boot picture partition (for the writer built for it)')
    args = parser.parse_args()
    start, size = (int(v, 0) for v in args.window.split(':'))
    image, manifest = build(args.source, args.source_base, start, size, args.out, args.note,
                            args.max_fresh_corrections, args.bootlogo)
    print(manifest.read_text(), end='')
    return 0


if __name__ == '__main__':
    sys.exit(main())
