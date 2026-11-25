#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Make the rescue stick's image: the kernel and the root filesystem as one file.

  tools/rescue_image.py --kernel plana-b.bin --rootfs PARTITION.IMG --out RESCUE.IMG

The rescue installer (recovery/plan-a/init-rescue) writes the whole of the
kernel and rootfs partitions, 0x160000 + 0x7100000, from this one file. It
finds it either in RAM, where U-Boot loaded it from the stick in the CARPLAY
socket (0x04000000), or as a file on a stick Linux can read. The layout, in
4096-byte steps so that `dd bs=4096` reaches every part:

  block 0     the header, text, NUL-padded:
                AUTOTOP-RESCUE 1
                window 0x160000 0x7100000
                manifest OFFSET LENGTH      (bytes from the file's start)
                image OFFSET LENGTH
                manifest_sha256 HEX
                mode install|plan           (plan: check and compare, write nothing)
                note ...
  then        the manifest (tools/restore_manifest.py: the window, the image's
              SHA-256 and one SHA-256 per eraseblock), padded
  then        the window's bytes: the kernel partition (the kernel, then ff)
              and the rootfs partition image (tools/plana_b_image.py)

Nothing is signed: whoever can put a stick into that socket can start any
kernel there (U-Boot asks for no signature). The hashes are against a file
that was damaged on the way, not against a person.
"""
import argparse
import hashlib
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import restore_manifest  # noqa: E402

KERNEL_AT, KERNEL_SIZE = 0x160000, 0x400000
ROOTFS_AT, ROOTFS_SIZE = 0x560000, 0x6d00000
STEP = 4096
RAM_AT, RAM_END = 0x04000000, 0x0B400000    # above U-Boot's own 64 MiB, below the panel's memory
STOCK_KERNEL_READ = 3342040                 # what U-Boot's nandboot reads of the kernel partition


def pad(data, step=STEP, fill=b'\0'):
    return data + fill * (-len(data) % step)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel', required=True, type=Path, help='the kernel as U-Boot boots it (plana-b.bin)')
    ap.add_argument('--rootfs', required=True, type=Path, help='the rootfs partition image (PARTITION.IMG)')
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--note', default='')
    ap.add_argument('--max-fresh-corrections', type=int, default=2)
    ap.add_argument('--plan-only', action='store_true',
                    help='the installer checks and compares but writes nothing (a first boot at the bench)')
    args = ap.parse_args()

    kernel, rootfs = args.kernel.read_bytes(), args.rootfs.read_bytes()
    if not 0 < len(kernel) <= STOCK_KERNEL_READ:
        sys.exit(f'{args.kernel}: {len(kernel)} bytes; U-Boot reads {STOCK_KERNEL_READ} of the kernel partition')
    if len(rootfs) != ROOTFS_SIZE:
        sys.exit(f'{args.rootfs}: {len(rootfs)} bytes, not the rootfs partition ({ROOTFS_SIZE})')
    window = kernel + b'\xff' * (KERNEL_SIZE - len(kernel)) + rootfs
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'window.img'
        src.write_bytes(window)
        _, man = restore_manifest.build(src, KERNEL_AT, KERNEL_AT, len(window), Path(tmp) / 'out',
                                        args.note or None, args.max_fresh_corrections)
        manifest = man.read_bytes().replace(str(src).encode(), b'the rescue window')
    man_at = STEP
    img_at = man_at + len(pad(manifest))
    header = (f'AUTOTOP-RESCUE 1\nwindow 0x{KERNEL_AT:x} 0x{len(window):x}\n'
              f'manifest {man_at} {len(manifest)}\nimage {img_at} {len(window)}\n'
              f'manifest_sha256 {hashlib.sha256(manifest).hexdigest()}\n'
              f'mode {"plan" if args.plan_only else "install"}\n'
              f'note {args.note}\n').encode()
    if len(header) > STEP:
        sys.exit('the note is too long')
    out = pad(header) + pad(manifest) + window
    if RAM_AT + len(out) > RAM_END:
        sys.exit(f'{len(out)} bytes do not fit between 0x{RAM_AT:x} and 0x{RAM_END:x}')
    args.out.write_bytes(out)
    print(f'{args.out}: {len(out)} bytes; kernel {hashlib.sha256(kernel).hexdigest()[:16]} '
          f'rootfs {hashlib.sha256(rootfs).hexdigest()[:16]} window {hashlib.sha256(window).hexdigest()[:16]}; '
          f'in RAM it ends at 0x{RAM_AT + len(out):x} (limit 0x{RAM_END:x})')


if __name__ == '__main__':
    main()
