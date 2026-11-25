#!/usr/bin/env python3
"""Verify every SDK archive member against Git and inventory it without execution.

The optional extraction is an inert review view: regular files only, mode 0444,
no symlinks, no archive-provided permissions. It is not a build tree.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import tarfile


def kind_of(data):
    for magic, kind in [(b'\x7fELF', 'elf'), (b'MZ', 'pe-or-dos'),
                        (b'!<arch>\n', 'ar'), (b'\x1f\x8b', 'gzip'),
                        (b'\xfd7zXZ\x00', 'xz'), (b'BZh', 'bzip2'),
                        (b'PK\x03\x04', 'zip'), (b'Rar!', 'rar'),
                        (b'7z\xbc\xaf\x27\x1c', '7zip'), (b'%PDF-', 'pdf'),
                        (b'\x4c\x00\x00\x00\x01\x14\x02\x00', 'windows-shortcut')]:
        if data.startswith(magic):
            return kind
    if data.startswith(b'#!'):
        return 'script'
    return 'binary' if b'\0' in data[:8192] else 'text'


def audit(archive_path, manifest_path, report, extract=None):
    expected = {r['path']: r for r in json.loads(manifest_path.read_text())}
    seen = set()
    counts = collections.Counter()
    total = 0
    report.mkdir(parents=True, exist_ok=True)
    if extract:
        extract.mkdir(parents=True, exist_ok=False)
    markers = {
        'known-shortcut-host': re.compile(rb'SIM-VIRTUAL-MAC', re.I),
        'known-shortcut-payload': re.compile(rb'(?:grkdv|bpmlvj)\.tmp', re.I),
        'private-key-material': re.compile(rb'-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----'),
        'shell-network-exec': re.compile(rb'(?:curl|wget)[^\r\n]{0,200}\|\s*(?:sh|bash)\b'),
    }
    with report.joinpath('inventory.jsonl').open('w') as inventory, tarfile.open(archive_path, 'r|gz') as tar:
        prefix = None
        for member in tar:
            parts = PurePosixPath(member.name).parts
            if not parts or member.name.startswith('/') or '..' in parts:
                raise ValueError('Unsafe archive path')
            if prefix is None:
                prefix = parts[0]
            if parts[0] != prefix:
                raise ValueError('Multiple archive roots')
            path = '/'.join(parts[1:])
            if member.isdir():
                continue
            if path not in expected or path in seen:
                raise ValueError(f'Unexpected/duplicate member: {path}')
            meta = expected[path]
            is_link = member.issym()
            if not (is_link or member.isfile()):
                raise ValueError(f'Unexpected member type: {path}')
            mode = '120000' if is_link else ('100755' if member.mode & 0o111 else '100644')
            if mode != meta['mode']:
                raise ValueError(f'Git mode mismatch: {path}')
            link = member.linkname.encode() if is_link else None
            size = len(link) if is_link else member.size
            sha1 = hashlib.sha1(f'blob {size}\0'.encode())
            sha256 = hashlib.sha256()
            hits = set()
            first = b''
            previous = b''
            consumed = 0
            source = None if is_link else tar.extractfile(member)
            dest = None
            if extract and not is_link:
                target = extract / path
                target.parent.mkdir(parents=True, exist_ok=True)
                dest = target.open('xb')
            try:
                while True:
                    chunk = link if is_link and consumed == 0 else (b'' if is_link else source.read(1024 * 1024))
                    if not chunk:
                        break
                    if not consumed:
                        first = chunk[:8192]
                    consumed += len(chunk)
                    sha1.update(chunk)
                    sha256.update(chunk)
                    sample = previous + chunk
                    for name, pattern in markers.items():
                        if pattern.search(sample):
                            hits.add(name)
                    # Windows shortcuts commonly store these strings as UTF-16LE.
                    if b'S\0I\0M\0-\0V\0I\0R\0T\0U\0A\0L\0-\0M\0A\0C\0' in sample:
                        hits.add('known-shortcut-host')
                    previous = sample[-512:]
                    if dest:
                        dest.write(chunk)
            finally:
                if source:
                    source.close()
                if dest:
                    dest.close()
                    target.chmod(0o444)
            if consumed != size or sha1.hexdigest() != meta['object']:
                raise ValueError(f'Git content mismatch: {path}')
            kind = 'symlink' if is_link else kind_of(first)
            row = dict(meta, size=size, sha256=sha256.hexdigest(), kind=kind, indicators=sorted(hits))
            if is_link:
                normalized = posixpath.normpath(posixpath.join(posixpath.dirname(path), member.linkname))
                row['target'] = member.linkname
                row['external_target'] = member.linkname.startswith('/') or normalized == '..' or normalized.startswith('../')
            if kind == 'elf' and len(first) >= 20:
                row['elf_bits'] = {1: 32, 2: 64}.get(first[4])
                order = 'little' if first[5] == 1 else 'big'
                row['elf_machine'] = int.from_bytes(first[18:20], order)
            inventory.write(json.dumps(row, ensure_ascii=False) + '\n')
            seen.add(path)
            counts[kind] += 1
            total += size
    if seen != set(expected):
        raise ValueError(f'Archive missing {len(set(expected) - seen)} Git entries')
    with archive_path.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    result = {'complete': True, 'entries': len(seen), 'bytes': total,
              'archive_sha256': digest, 'kinds': dict(counts)}
    report.joinpath('inventory-summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('git_manifest', type=Path)
    parser.add_argument('report', type=Path)
    parser.add_argument('--extract', type=Path)
    args = parser.parse_args()
    audit(args.archive, args.git_manifest, args.report, args.extract)
