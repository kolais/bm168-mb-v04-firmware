#!/usr/bin/env python3
"""Build the option B rootfs-partition image (run in the build container).

    plana_b_image.py --squashfs SQF --state-dir DIR --out DIR

Volume 0 `plana-root`: the Plan A squashfs (dynamic, exact size, no
autoresize), read by the kernel through ubiblock. Volume 1 `plana-state`
(static): a tar of the session files in --state-dir (PLANA.CFG, DATA*.TAR,
GOC*.TAR), unpacked into RAM by init-b. The state is private (keys); the
output belongs under backups/, never in git or dist/.

The UBI image is padded to the whole partition (872 PEBs) with PEBs that
carry only a valid erase-counter header, as ubiformat leaves them. A
read-only attach then finds no empty PEBs to erase, so UBI never needs to
write. Output: PARTITION.IMG (exactly the partition size) and a JSON record.
"""
import argparse
import hashlib
import io
import json
import struct
import subprocess
import tarfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD = json.loads((ROOT / 'board/bm168.json').read_text())
PEB, PAGE, LEB = BOARD['peb_size'], BOARD['min_io_size'], BOARD['leb_size']
PART = BOARD['rootfs_size']
SEQ = BOARD['ubi_image_sequence'] + 2
STATE_NAMES = ('PLANA.CFG',)


def ubi_crc(data):
    # UBI: crc32_le seeded with 0xffffffff and no final inversion; zlib's
    # CRC-32 is the same with a final inversion.
    return ~zlib.crc32(data) & 0xffffffff


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--squashfs', required=True, type=Path)
    ap.add_argument('--state-dir', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in a.state_dir.iterdir()
                   if p.name in STATE_NAMES or p.name.startswith(('DATA', 'GOC')) and p.suffix.upper() == '.TAR')
    if not any(p.name == 'PLANA.CFG' for p in files):
        raise SystemExit('state dir needs PLANA.CFG')
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for p in files:
            info = tar.gettarinfo(str(p), arcname=p.name)
            info.uid = info.gid = 0
            info.uname = info.gname = 'root'
            info.mtime = BOARD['source_date_epoch']
            info.mode = 0o600
            with p.open('rb') as f:
                tar.addfile(info, f)
    state = a.out / 'plana-state.tar'
    state.write_bytes(buf.getvalue())
    sqf_size = a.squashfs.stat().st_size
    ini = a.out / 'plana-b.ini'
    ini.write_text(
        f'[plana-root]\nmode=ubi\nimage={a.squashfs}\nvol_type=dynamic\nvol_id=0\nvol_name=plana-root\n'
        f'vol_size={-(-sqf_size // LEB) * LEB}\n'
        f'[plana-state]\nmode=ubi\nimage={state}\nvol_type=static\nvol_id=1\nvol_name=plana-state\n'
        f'vol_size={-(-state.stat().st_size // LEB) * LEB}\n')
    ubi = a.out / 'plana-b.ubi'
    subprocess.run(['ubinize', '-o', str(ubi), '-p', str(PEB), '-m', str(PAGE), '-s', str(PAGE),
                    '-O', str(BOARD['vid_header_offset']), '-Q', str(SEQ), str(ini)], check=True)
    image = bytearray(ubi.read_bytes())
    if len(image) % PEB:
        raise SystemExit('ubinize output is not a whole number of PEBs')
    used = len(image) // PEB
    # Padding PEB: the EC header page of ubinize's PEB 0 (same EC, offsets and
    # image sequence for every PEB), checked, then erased bytes.
    ec = bytes(image[:64])
    magic, version, ec_count, vid_off, data_off, seq, crc = struct.unpack('>IB3xQIII32xI', ec)
    if magic != 0x55424923 or version != 1 or seq != SEQ or vid_off != BOARD['vid_header_offset']:
        raise SystemExit(f'unexpected EC header: {ec.hex()}')
    if crc != ubi_crc(ec[:60]):
        raise SystemExit('EC header CRC check failed (UBI CRC model wrong)')
    pad = ec + b'\xff' * (PEB - 64)
    total = PART // PEB
    if used + 24 > total:
        raise SystemExit(f'{used} PEBs leave too little room in {total}')
    image += pad * (total - used)
    if len(image) != PART:
        raise SystemExit('image is not the partition size')
    out = a.out / 'PARTITION.IMG'
    out.write_bytes(image)
    record = {
        'partition': 'rootfs', 'start': '0x560000', 'size': hex(PART), 'pebs': total, 'ubi_pebs': used,
        'padding_pebs': total - used, 'image_sequence': SEQ,
        'squashfs': {'file': a.squashfs.name, 'bytes': sqf_size,
                     'sha256': hashlib.sha256(a.squashfs.read_bytes()).hexdigest()},
        'state_files': [p.name for p in files],
        'state_tar_sha256': hashlib.sha256(state.read_bytes()).hexdigest(),
        'partition_image_sha256': hashlib.sha256(image).hexdigest(),
    }
    (a.out / 'plana-b-image.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
