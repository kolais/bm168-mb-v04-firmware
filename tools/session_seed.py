#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The unit's own session files, from the builder's captures of its flash
(run in the build container; `make session`):

  session_seed.py ROOTFS.ubi USERDATA.ubi OUT_DIR

The image starts the vendor's CarPlay and Bluetooth programs on the data
they had on that unit under the stock firmware: the files of its `userdata`
partition (settings, CarPlay's keychain) and its Bluetooth pairing list.
They are private to the unit and never part of this repository. Written to
OUT_DIR (the build's local/state/):

  DATA.TAR    the files of the userdata partition, as /data
  GOC.TAR     etc/goc/bt_conf.ini and sc_db.db of the rootfs
  PLANA.CFG   the session's settings, with a name and a first passphrase for
              the unit's Wi-Fi made here (only if there is none yet: edit it)

What the unit changes later (pairings, settings, its name) it keeps by
itself on its userdata partition; these files are only where it starts from.
"""
import secrets
import subprocess
import sys
import tempfile
from pathlib import Path


def extract(image, where, volume):
    log = where / 'extract.log'
    with log.open('w') as f:
        subprocess.run(['ubireader_extract_files', '-k', '-x', '-o', str(where / 'x'), str(image)],
                       check=True, stdout=f, stderr=subprocess.STDOUT)
    roots = list((where / 'x').glob(f'*/{volume}'))
    if len(roots) != 1:
        sys.exit(f'{image}: expected one "{volume}" volume, found {len(roots)}')
    return roots[0]


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    rootfs, userdata, out = (Path(a) for a in sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    tar = ['tar', '--sort=name', '--numeric-owner', '--owner=0', '--group=0', '-c', '-f']
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        data = extract(userdata, Path(a), 'userdata')
        subprocess.run([*tar, str(out / 'DATA.TAR'), '-C', str(data), '.'], check=True)
        goc = extract(rootfs, Path(b), 'rootfs') / 'etc/goc'
        have = [n for n in ('bt_conf.ini', 'sc_db.db') if (goc / n).is_file()]
        subprocess.run([*tar, str(out / 'GOC.TAR'), '-C', str(goc), *have], check=True)
    cfg = out / 'PLANA.CFG'
    if not cfg.exists():
        letters = 'abcdefghijkmnopqrstuvwxyz23456789'
        cfg.write_text('\r\n'.join([
            'DATA=DATA.TAR', 'GOC=GOC.TAR', 'LAUNCHER=autotop',
            'AP_SSID=BM168-' + ''.join(secrets.choice(letters) for _ in range(4)),
            'AP_PSK=' + ''.join(secrets.choice(letters) for _ in range(16)), '']))
    print(f'{out}: DATA.TAR ({(out / "DATA.TAR").stat().st_size} bytes), GOC.TAR ({", ".join(have) or "empty"}), PLANA.CFG')


if __name__ == '__main__':
    main()
