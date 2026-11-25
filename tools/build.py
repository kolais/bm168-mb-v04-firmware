#!/usr/bin/env python3
"""Host fetch/preview and Linux container builds for the BM168 development kit."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
LOCK = json.loads((ROOT / 'sources.lock.json').read_text())
BOARD = json.loads((ROOT / 'board/bm168.json').read_text())
DOWNLOADS = ROOT / '.cache/downloads'


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def run(*args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def verified_source(name):
    source = LOCK[name]
    path = DOWNLOADS / source['file']
    if not path.is_file() or sha(path) != source['sha256']:
        raise RuntimeError(f'{path}: missing or checksum mismatch; run make fetch')
    return path


def download(url, path):
    """One file with curl, to PATH whole or not at all. curl and not Python's own client: one of the
    pinned sources refuses the latter (403), and every file is judged by its pinned SHA-256 anyway."""
    if not shutil.which('curl'):
        raise RuntimeError('curl is needed for the downloads')
    temp = path.with_name(path.name + '.partial')
    print(f'Downloading {url}', flush=True)
    subprocess.run(['curl', '--fail', '--silent', '--show-error', '--location', '--retry', '3',
                    '--max-time', '3600', '--output', str(temp), url], check=True)
    return temp


def fetch():
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    for name in ('lvgl', 'inter', 'toolchain', 'toolchain_glibc', 'dropbear'):
        source = LOCK[name]
        path = DOWNLOADS / source['file']
        if path.exists() and sha(path) == source['sha256']:
            print(f'{name}: verified cache')
            continue
        temp = download(source['url'], path)
        if sha(temp) != source['sha256']:
            temp.unlink()
            raise RuntimeError(f'{name}: downloaded checksum mismatch')
        temp.replace(path)


def unpack(name, dest):
    archive = verified_source(name)
    marker = dest / '.source-sha256'
    if marker.exists() and marker.read_text().strip() == LOCK[name]['sha256']:
        return
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    # Archives are authenticated against the repository's lock before extraction.
    if archive.suffix == '.zip':
        # Only the listed members, flattened (the Inter release is 33 MB of other formats).
        with zipfile.ZipFile(archive) as z:
            for member in LOCK[name]['members']:
                (dest / Path(member).name).write_bytes(z.read(member))
    else:
        run('tar', 'xf', archive, '--strip-components=1', '-C', dest)
    marker.write_text(LOCK[name]['sha256'] + '\n')


def configure(work, desktop=False, snap=False):
    unpack('lvgl', work / 'lvgl')
    unpack('inter', work / 'inter')
    args = ['cmake', '-S', ROOT, '-B', work / 'ui', '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', f'-DLVGL_SOURCE_DIR={work / "lvgl"}',
            f'-DINTER_DIR={work / "inter"}',
            '-DAUTOTOP_DESKTOP=' + ('ON' if desktop else 'OFF'),
            '-DAUTOTOP_SNAP=' + ('ON' if snap else 'OFF')]
    if not desktop and not snap:
        unpack('toolchain', work / 'toolchain')
        args += [f'-DCMAKE_TOOLCHAIN_FILE={ROOT / "cmake/arm-musl.cmake"}']
    run(*args)
    run('cmake', '--build', work / 'ui', '--parallel', str(min(os.cpu_count() or 2, 8)))
    return work / ('ui/launcher-snap' if snap else 'ui/autotop-ui')


def dropbear(work):
    """The developer shell's SSH server (recovery/plan-a/dev-access): Dropbear, static with
    the musl toolchain, one program with three names (dropbear, dropbearkey, scp). What it
    may do is fixed at build time in recovery/plan-a/dropbear-options.h: keys only, one
    key type, no forwarding."""
    unpack('dropbear', work / 'dropbear')
    unpack('toolchain', work / 'toolchain')
    build, options = work / 'dropbear-build', ROOT / 'recovery/plan-a/dropbear-options.h'
    stamp = build / '.built-from'
    want = LOCK['dropbear']['sha256'] + ' ' + sha(options)
    binary = build / 'dropbearmulti'
    if binary.is_file() and stamp.is_file() and stamp.read_text() == want:
        return binary
    if build.exists():
        shutil.rmtree(build)
    shutil.copytree(work / 'dropbear', build, symlinks=True)
    shutil.copyfile(options, build / 'localoptions.h')
    cross = work / 'toolchain/bin/arm-linux-'
    env = dict(os.environ, CC=f'{cross}gcc', AR=f'{cross}ar', RANLIB=f'{cross}ranlib', STRIP=f'{cross}strip',
               CFLAGS='-Os -ffunction-sections -fdata-sections', LDFLAGS='-Wl,--gc-sections')
    run('./configure', '--host=arm-linux', '--disable-zlib', '--disable-syslog', '--disable-lastlog',
        '--disable-utmp', '--disable-utmpx', '--disable-wtmp', '--disable-wtmpx', '--disable-pututline',
        '--disable-pututxline', '--disable-pam', '--enable-bundled-libtom', '--enable-static',
        cwd=build, env=env, stdout=subprocess.DEVNULL)
    run('make', '-j', str(min(os.cpu_count() or 2, 8)), 'PROGRAMS=dropbear dropbearkey scp', 'MULTI=1', 'STATIC=1',
        cwd=build, env=env, stdout=subprocess.DEVNULL)
    run(f'{cross}strip', binary)
    stamp.write_text(want)
    return binary


# Scenes also drawn in Full, with the rail on the right, and (the spinner) 290 ms on.
SNAP_VARIANTS = {'home': (('full',), ('rhd',)),
                 'wait-connecting': (('full',), ('rhd',), ('at=290',), ('full', 'at=290')),
                 'display': (('full',), ('rhd',)),
                 'phones': (('rhd',),),
                 'name-dial': (('full',), ('rhd',)),
                 'update': (('full',), ('rhd',)),
                 'banner': (('full',), ('at=8100',), ('full', 'at=8100'))}


def ui_test():
    """Build the launcher natively; run the navigation tests and launcherd's scenarios."""
    work = ROOT / 'build/snap'
    binary = configure(work, snap=True)
    run(binary.with_name('launcher-uitest'))
    run(binary.with_name('launcher-coretest'))
    return binary


def ui_snap():
    """Draw every scene of the launcher natively on the host: build/snap/out/*.png."""
    binary = ui_test()
    out = ROOT / 'build/snap/out'
    if out.exists():
        shutil.rmtree(out)
    out.mkdir()
    convert = shutil.which('magick')
    usage = subprocess.run([binary], capture_output=True, text=True).stderr
    scenes = usage.split('scenes:')[1].split()
    for scene in scenes:
        for variant in ((), *SNAP_VARIANTS.get(scene, ())):
            ppm = out / ('-'.join((scene, *variant)).replace('=', '') + '.ppm')
            subprocess.run([binary, scene, ppm, *variant], check=True)
            if convert:
                subprocess.run([convert, ppm, ppm.with_suffix('.png')], check=True)
                ppm.unlink()
    print(f'{len(scenes)} scenes in {out.relative_to(ROOT)}/' + ('' if convert else ' (PPM: ImageMagick not found)'))


def overlay_into(source, dest):
    """Merge without following destination symlinks outside the staged root."""
    for path in sorted(source.rglob('*')):
        relative = path.relative_to(source)
        target = dest / relative
        for parent in list(relative.parents)[:-1]:
            if (dest / parent).is_symlink():
                raise RuntimeError(f'Overlay would traverse symlink: {relative}')
        if path.is_dir() and not path.is_symlink():
            if target.is_symlink() or (target.exists() and not target.is_dir()):
                raise RuntimeError(f'Overlay directory conflicts with existing path: {relative}')
            if not target.exists():
                target.mkdir(mode=path.stat().st_mode & 0o7777)
            continue
        if not (path.is_file() or path.is_symlink()):
            raise RuntimeError(f'Unsupported overlay entry: {relative}')
        if target.is_symlink() or target.is_file():
            target.unlink()
        if path.is_symlink():
            target.symlink_to(os.readlink(path))
        else:
            shutil.copy2(path, target)
        os.chown(target, 0, 0, follow_symlinks=False)


def write_manifest(out, names, kind):
    inputs = {}
    for directory in ('ui', 'overlay', 'tools', 'board', 'cmake'):
        for path in sorted((ROOT / directory).rglob('*')):
            if '__pycache__' not in path.parts and path.is_file():
                inputs[str(path.relative_to(ROOT))] = sha(path)
    for name in ('sources.lock.json', 'CMakeLists.txt', 'Makefile'):
        inputs[name] = sha(ROOT / name)
    manifest = {
        'kind': kind, 'board': BOARD, 'sources': LOCK, 'project_inputs_sha256': inputs,
        'artifacts': {n: {'sha256': sha(out / n), 'bytes': (out / n).stat().st_size} for n in names},
        'boot_tested': False, 'vendor_update_container': False,
        'limitations': ['UBIFS journal is not replayed during extraction',
                        'Device display/input and projection integration are not validated',
                        'No bootloader, kernel, userdata or MCU image is replaced'],
        'tool_versions': subprocess.check_output(['dpkg-query', '-W', 'mtd-utils', 'cmake', 'gcc'], text=True),
        'python_packages': subprocess.check_output(['python3', '-m', 'pip', 'freeze'], text=True),
    }
    path = out / f'{kind}-manifest.json'
    temp = path.with_suffix('.partial')
    temp.write_text(json.dumps(manifest, indent=2) + '\n')
    temp.replace(path)


def apps(work, out):
    (out / 'applications-manifest.json').unlink(missing_ok=True)
    binary = configure(work)
    stage = work / 'app-overlay'
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir()
    overlay_into(ROOT / 'overlay', stage)
    install = stage / 'usr/local/bin'
    install.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, install / 'autotop-ui')
    shutil.copy2(binary, out / 'autotop-ui')
    # A deterministic tar simplifies inspecting exactly what an application update adds.
    run('tar', '--sort=name', f'--mtime=@{BOARD["source_date_epoch"]}',
        '--owner=0', '--group=0', '--numeric-owner', '-cf', out / 'applications.tar', '-C', stage, '.')
    write_manifest(out, ['autotop-ui', 'applications.tar'], 'applications')
    return stage


def stock_image():
    """The builder's own capture of the stock rootfs partition (docs/stock-capture.md), and whether it
    is the very capture this work was done on (then its files need no further check)."""
    image = ROOT / BOARD['rootfs_source']
    if not image.is_file():
        raise RuntimeError(f"no stock rootfs capture at {BOARD['rootfs_source']}: it comes from the builder's "
                           'own unit, see docs/stock-capture.md')
    if image.stat().st_size != BOARD['rootfs_size']:
        raise RuntimeError(f"{BOARD['rootfs_source']} is not a whole rootfs partition ({BOARD['rootfs_size']} bytes)")
    return image, sha(image) == BOARD['rootfs_sha256']


def stock_verify(root, reference):
    """Another unit's capture is judged file by file (tools/stock_manifest.py)."""
    if reference:
        return
    import stock_manifest
    fixed, changing = stock_manifest.check(root, ROOT / 'board/stock-files.sha256')
    print(f'stock files: {len(fixed)} fixed files differ, {len(changing)} in directories a running unit writes to')
    for line in fixed[:40]:
        print('  ' + line)
    if fixed:
        raise RuntimeError(f"this capture is not the stock version the build is made for ({BOARD['baseline']})")


def firmware(work, out):
    (out / 'firmware-manifest.json').unlink(missing_ok=True)
    image, reference = stock_image()
    app_overlay = apps(work, out)
    extracted = work / 'extracted'
    if extracted.exists():
        shutil.rmtree(extracted)
    log = out / 'extraction.log'
    with log.open('w') as f:
        run('ubireader_extract_files', '-k', '-x', '-o', extracted, image, stdout=f, stderr=subprocess.STDOUT)
    if re.search(r'\b(?:Warn|Error|Fatal)\b', log.read_text(), re.IGNORECASE):
        raise RuntimeError('Extraction reported a warning/error; inspect dist/extraction.log')
    roots = list(extracted.glob('*/rootfs'))
    if len(roots) != 1:
        raise RuntimeError('Expected exactly one rootfs volume')
    stock_verify(roots[0], reference)
    root = roots[0]
    for name in ('etc/inittab', 'etc/profile', 'usr/bin/Launcher', 'usr/bin/carplay'):
        if not (root / name).is_file():
            raise RuntimeError(f'Baseline missing required file: {name}')
    overlay_into(app_overlay, root)
    ubifs = out / 'rootfs.ubifs.partial'
    run('mkfs.ubifs', '-r', root, '-m', BOARD['min_io_size'], '-e', BOARD['leb_size'],
        '-c', BOARD['max_leb_count'], '-x', 'lzo', '-F', '-o', ubifs)
    config = work / 'ubinize.ini'
    config.write_text('[rootfs]\nmode=ubi\nimage=/out/rootfs.ubifs.partial\nvol_type=dynamic\n'
                      f'vol_id={BOARD["volume_id"]}\nvol_name={BOARD["volume_name"]}\nvol_flags=autoresize\n')
    ubi = out / 'rootfs.ubi.partial'
    run('ubinize', '-o', ubi, '-p', BOARD['peb_size'], '-m', BOARD['min_io_size'],
        '-s', BOARD['min_io_size'], '-O', BOARD['vid_header_offset'],
        '-Q', BOARD['ubi_image_sequence'], config)
    # Leave 24 PEBs for UBI overhead/reserve; actual device bad-block allowance must still be checked.
    if ubi.stat().st_size > BOARD['rootfs_size'] - 24 * BOARD['peb_size']:
        raise RuntimeError('Generated UBI image exceeds conservative partition budget')
    ubifs.replace(out / 'rootfs.ubifs')
    ubi.replace(out / 'rootfs.ubi')
    write_manifest(out, ['rootfs.ubifs', 'rootfs.ubi', 'autotop-ui', 'applications.tar'], 'firmware')
    print('Built dist/rootfs.ubi and dist/rootfs.ubifs. These are development images, not a vendor update pack.')


def require_sdk():
    sdk = LOCK['arkmicro_sdk']
    if not sdk.get('enabled', False):
        raise RuntimeError('SDK use disabled: ' + sdk['status'])
    manifest = ROOT / sdk['sanitization_manifest']
    if sha(manifest) != sdk['sanitization_manifest_sha256']:
        raise RuntimeError('SDK sanitisation policy checksum mismatch')
    policy = json.loads(manifest.read_text())
    if (policy['upstream_commit'] != sdk['commit'] or
            policy['sanitized_commit'] != sdk['sanitized_commit'] or
            policy['sanitized_tree'] != sdk['sanitized_tree'] or
            policy['snapshot']['tree'] != sdk['sanitized_tree']):
        raise RuntimeError('SDK source identities differ from the lock')
    return policy


def verified_sdk():
    policy = require_sdk()
    snapshot = policy['snapshot']
    path = ROOT / snapshot['path']
    if (not path.is_file() or path.stat().st_size != snapshot['size'] or
            sha(path) != snapshot['sha256']):
        raise RuntimeError('Sanitised SDK archive missing or changed; run make sdk-fetch')
    return policy, path


def sdk_fetch():
    policy = require_sdk()
    dest = ROOT / policy['snapshot']['path']
    if dest.exists():
        verified_sdk()
        print(f'Sanitised SDK verified: {policy["sanitized_commit"]}')
        return
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    archive = DOWNLOADS / f'arkmicro-{policy["upstream_commit"]}.tar.gz'
    previous = ROOT / 'build/sdk-audit/vendor-baseline.tar.gz'
    if not archive.exists() and previous.is_file():
        archive = previous
    if not archive.exists():
        temp = download(policy['archive_url'], archive)
        if sha(temp) != policy['archive_sha256']:
            temp.unlink()
            raise RuntimeError('Downloaded SDK archive checksum mismatch')
        temp.replace(archive)
    if sha(archive) != policy['archive_sha256']:
        raise RuntimeError('SDK baseline archive checksum mismatch')
    # Never check out the contaminated baseline or use its working-tree files.
    repo = ROOT / '.cache/sdk-review.git'
    if not repo.exists():
        run('git', '-c', 'core.hooksPath=/dev/null', 'init', '--bare', repo)
        run('git', '-C', repo, 'remote', 'add', 'origin', policy['upstream_url'])
    if subprocess.run(['git', '-C', str(repo), 'cat-file', '-e',
                       policy['upstream_commit'] + '^{commit}'],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        run('git', '-c', 'core.hooksPath=/dev/null', '-C', repo,
            'fetch', '--filter=blob:none', '--depth=1', 'origin', policy['upstream_commit'])
        run('git', '-C', repo, 'update-ref', 'refs/heads/vendor-base', policy['upstream_commit'])
    run(sys.executable, ROOT / 'tools/sanitize_sdk.py', archive, repo, dest)
    verified_sdk()


def verified_wifi():
    """Return the pinned RTL8821CS driver archive after checking it against the lock."""
    source = LOCK['rtl8821cs']
    path = ROOT / source['archive']
    if not path.is_file() or path.stat().st_size != source['size'] or sha(path) != source['sha256']:
        raise RuntimeError('RTL8821CS archive missing or changed; run make wifi-fetch')
    return source, path


def wifi_fetch():
    """Reproduce the pinned driver archive with a sparse, partial clone of one directory."""
    source = LOCK['rtl8821cs']
    dest = ROOT / source['archive']
    if dest.exists():
        verified_wifi()
        print(f'RTL8821CS archive verified: {source["commit"]}')
        return
    repo = ROOT / '.cache/rtl8821cs.git'
    if not repo.exists():
        run('git', '-c', 'core.hooksPath=/dev/null', 'init', '--bare', repo)
        run('git', '-C', repo, 'remote', 'add', 'origin', source['url'])
    run('git', '-c', 'core.hooksPath=/dev/null', '-C', repo, 'fetch', '--filter=blob:none',
        '--depth=1', 'origin', source['commit'])
    if subprocess.check_output(['git', '-C', str(repo), 'rev-parse',
                                f'{source["commit"]}:{source["path"]}'], text=True).strip() != source['tree']:
        raise RuntimeError('RTL8821CS source tree differs from the lock')
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open('wb') as out:
        archive = subprocess.Popen(['git', '-C', str(repo), 'archive', '--format=tar',
                                    source['commit'], source['path']], stdout=subprocess.PIPE)
        subprocess.run(['gzip', '-n', '-9'], stdin=archive.stdout, stdout=out, check=True)
        if archive.wait():
            raise RuntimeError('git archive failed')
    verified_wifi()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['fetch', 'apps', 'firmware', 'preview', 'ui-snap', 'ui-test', 'sdk-fetch', 'wifi-fetch'])
    args = parser.parse_args()
    if args.action == 'fetch':
        fetch()
    elif args.action == 'preview':
        binary = configure(ROOT / 'build/desktop', desktop=True)
        run(binary)
    elif args.action == 'ui-snap':
        ui_snap()
    elif args.action == 'ui-test':
        ui_test()
    elif args.action == 'sdk-fetch':
        sdk_fetch()
    elif args.action == 'wifi-fetch':
        wifi_fetch()
    else:
        if ROOT != Path('/project') or not Path('/out').is_dir():
            raise RuntimeError('Run this build through make apps / make firmware in the Linux container')
        os.environ['SOURCE_DATE_EPOCH'] = str(BOARD['source_date_epoch'])
        work, out = Path('/work'), Path('/out')
        if args.action == 'apps':
            apps(work, out)
        else:
            firmware(work, out)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
