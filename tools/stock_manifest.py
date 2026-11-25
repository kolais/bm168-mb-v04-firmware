#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Is this the stock software the build was made for?

The build takes the vendor's programs and libraries from the builder's own
unit: a capture of its rootfs partition (docs/stock-capture.md), which is
never part of this repository. Another unit's capture is not the same file
(its filesystem was written to while the unit ran), so it is judged file by
file against board/stock-files.sha256: the names, kinds and SHA-256 of every
file of the stock version this work was done on (BM168_V3.1.21-20230216).
The hashes say nothing about the files' contents.

  stock_manifest.py make ROOT OUT     write the manifest of an extracted rootfs
  stock_manifest.py check ROOT MANIFEST

`check` exits 0 if every program, library and other fixed file is there and
the same. Files a running unit writes to (under etc, data, var, tmp, root,
and a few at the top) may differ: they are listed and do not fail the check. A
missing or different fixed file does: the unit has another stock version,
and the build's assumptions about it (addresses, D-Bus names, service files)
would not hold.
"""
import hashlib
import os
import sys
from pathlib import Path

CHANGING = ('etc/', 'data/', 'var/', 'tmp/', 'root/', '.ash_history', '.android/', 'Settings')


def entries(root):
    root = Path(root)
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files + [d for d in dirs if (Path(base) / d).is_symlink()]):
            p = Path(base) / name
            rel = p.relative_to(root).as_posix()
            if p.is_symlink():
                yield rel, 'link ' + os.readlink(p)
            elif p.is_file():
                yield rel, 'file ' + hashlib.sha256(p.read_bytes()).hexdigest()
            else:
                yield rel, 'other'


def load(path):
    out = {}
    for line in Path(path).read_text().splitlines():
        if line and not line.startswith('#'):
            what, rel = line.split('\t')
            out[rel] = what
    return out


def check(root, manifest):
    want, got = load(manifest), dict(entries(root))
    fixed, changing = [], []
    for rel in sorted(set(want) | set(got)):
        if want.get(rel) == got.get(rel):
            continue
        how = 'missing' if rel not in got else 'not in the manifest' if rel not in want else 'different'
        (changing if rel.startswith(CHANGING) else fixed).append(f'{how}: {rel}')
    return fixed, changing


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'make':
        lines = [f'{what}\t{rel}' for rel, what in entries(sys.argv[2])]
        Path(sys.argv[3]).write_text('# kind and SHA-256 or link target, then the path: tools/stock_manifest.py\n' +
                                     '\n'.join(lines) + '\n')
        print(f'{sys.argv[3]}: {len(lines)} entries')
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == 'check':
        fixed, changing = check(sys.argv[2], sys.argv[3])
        for line in changing:
            print('  (may differ) ' + line)
        for line in fixed:
            print('  ' + line)
        print(f'{len(fixed)} fixed files differ, {len(changing)} in directories a running unit writes to')
        return 1 if fixed else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
