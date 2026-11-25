#!/usr/bin/env python3
"""Reproduce the audited BSP tree and archive without checking out vendor files.

Requires a partial clone containing the pinned baseline's commits/trees, and
the downloaded baseline archive. Only reviewed text blobs enter the Git store.
No vendor command is executed and no branch or working tree is changed.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def sanitize(archive, repo, output):
    policy = json.loads((ROOT / 'board/sdk-sanitization.json').read_text())
    if digest(archive) != policy['archive_sha256']:
        raise ValueError('Baseline archive checksum mismatch')
    exclusions = ROOT / 'board/sdk-exclusions.txt'
    if digest(exclusions) != policy['exclusions_sha256']:
        raise ValueError('Exclusion list checksum mismatch')
    excluded = set(exclusions.read_text().splitlines())
    patches = [ROOT / 'board/sdk-patches' / p for p in policy['patches']]
    text_paths = set(policy['wifi_templates'])
    for patch in patches:
        if digest(patch) != policy['patches'][patch.name]:
            raise ValueError(f'Patch checksum mismatch: {patch.name}')
        text_paths.update(line[6:] for line in patch.read_text().splitlines()
                          if line.startswith('--- a/'))
    environment = dict(os.environ, GIT_NO_LAZY_FETCH='1')

    def git(*args, **kwargs):
        return subprocess.check_output(['git', '-c', 'core.hooksPath=/dev/null',
                                        '-C', str(repo), *args], env=environment, **kwargs)

    def tree(ref):
        result = {}
        for entry in git('ls-tree', '-rz', ref).split(b'\0'):
            if entry:
                info, path = entry.split(b'\t', 1)
                mode, kind, oid = info.decode().split()
                if kind != 'blob':
                    raise ValueError('Unexpected non-blob source entry')
                result[path.decode()] = (mode, oid)
        return result

    def members(tar):
        prefix = None
        for member in tar:
            parts = PurePosixPath(member.name).parts
            if not parts or member.name.startswith('/') or '..' in parts:
                raise ValueError('Unsafe archive member')
            prefix = prefix or parts[0]
            if parts[0] != prefix:
                raise ValueError('Multiple archive roots')
            if not member.isdir():
                yield '/'.join(parts[1:]), member

    baseline = tree(policy['upstream_commit'])
    texts = {}
    with tarfile.open(archive, 'r|gz') as source:
        for path, member in members(source):
            if path in text_paths:
                if not member.isfile() or path in texts:
                    raise ValueError('Invalid patch input')
                texts[path] = source.extractfile(member).read()
                oid = git('hash-object', '-w', '--stdin', input=texts[path]).decode().strip()
                if oid != baseline[path][1]:
                    raise ValueError(f'Text blob mismatch: {path}')
    if set(texts) != text_paths:
        raise ValueError('Missing patch inputs')
    with tempfile.TemporaryDirectory(prefix='bm168-sdk-index-') as temp:
        environment['GIT_INDEX_FILE'] = str(Path(temp) / 'index')
        # A fresh fetch (tools/build.py sdk_fetch) is a bare repository, and git's
        # index commands want a work tree even when they touch only the index:
        # an empty one of our own. Nothing is ever checked out into it.
        (Path(temp) / 'empty').mkdir()
        environment['GIT_WORK_TREE'] = str(Path(temp) / 'empty')
        environment['GIT_DIR'] = subprocess.check_output(
            ['git', '-C', str(repo), 'rev-parse', '--absolute-git-dir'], text=True).strip()
        git('read-tree', policy['upstream_commit'])
        git('update-index', '--force-remove', '-z', '--stdin',
            input=b'\0'.join(p.encode() for p in sorted(excluded)) + b'\0')
        for patch in patches:
            git('apply', '--cached', str(patch))
        for path in policy['wifi_templates']:
            data, count = re.subn(rb'(?m)^(\s*(?:wpa_passphrase|wpa_psk)\s*=)[^\r\n]*',
                                  rb'\1', texts[path])
            if not count:
                raise ValueError(f'Missing credential directive: {path}')
            data = b'# Shared credential removed: provision a per-device WPA credential before use.\n' + data
            oid = git('hash-object', '-w', '--stdin', input=data).decode().strip()
            git('update-index', '--add', '--cacheinfo', baseline[path][0], oid, path)
        # Unchanged blobs stay in the verified archive, not this partial clone.
        # Every exported blob is checked against this tree below.
        actual = git('write-tree', '--missing-ok').decode().strip()
    del environment['GIT_WORK_TREE']    # it is gone with the temporary directory; the rest reads objects only
    if actual != policy['sanitized_tree']:
        raise ValueError('Sanitized tree differs from reviewed policy')
    selected = tree(actual)
    overrides = {p: git('cat-file', 'blob', oid) for p, (mode, oid) in selected.items()
                 if baseline.get(p) != (mode, oid)}
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_suffix(output.suffix + '.partial')
    seen = set()
    with pending.open('wb') as dest, gzip.GzipFile(filename='', mode='wb', fileobj=dest,
                                                mtime=0, compresslevel=6) as compressed:
        with tarfile.open(fileobj=compressed, mode='w|', format=tarfile.PAX_FORMAT) as clean:
            with tarfile.open(archive, 'r|gz') as source:
                for path, member in members(source):
                    if path in excluded:
                        continue
                    if path not in selected or path in seen:
                        raise ValueError(f'Unexpected retained entry: {path}')
                    mode, oid = selected[path]
                    info = tarfile.TarInfo('bm168-bsp/' + path)
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ''
                    if mode == '120000':
                        if not member.issym():
                            raise ValueError('Symlink type mismatch')
                        data = member.linkname.encode()
                        info.type = tarfile.SYMTYPE
                        info.mode = 0o777
                        info.linkname = member.linkname
                        clean.addfile(info)
                    else:
                        if not member.isfile():
                            raise ValueError('Regular-file type mismatch')
                        data = overrides[path] if path in overrides else source.extractfile(member).read()
                        info.size = len(data)
                        info.mode = 0o755 if mode == '100755' else 0o644
                        clean.addfile(info, io.BytesIO(data))
                    computed = hashlib.sha1(f'blob {len(data)}\0'.encode() + data).hexdigest()
                    if computed != oid:
                        raise ValueError(f'Exported blob mismatch: {path}')
                    seen.add(path)
    if seen != set(selected):
        raise ValueError('Incomplete sanitized archive')
    pending.replace(output)
    result = {'tree': actual, 'entries': len(seen), 'sha256': digest(output),
              'size': output.stat().st_size}
    output.with_suffix(output.suffix + '.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('repository', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    sanitize(args.archive.resolve(), args.repository.resolve(), args.output.resolve())
