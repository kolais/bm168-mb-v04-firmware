#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Make a signed app bundle for the unit (recovery/plan-a/app-slot).

  tools/app_bundle.py --tree DIR --version N [--system M] --key KEY.pem --out FILE

DIR holds the app as it lies under /opt/autotop: bin/ (the launcher and its
tools), plana-up.sh, state-save, carplay-run, carplay-icon.png. The image
build leaves it in dist/<variant>/app. The bundle is a plain tar of
"manifest", "manifest.sig" (RSA with SHA-256, by KEY) and "app.zip".

The key's private half never goes into the repository; its public half is
local/keys/update-key.pem, in every image. `--check PUB.pem` verifies a
finished bundle with the host's openssl the way the unit will.
"""
import argparse
import hashlib
import io
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

NEEDED = ("bin/launcherd", "plana-up.sh", "state-save", "carplay-run")


def app_zip(tree):
    """The files in a fixed order with fixed dates, so the same tree gives the same bytes."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(tree.rglob("*")):
            if p.is_dir() or p.is_symlink():
                continue
            info = zipfile.ZipInfo(p.relative_to(tree).as_posix(), date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100755 << 16
            z.writestr(info, p.read_bytes())
    return buf.getvalue()


def member(tar, name, data):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    tar.addfile(info, io.BytesIO(data))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tree", type=Path)
    ap.add_argument("--version", type=int)
    ap.add_argument("--system", type=int, default=1)
    ap.add_argument("--key", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--check", type=Path, metavar="PUB.pem")
    args = ap.parse_args()
    if not (args.tree and args.version and args.key and args.out):
        ap.error("--tree, --version, --key and --out are needed")
    if not 0 < args.version < 10 ** 9 or not 0 < args.system < 10 ** 9:
        ap.error("version and system are whole numbers below 1,000,000,000")
    missing = [n for n in NEEDED if not (args.tree / n).is_file()]
    if missing:
        sys.exit(f"{args.tree}: missing {', '.join(missing)}")

    payload = app_zip(args.tree)
    manifest = (f"autotop-app 1\nversion {args.version}\nsystem {args.system}\n"
                f"sha256 {hashlib.sha256(payload).hexdigest()}\n").encode()
    with tempfile.TemporaryDirectory() as tmp:
        m, s = Path(tmp) / "manifest", Path(tmp) / "manifest.sig"
        m.write_bytes(manifest)
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(args.key), "-out", str(s), str(m)], check=True)
        sig = s.read_bytes()
        if args.check:
            subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(args.check), "-signature", str(s), str(m)],
                           check=True, stdout=subprocess.DEVNULL)
    with tarfile.open(args.out, "w", format=tarfile.USTAR_FORMAT) as tar:
        member(tar, "manifest", manifest)
        member(tar, "manifest.sig", sig)
        member(tar, "app.zip", payload)
    print(f"{args.out}: version {args.version} for system {args.system} and later, "
          f"{args.out.stat().st_size} bytes (files {len(payload)} bytes packed)")


if __name__ == "__main__":
    main()
