#!/usr/bin/env python3
"""Build RAM-only recovery probes; none of them can erase or program NAND."""
import json
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zlib

from build import ROOT, BOARD, LOCK, configure, dropbear, run, sha, stock_image, stock_verify, unpack, verified_sdk
import restore_manifest


def check_applets(runtime, init):
    """Every BusyBox applet the init script calls must exist in the stock binary."""
    listed = subprocess.check_output(['qemu-arm', '-L', str(runtime),
                                      str(runtime / 'bin/busybox'), '--list'], text=True).split()
    used = set(re.findall(r'/bin/busybox (\w+)', init.read_text()))
    missing = sorted(used - set(listed))
    if missing:
        raise RuntimeError(f'{init.name} uses BusyBox applets missing from stock: {missing}')


def build_capture(cross, out, work, empty):
    """Compile nand-capture and exercise it against the tracked syscall fixture."""
    source, fixture = ROOT / 'recovery/nand-capture.c', ROOT / 'recovery/tests/nand-capture-fixture.c'
    flags = ['-std=c99', '-D_GNU_SOURCE', '-Os', '-static', '-Wall', '-Wextra', '-Werror']
    binary = out / 'nand-capture'
    run(cross + 'gcc', *flags, '-o', binary, source)
    rejected = subprocess.run(['qemu-arm', str(binary), str(empty), '/mnt/usb/CAPTURE/t/x'],
                              capture_output=True, text=True)
    if rejected.returncode != 1 or 'read-only /dev/mtdNro' not in rejected.stderr:
        raise RuntimeError('nand-capture accepted a non-MTD input')
    root = work / 'capture-fixture'
    if root.exists():
        shutil.rmtree(root)
    (root / 'run').mkdir(parents=True)
    test = work / 'nand-capture-fixture'
    run(cross + 'gcc', *flags, f'-DCAPTURE_ROOT="{root}/"', '-o', test, source, fixture,
        '-Wl,--wrap=open,--wrap=pread,--wrap=ioctl,--wrap=close')

    def case(prefix, fault=None, device='/dev/mtd99ro'):
        env = dict(os.environ, FAULT=fault or '')
        return subprocess.run(['qemu-arm', str(test), device, str(prefix)],
                              capture_output=True, text=True, env=env)

    good = case(root / 'run/mtd99')
    if good.returncode or not good.stdout.startswith('PASS') or 'bad=1 corrected_pages=1' not in good.stdout:
        raise RuntimeError(f'nand-capture fixture failed: {good.stdout}{good.stderr}')
    raw = (root / 'run/mtd99.raw').read_bytes()
    ecc = (root / 'run/mtd99.ecc').read_bytes()
    if len(raw) != 128 * 2112 or len(ecc) != 128 * 2048:
        raise RuntimeError('nand-capture fixture output sizes are wrong')
    for page in range(128):
        main = bytes((page + i) & 0xff for i in range(2048))
        oob = b'\xff' * 3 + bytes([page]) + b'\xff' * 60
        expect_raw = bytearray(main)
        if page == 2:
            expect_raw[7] ^= 0x10
        if raw[page * 2112:(page + 1) * 2112] != bytes(expect_raw) + oob or \
                ecc[page * 2048:(page + 1) * 2048] != main:
            raise RuntimeError(f'nand-capture fixture content differs at page {page}')
    log = (root / 'run/mtd99.log').read_text()
    for line in ('BLOCK offset=0x00020000 bad=1', 'ECC_CORRECTED offset=0x00001000 bits=1',
                 'END blocks=2 bad=1 corrected_pages=1 failed_pages=0'):
        if line not in log:
            raise RuntimeError(f'nand-capture fixture log lacks {line!r}')
    if case(root / 'run/mtd99').returncode != 1:
        raise RuntimeError('nand-capture overwrote an existing capture')
    unc = case(root / 'run/unc', 'uncorrectable')
    if unc.returncode != 2 or 'ECC_FAILED offset=0x00002000 count=1' not in (root / 'run/unc.log').read_text():
        raise RuntimeError('nand-capture did not report an uncorrectable page')
    (root / 'update').mkdir()  # so the vendor-name case can only fail on its name check
    reasons = {'geometry': 'unsupported geometry', 'writable': 'unsupported geometry',
               'bad-ioctl': 'MEMGETBADBLOCK', 'short-oob': 'short OOB read',
               'short-data': 'short raw read', 'read-error': 'raw read'}
    rejected_cases = {f'fault-{f}': (root / f'run/{f}', f, '/dev/mtd99ro', why)
                      for f, why in reasons.items()}
    rejected_cases.update({
        'vendor-dir': (root / 'update/x', None, '/dev/mtd99ro', 'vendor update input'),
        'vendor-file': (root / 'run/ZIMAGE', None, '/dev/mtd99ro', 'vendor update input'),
        'outside-root': (work / 'elsewhere', None, '/dev/mtd99ro', 'output prefix must be below'),
        'dotdot': (root / 'run/../x', None, '/dev/mtd99ro', 'output prefix must be below'),
        'writable-node': (root / 'run/node', None, '/dev/mtd99', 'read-only /dev/mtdNro'),
    })
    for name, (prefix, fault, device, why) in rejected_cases.items():
        result = case(prefix, fault, device)
        if result.returncode != 1 or why not in result.stderr:
            raise RuntimeError(f'nand-capture fixture case {name} was not rejected as expected: '
                               f'{result.returncode} {result.stderr!r}')
    print(f'nand-capture fixture: content, overwrite, uncorrectable and {len(rejected_cases)} rejection cases passed')
    return binary


def check_writer_dtb(dtc, dtb, window):
    """The built DTB must carry exactly the requested window and one writable partition."""
    text = subprocess.check_output([str(dtc), '-I', 'dtb', '-O', 'dts', str(dtb)],
                                   stderr=subprocess.DEVNULL, text=True)
    cells = f'<{window[0]:#x} {window[1]:#x}>'
    if text.count('autotop,write-window') != 1 or f'autotop,write-window = {cells};' not in text:
        raise RuntimeError('writer DTB window property is missing or differs')
    nodes = re.findall(r'\n\t*([\w@,.-]+) \{([^{}]*)\}', text)
    parts = [(n, body) for n, body in nodes if 'label = ' in body and 'reg = ' in body]
    writable = [(n, body) for n, body in parts if 'read-only' not in body]
    if len(parts) != 15 or len(writable) != 1 or 'label = "autotop-write-window"' not in writable[0][1] \
            or f'reg = {cells};' not in writable[0][1]:
        raise RuntimeError(f'writer DTB must have 14 read-only partitions plus one window, got {len(parts)}')


STATE_WINDOW = (0x7260000, 0x600000)  # the stock userdata partition


def check_state_dtb(dtc, dtb, state):
    """Plan A DTBs: every NAND partition read-only; with the state window, userdata alone is not."""
    text = subprocess.check_output([str(dtc), '-I', 'dtb', '-O', 'dts', str(dtb)],
                                   stderr=subprocess.DEVNULL, text=True)
    parts = re.findall(r'partition@([0-9a-f]+) \{(.*?)\};', text, re.S)
    writable = [(a, body) for a, body in parts if 'read-only;' not in body]
    cells = f'<{STATE_WINDOW[0]:#x} {STATE_WINDOW[1]:#x}>'
    if len(parts) != 14 or 'autotop,write-window' in text:
        raise RuntimeError('Plan A DTB: 14 NAND partitions and no installer window expected')
    if not state:
        if writable or 'autotop,state-window' in text:
            raise RuntimeError('Plan A DTB: every NAND partition must be read-only')
        return
    if text.count('autotop,state-window') != 1 or f'autotop,state-window = {cells};' not in text:
        raise RuntimeError('Plan A state DTB: the state window is missing or differs')
    if len(writable) != 1 or writable[0][0] != f'{STATE_WINDOW[0]:x}' or 'label = "userdata"' not in writable[0][1] \
            or f'reg = {cells};' not in writable[0][1]:
        raise RuntimeError('Plan A state DTB: userdata, and only userdata, may lack read-only')


def build_writer(cross, out, work, bootlogo=False):
    """Compile nand-restore, check the shared format code on ARM, and run the fixture suite.
    bootlogo: for the writer whose window is the boot picture's partition, which the
    shared rule admits only when built so (recovery/bm168_nand_format.h)."""
    import hashlib
    sys.path.insert(0, str(ROOT / 'tools'))
    import nand_ecc
    flags = ['-std=c99', '-D_GNU_SOURCE', '-Os', '-static', '-Wall', '-Wextra', '-Werror',
             '-I', str(ROOT / 'recovery'), *(['-DBM168_BOOTLOGO_WINDOW'] if bootlogo else [])]
    binary = out / 'nand-restore'
    run(cross + 'gcc', *flags, '-o', binary, ROOT / 'recovery/nand-restore.c')
    root = work / 'writer-fixture'
    if root.exists():
        shutil.rmtree(root)
    root.mkdir()

    # Shared format code, compiled for ARM, against the Python model and window rules.
    check = work / 'nand-format-check'
    run(cross + 'gcc', *flags, '-o', check, ROOT / 'recovery/tests/nand-format-check.c')
    if subprocess.run(['qemu-arm', str(check), 'window'], capture_output=True).returncode:
        raise RuntimeError('ARM window rule check failed')
    vectors = root / 'vectors.bin'
    rng = __import__('random').Random(1676505600)
    vectors.write_bytes(b'\xff' * 2048 + bytes(2048) + bytes(rng.getrandbits(8) for _ in range(2048 * 62)))
    c_out = subprocess.check_output(['qemu-arm', str(check), 'encode', str(vectors)], text=True)
    py_out = ''.join(f'{n * 2048:08x} {nand_ecc.page_oob(vectors.read_bytes()[n * 2048:(n + 1) * 2048]).hex()}\n'
                     for n in range(64))
    if c_out != py_out:
        raise RuntimeError('ARM stock-format encoder differs from tools/nand_ecc.py')

    # nand-restore against the simulated writer kernel.
    sysfs = '/fixture/autotop_writer'
    test = work / 'nand-restore-fixture'
    run(cross + 'gcc', *flags, f'-DWRITER_SYSFS="{sysfs}"', '-o', test,
        ROOT / 'recovery/nand-restore.c', ROOT / 'recovery/tests/nand-restore-fixture.c',
        '-Wl,--wrap=open,--wrap=close,--wrap=pread,--wrap=read,--wrap=write,--wrap=ioctl')
    pages = []
    for n in range(128):
        if n % 5 == 4:
            pages.append(b'\xff' * 2048)
        else:
            pages.append(bytes((n * 7 + i * 3 + (i >> 7)) & 0xff for i in range(2048)))
    source = root / 'source.bin'
    source.write_bytes(b''.join(pages))
    image, man = restore_manifest.build(source, 0x160000, 0x160000, 0x40000, root / 'good')
    expected = b''.join(p + (b'\xff' * 64 if p == b'\xff' * 2048 else nand_ecc.page_oob(p)) for p in pages)
    programs = sum(p != b'\xff' * 2048 for p in pages)

    def variant(name, edit):
        d = root / name
        d.mkdir()
        img, mf = d / 'RESTORE.IMG', d / 'RESTORE.MAN'
        img.write_bytes(image.read_bytes())
        mf.write_text(man.read_text())
        edit(img, mf)
        return img, mf

    bad_hash = variant('bad-hash', lambda i, m: m.write_text(m.read_text().replace(
        'block 0 ' + hashlib.sha256(pages_bytes(pages, 0)).hexdigest(), 'block 0 ' + '0' * 64)))
    short = variant('short', lambda i, m: i.write_bytes(i.read_bytes()[:-2048]))
    other_window = variant('other-window', lambda i, m: m.write_text(
        m.read_text().replace('window 0x160000 0x40000', 'window 0x180000 0x40000')))
    arkdata = variant('arkdata', lambda i, m: m.write_text(
        m.read_text().replace('window 0x160000 0x40000', 'window 0x120000 0x40000')))

    # The fixture adds one corrected bit per verify read after the first erase,
    # so a tolerance of 1 passes and a tolerance of 0 (above) stops.
    tolerant = variant('tolerant', lambda i, m: m.write_text(m.read_text() + 'max_fresh_corrections 1\n'))
    tolerant_low = variant('tolerant-zero', lambda i, m: m.write_text(m.read_text() + 'max_fresh_corrections 0\n'))
    too_high = variant('tolerant-8', lambda i, m: m.write_text(m.read_text() + 'max_fresh_corrections 8\n'))
    resume = variant('resume', lambda i, m: m.write_text(m.read_text() + 'resume_from_block 1\n'))
    resume_bad = variant('resume-2', lambda i, m: m.write_text(m.read_text() + 'resume_from_block 2\n'))

    def case(mode, fault=None, files=(image, man), device='/dev/mtd14'):
        dump = root / f'dump-{mode}-{fault}.bin'
        env = dict(os.environ, FAULT=fault or '', FIXTURE_DUMP=str(dump))
        r = subprocess.run(['qemu-arm', str(test), mode, device, str(files[0]), str(files[1])],
                           capture_output=True, text=True, env=env)
        counters = dict(re.findall(r'(\w+)=(\d+)', r.stderr.split('FIXTURE', 1)[-1]))
        return r, {k: int(v) for k, v in counters.items()}, dump.read_bytes() if dump.exists() else b''

    plan, c, initial = case('plan')
    if plan.returncode or 'PLAN_OK blocks=2' not in plan.stdout or c['erases'] or c['programs'] or c['disarms']:
        raise RuntimeError(f'nand-restore plan case failed: {plan.stdout}{plan.stderr}')
    # The image inside a larger file (the rescue container): IMAGE@OFFSET,
    # mapped; the same plan and the same flash as from the plain file.
    container = root / 'container.bin'
    container.write_bytes(bytes(range(256)) * 16 + image.read_bytes() + b'tail')
    inside = (f'{container}@4096', man)
    plan_in, c, _ = case('plan', None, inside)
    if plan_in.returncode or plan_in.stdout != plan.stdout or c['erases'] or c['programs']:
        raise RuntimeError(f'nand-restore plan from inside a container failed: {plan_in.stdout}{plan_in.stderr}')
    good_in, c, final = case('execute', None, inside)
    if good_in.returncode or 'RESULT PASS blocks=2' not in good_in.stdout or final != expected:
        raise RuntimeError(f'nand-restore execute from inside a container failed: {c} {good_in.stdout[-400:]}')
    odd, c, _ = case('plan', None, (f'{container}@4000', man))
    if not odd.returncode or 'multiple of 4096' not in odd.stdout + odd.stderr:
        raise RuntimeError('nand-restore took an image offset that is not page-aligned')
    good, c, final = case('execute')
    if good.returncode or 'RESULT PASS blocks=2' not in good.stdout or c != {
            'erases': 2, 'programs': programs, 'disarms': 1, 'armed': 0} or final != expected:
        raise RuntimeError(f'nand-restore execute case failed: {c} {good.stdout[-500:]}{good.stderr[-300:]}')
    tol, c, final = case('execute', 'ecc-corrected', tolerant)
    if tol.returncode or 'RESULT PASS blocks=2' not in tol.stdout or tol.stdout.count('TOLERATED') != 2 \
            or 'max_fresh_corrections=1' not in tol.stdout or final != expected or c['armed'] != 0:
        raise RuntimeError(f'nand-restore tolerance case failed: {c} {tol.stdout[-400:]}')
    # One raw bit flip in a fresh page passes with tolerance 1 (corrected hash still exact).
    rawtol, c, final = case('execute', 'bitflip', tolerant)
    if rawtol.returncode or 'RESULT PASS blocks=2' not in rawtol.stdout or \
            'TOLERATED_RAW block=0 page=2 bits=1' not in rawtol.stdout or final != expected:
        raise RuntimeError(f'nand-restore raw tolerance case failed: {c} {rawtol.stdout[-400:]}')
    # Resume writes only block 1; block 0 keeps its previous contents.
    res, c, final = case('execute', None, resume)
    second = sum(p != b'\xff' * 2048 for p in pages[64:])
    if res.returncode or 'written_from=1' not in res.stdout or c['erases'] != 1 or \
            c['programs'] != second or final[:64 * 2112] != initial[:64 * 2112] or \
            final[64 * 2112:] != expected[64 * 2112:]:
        raise RuntimeError(f'nand-restore resume case failed: {c} {res.stdout[-400:]}')
    # (mode, fault, files, device, expected message, erases, programs)
    stops = [
        ('execute', 'disarmed', None, None, 'writer is not armed', 0, 0),
        ('execute', 'window-mismatch', None, None, 'differs from manifest', 0, 0),
        ('execute', 'wrong-name', None, None, 'not the autotop-write-window', 0, 0),
        ('execute', 'bad-block', None, None, 'bad-block marker', 0, 0),
        ('execute', 'marker', None, None, 'bad-block marker', 0, 0),
        ('execute', None, bad_hash, None, 'image block 0 hash differs', 0, 0),
        ('execute', None, short, None, 'image size differs', 0, 0),
        ('execute', None, other_window, None, 'differs from manifest', 0, 0),
        ('execute', None, arkdata, None, 'manifest incomplete or inconsistent', 0, 0),
        ('execute', None, too_high, None, 'manifest line not understood', 0, 0),
        ('execute', None, resume_bad, None, 'manifest incomplete or inconsistent', 0, 0),
        ('execute', None, None, '/dev/mtd14ro', 'must be a /dev/mtdN node', 0, 0),
        ('execute', 'erase-fail', None, None, 'erase failed at block 0', 0, 0),
        ('execute', 'erase-incomplete', None, None, 'not erased after erase', 1, 0),
        ('execute', 'program-fail', None, None, 'program failed at block 0', 1, 2),
        ('execute', 'bitflip', None, None, 'raw readback differs', 1, None),
        ('execute', 'ecc-corrected', None, None, 'needed ECC corrections', 1, None),
        ('execute', 'ecc-corrected', tolerant_low, None, 'needed ECC corrections', 1, None),
        ('plan', 'bad-block', None, None, 'bad-block marker', 0, 0),
    ]
    first_block = sum(p != b'\xff' * 2048 for p in pages[:64])
    for mode, fault, files, device, message, erases, progs in stops:
        r, c, _ = case(mode, fault, files or (image, man), device or '/dev/mtd14')
        progs = first_block if progs is None else progs
        if r.returncode != 1 or f'STOP' not in r.stdout or message not in r.stdout or \
                c.get('erases') != erases or c.get('programs') != progs or \
                (mode == 'execute' and fault != 'disarmed' and c.get('armed') != 0):
            raise RuntimeError(f'nand-restore stop case {mode}/{fault}/{files} failed: {c} '
                               f'{r.stdout[-300:]}{r.stderr[-300:]}')
    print(f'nand-restore fixture: plan, full execute ({programs} pages, exact flash image), '
          f'tolerated corrections and raw flips, resume '
          f'and {len(stops)} stop cases passed; ARM format code matches tools/nand_ecc.py')
    return binary


def pages_bytes(pages, block):
    return b''.join(pages[block * 64:(block + 1) * 64])


def stock_closure(root, names, cross, have):
    """Stock files plus every shared library they need, resolved inside the stock root."""
    wanted, queue = [], list(names)
    search = ('lib', 'usr/lib')
    while queue:
        name = queue.pop()
        if name in wanted or name in have:
            continue
        path = (root / name).resolve()
        if not path.is_relative_to(root) or not path.is_file():
            raise RuntimeError(f'Missing stock file: {name}')
        wanted.append(name)
        dynamic = subprocess.check_output([cross + 'readelf', '-d', str(path)], text=True)
        for lib in re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', dynamic):
            hits = [f'{d}/{lib}' for d in search if (root / d / lib).exists()]
            if not hits:
                raise RuntimeError(f'{name} needs {lib}, absent from the stock root')
            queue.append(hits[0])
    return wanted


def build_wifi_module(cross, make, kernel, work, out):
    """Build the pinned, patched RTL8821CS driver against this exact kernel build."""
    from build import verified_wifi
    source, archive = verified_wifi()
    tree = work / 'rtl8821cs'
    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir()
    run('tar', 'xf', archive, '--no-same-owner', '-C', tree,
        f'--strip-components={source["path"].count("/") + 1}')
    for patch in sorted((ROOT / 'recovery/wifi/patches').glob('*.patch')):
        run('patch', '-p1', '--no-backup-if-mismatch', '-i', patch, cwd=tree)
    if subprocess.run(['grep', '-rlE', 'rockchip_wifi|rfkill-wlan', '--include=*.c', '--include=*.h', str(tree)],
                      capture_output=True).stdout:
        raise RuntimeError('Rockchip platform hooks remain in the patched driver')
    run(*make, f'M={tree}', 'CONFIG_RTL8821CS=m', '-j', str(min(os.cpu_count() or 2, 8)), 'modules')
    module = out / '8821cs.ko'
    run(cross + 'strip', '--strip-debug', '-o', module, tree / '8821cs.ko')
    release = (kernel / 'include/config/kernel.release').read_text().strip()
    modinfo = work / '8821cs.modinfo'
    run(cross + 'objcopy', '-O', 'binary', '--only-section=.modinfo', module, modinfo)
    info = modinfo.read_bytes()
    fields = dict(f.split('=', 1) for f in info.decode(errors='replace').split('\0') if '=' in f)
    if not fields.get('vermagic', '').startswith(release + ' '):
        raise RuntimeError(f'module vermagic {fields.get("vermagic")!r} does not match kernel {release}')
    if 'sdio:c*v024CdC821*' not in info.decode(errors='replace'):
        raise RuntimeError('module lacks the RTL8821CS SDIO alias')
    print(f'Built {module.name}: {module.stat().st_size} bytes, vermagic {fields["vermagic"]}')
    return module


GALCORE_MEMBER = 'bm168-bsp/buildroot-external/package/libgal/software/lib/galcore.ko'
GALCORE_SHA256 = '647302b26991cd4a6a624bb25ce016c77f0e57475a9bab6cfba4543ad72be710'


# Options that change core kernel structure layouts seen by binary modules.
SPIKE_ABI_KEYS = (
    'SMP', 'PREEMPT_NONE', 'PREEMPT_VOLUNTARY', 'PREEMPT', 'GENERIC_BUG', 'BUG', 'JUMP_LABEL',
    'SLAB', 'SLUB', 'SLOB', 'CGROUPS', 'MEMCG', 'MEMCG_KMEM', 'PM', 'PM_SLEEP', 'CMA', 'DMA_CMA',
    'KERNEL_MODE_NEON', 'CPU_SW_DOMAIN_PAN', 'SYSVIPC', 'NO_HZ_IDLE', 'NO_HZ_COMMON',
    'HIGH_RES_TIMERS', 'STACKPROTECTOR', 'STACKPROTECTOR_STRONG', 'MODULE_UNLOAD', 'KALLSYMS',
    'TRACEPOINTS', 'TRACING', 'FTRACE', 'DEBUG_SPINLOCK', 'DEBUG_MUTEXES', 'DEBUG_LOCK_ALLOC',
    'LOCKDEP', 'PROVE_LOCKING', 'DEBUG_OBJECTS', 'ARM_PATCH_PHYS_VIRT', 'CACHE_L2X0',
    'OUTER_CACHE', 'VMSPLIT_3G', 'HIGHMEM', 'ARM_LPAE', 'AEABI', 'VFP', 'NEON', 'DEBUG_FS',
    'SYSFS', 'MMU', 'THUMB2_KERNEL', 'CPU_V7', 'MEMORY_ISOLATION', 'COMPACTION', 'MIGRATION',
    'FAIR_GROUP_SCHED', 'RT_GROUP_SCHED', 'CGROUP_SCHED', 'SCHEDSTATS', 'AUDIT', 'SECURITY',
    'NUMA', 'SPARSEMEM', 'FLATMEM', 'ZONE_DMA', 'PAGE_EXTENSION', 'KASAN', 'MODULES_USE_ELF_REL',
    'MODVERSIONS', 'DEBUG_PAGEALLOC', 'FORCE_MAX_ZONEORDER', 'HZ', 'CPU_PM', 'SUSPEND',
)
# Additive or driver-local option families the spike may change.
SPIKE_ALLOWED = re.compile(
    r'^CONFIG_(ARM_APPENDED_DTB|ARM_ATAG_DTB_COMPAT|BLK_DEV_INITRD|INITRAMFS_\w+|RD_\w+|DECOMPRESS_\w+|'
    r'XZ_DEC\w*|LZ4_DECOMPRESS|CMDLINE\w*|DEVTMPFS\w*|BLK_DEV_LOOP\w*|SQUASHFS\w*|DYNAMIC_DEBUG|'
    r'NET\w*|INET\w*|UNIX\w*|PACKET\w*|XFRM\w*|TCP_CONG_\w+|DEFAULT_TCP_CONG|IPV6\w*|GRO_CELLS|'
    r'HAVE_NET_DSA|DVB_NET|WIRELESS\w*|CFG80211\w*|MAC80211\w*|CRYPTO_\w+|ASN1|ASYMMETRIC_\w+|'
    r'MPILIB|CLZ_TAB|OID_REGISTRY|PKCS7_\w+|X509_\w+|SYSTEM_(DATA_VERIFICATION|TRUSTED_\w+)|'
    r'MMC\w*|MTD_(UBI\w*|BLOCK|BLKDEVS|CMDLINE_PARTS)|UBIFS\w*|USB_\w+|MUSB_\w+|CHR_DEV_SG|'
    r'FRAMEBUFFER_CONSOLE\w*|LOGO\w*|MODULE_FORCE_LOAD|KERNEL_(GZIP|LZO|XZ)|ARK_HX170DEC|TOUCHSCREEN_GOODIX|ARK_DMA|DW_DMAC\w*|DMA_ENGINE\w*|DMA_OF|DMA_VIRTUAL_CHANNELS|SND_\w+|DST_CACHE|BACKLIGHT_PWM|PROFILING)$')


def check_spike_abi(sdk, ours):
    parse = lambda text: dict(re.findall(r'^(CONFIG_\w+)=(.*)$', text, re.M))
    a, b = parse(sdk), parse(ours)
    for key in SPIKE_ABI_KEYS:
        name = 'CONFIG_' + key
        if a.get(name) != b.get(name):
            raise RuntimeError(f'ABI option {name} differs from ark1668_defconfig: '
                               f'{a.get(name)} vs {b.get(name)}')
    stray = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k) and not SPIKE_ALLOWED.match(k))
    if stray:
        raise RuntimeError(f'spike config differs from ark1668_defconfig outside the allow-list: {stray}')
    changed = sum(1 for k in set(a) | set(b) if a.get(k) != b.get(k))
    print(f'Spike config: {changed} options differ from ark1668_defconfig, all allow-listed; '
          f'{len(SPIKE_ABI_KEYS)} ABI options identical')


def build_spike_extras(cross, make, kernel, work, out, sdk_archive, root):
    """LCD driver module, the hash-pinned SDK galcore.ko, and the stock squashfs with probes."""
    jobs = str(min(os.cpu_count() or 2, 8))
    run(*make, '-j', jobs, 'modules')
    release = (kernel / 'include/config/kernel.release').read_text().strip()
    galcore = out / 'galcore.ko'
    with galcore.open('wb') as f:
        run('tar', 'xzf', sdk_archive, '-O', GALCORE_MEMBER, stdout=f)
    if sha(galcore) != GALCORE_SHA256:
        raise RuntimeError('SDK galcore.ko differs from the audited hash')
    helpers = []
    # Every symbol galcore.ko imports must be exported by this kernel.
    exported = {line.split()[1] for line in (kernel / 'Module.symvers').read_text().splitlines() if line.split()}
    for helper in helpers:
        exported |= set(re.findall(r'__ksymtab_(\w+)', subprocess.check_output(
            [cross + 'nm', str(helper)], text=True)))
    imports = set(subprocess.check_output([cross + 'nm', '-u', str(galcore)], text=True).split()) - {'U'}
    missing = sorted(imports - exported)
    if missing:
        raise RuntimeError(f'galcore.ko imports symbols this kernel does not export: {missing}')
    vdec_ko = out / "arkvdec.ko"
    run(*make, '-j', str(min(os.cpu_count() or 2, 8)), 'modules')
    run(cross + 'strip', '--strip-debug', '-o', vdec_ko, kernel / 'drivers/soc/arkmicro/hx170dec/arkvdec.ko')
    # Decoder stage 2: stock-layout buffer pool for stock libmfc.
    pool_tree = work / 'memalloc-stock'
    if pool_tree.exists():
        shutil.rmtree(pool_tree)
    shutil.copytree(ROOT / 'recovery/plan-a/memalloc-stock', pool_tree)
    run(*make, f'M={pool_tree}', 'modules')
    pool_ko = out / 'memalloc_stock.ko'
    run(cross + 'strip', '--strip-debug', '-o', pool_ko, pool_tree / 'memalloc_stock.ko')
    # Projection display: stock-compatible /dev/ark_display for libarkcmn.
    disp_tree = work / 'ark-display-compat'
    if disp_tree.exists():
        shutil.rmtree(disp_tree)
    shutil.copytree(ROOT / 'recovery/plan-a/ark-display-compat', disp_tree)
    run(*make, f'M={disp_tree}', 'modules')
    disp_ko = out / 'ark_display_compat.ko'
    run(cross + 'strip', '--strip-debug', '-o', disp_ko, disp_tree / 'ark_display_compat.ko')
    goodix_ko = out / 'goodix.ko'
    run(cross + 'strip', '--strip-debug', '-o', goodix_ko, kernel / 'drivers/input/touchscreen/goodix.ko')
    # Audio: every sound module this kernel built (SDK drivers, simple card).
    audio = out / 'audio'
    if audio.exists():
        shutil.rmtree(audio)
    audio.mkdir()
    audio_kos = []
    for ko in sorted((kernel / 'sound').rglob('*.ko')):
        audio_kos.append(audio / ko.name)
        run(cross + 'strip', '--strip-debug', '-o', audio_kos[-1], ko)
    names = {k.name for k in audio_kos}
    for need in ('snd-soc-ark1668-i2s.ko', 'ark1668-sddac-codec.ko', 'ark1668-sdadc-codec.ko',
                 'snd-soc-ark-bd37033.ko', 'snd-soc-simple-card.ko'):
        if need not in names:
            raise RuntimeError(f'audio module {need} was not built')
    for module in (galcore, vdec_ko, pool_ko, disp_ko, goodix_ko, *audio_kos):
        info = work / (module.name + '.modinfo')
        run(cross + 'objcopy', '-O', 'binary', '--only-section=.modinfo', module, info)
        fields = dict(f.split('=', 1) for f in info.read_bytes().decode(errors='replace').split('\0') if '=' in f)
        if not fields.get('vermagic', '').startswith(release + ' '):
            raise RuntimeError(f'{module.name} vermagic {fields.get("vermagic")!r} does not match {release}')
    # Stock userspace as a read-only image: FAT cannot hold its symlinks.
    unpack('toolchain_glibc', Path('/work/toolchain-glibc'))
    headers = work / 'gal-headers'
    if headers.exists():
        shutil.rmtree(headers)
    headers.mkdir()
    run('tar', 'xzf', sdk_archive, '-C', headers, '--strip-components=6',
        '--wildcards', 'bm168-bsp/buildroot-external/package/demo-display/include/sys/HAL/*')
    stage = work / 'stock-stage'
    if stage.exists():
        shutil.rmtree(stage)
    run('cp', '-a', root, stage)
    (stage / 'opt/autotop').mkdir(parents=True, exist_ok=True)
    gcc = '/work/toolchain-glibc/bin/arm-linux-gcc'
    probe = stage / 'opt/autotop/probe-gal'
    (headers / 'HAL').mkdir(exist_ok=True)
    for h in list(headers.glob('*.h')):
        h.rename(headers / 'HAL' / h.name)
    run(gcc, '-O2', '-Wall', '-Werror', '-I', headers, '-o', probe, ROOT / 'recovery/plan-a/probe-gal.c',
        '-L', root / 'usr/lib', f'-Wl,-rpath-link,{root}/lib', f'-Wl,-rpath-link,{root}/usr/lib', '-lGAL')
    versions = subprocess.check_output([cross + 'objdump', '-T', str(probe)], text=True)
    newest = max((tuple(map(int, v.split('.'))) for v in re.findall(r'GLIBC_([0-9.]+)', versions)), default=(0,))
    if newest > (2, 27):
        raise RuntimeError(f'probe-gal needs glibc {newest}, newer than stock 2.27')
    # GPU compositing spike (docs/plan-a-gpu-display-spike.md): the same
    # library, drawing into the GUI layer.
    blend = stage / 'opt/autotop/probe-blend'
    run(gcc, '-O2', '-Wall', '-Werror', '-I', headers, '-o', blend, ROOT / 'recovery/plan-a/probe-blend.c',
        '-L', root / 'usr/lib', f'-Wl,-rpath-link,{root}/lib', f'-Wl,-rpath-link,{root}/usr/lib', '-lGAL')
    if re.search(r'\s(udiv|sdiv)\s', subprocess.check_output([cross + 'objdump', '-d', str(blend)], text=True)):
        raise RuntimeError('probe-blend contains hardware divide, which the Cortex-A5 lacks')
    # Video decoder stage 1: SDK libraries in their own directory, the probe
    # and the SDK's H.264 test pieces.
    sdk_dir = stage / 'opt/autotop/sdk'
    (sdk_dir / 'lib').mkdir(parents=True, exist_ok=True)
    (sdk_dir / 'pieces').mkdir(exist_ok=True)
    pkg = 'bm168-bsp/buildroot-external/package/'
    pieces = 'bm168-bsp/buildroot-external/board/arkmicro/ark169/rootfs_overlay/usr/share/pieces/'
    sdkx = work / 'sdk-vdec'
    if sdkx.exists():
        shutil.rmtree(sdkx)
    sdkx.mkdir()
    run('tar', 'xzf', sdk_archive, '-C', sdkx, '--wildcards', pkg + 'demo-display/lib/*',
        pkg + 'demo-display/include/*', pkg + 'libarkapi/software/include/*',
        pkg + 'libarkapi/software/src/ark_2d.c', pieces + '*')
    for lib in ('libmfc.so', 'libarkapi.so', 'libGAL.so'):
        shutil.copy2(sdkx / pkg / 'demo-display/lib' / lib, sdk_dir / 'lib' / lib)
    for piece in sorted((sdkx / pieces).glob('*.h264')):
        shutil.copy2(piece, sdk_dir / 'pieces' / piece.name)
    vdec = stage / 'opt/autotop/probe-vdec'
    # The prebuilt SDK libarkapi.so uses UDIV in arkapi_2d_process, which the
    # Cortex-A5 lacks (SIGILL); the probe carries the SDK's own ark_2d.c built
    # for this CPU, whose definitions take precedence.
    cpu = ['-mcpu=cortex-a5', '-mfpu=neon-vfpv4', '-mfloat-abi=hard']
    inc = ['-I', sdkx / pkg / 'libarkapi/software/include', '-I', sdkx / pkg / 'demo-display/include/sys',
           '-I', sdkx / pkg / 'demo-display/include/sys/HAL', '-I', sdkx / pkg / 'demo-display/include']
    ark2d_src = work / 'ark_2d.c'
    ark2d_src.write_bytes((sdkx / pkg / 'libarkapi/software/src/ark_2d.c').read_bytes().replace(b'\r\n', b'\n'))
    run(gcc, '-O2', *cpu, *inc, '-DLIBARKAPI_PLATFORM=LIBARKAPI_ARK1668', '-w', '-c', ark2d_src,
        '-o', work / 'ark_2d.o')
    run(gcc, '-O2', *cpu, '-Wall', '-Wno-unused-function', *inc, '-c', ROOT / 'recovery/plan-a/probe-vdec.c',
        '-o', work / 'probe-vdec.o')
    run(gcc, '-o', vdec, work / 'probe-vdec.o', work / 'ark_2d.o', '-L', sdk_dir / 'lib',
        f'-Wl,-rpath-link,{sdk_dir / "lib"}', f'-Wl,-rpath-link,{root}/lib', '-larkapi', '-lmfc', '-lGAL')
    if re.search(r'\s(udiv|sdiv)\s', subprocess.check_output([cross + 'objdump', '-d', str(vdec)], text=True)):
        raise RuntimeError('probe-vdec contains hardware divide, which the Cortex-A5 lacks')
    versions = subprocess.check_output([cross + 'objdump', '-T', str(vdec)], text=True)
    newest = max((tuple(map(int, v.split('.'))) for v in re.findall(r'GLIBC_([0-9.]+)', versions)), default=(0,))
    if newest > (2, 27):
        raise RuntimeError(f'probe-vdec needs glibc {newest}, newer than stock 2.27')
    # Decoder stage 2: the probe links the stock libmfc.so of the rootfs.
    mfc = stage / 'opt/autotop/probe-mfc'
    run(gcc, '-O2', *cpu, '-Wall', '-Werror', '-o', mfc, ROOT / 'recovery/plan-a/probe-mfc.c',
        '-L', root / 'usr/lib', f'-Wl,-rpath-link,{root}/lib', f'-Wl,-rpath-link,{root}/usr/lib', '-lmfc', '-larkcmn')
    versions = subprocess.check_output([cross + 'objdump', '-T', str(mfc)], text=True)
    newest = max((tuple(map(int, v.split('.'))) for v in re.findall(r'GLIBC_([0-9.]+)', versions)), default=(0,))
    if newest > (2, 27):
        raise RuntimeError(f'probe-mfc needs glibc {newest}, newer than stock 2.27')
    # Projection session kit (recovery/plan-a/plana-up.sh): every driver of
    # this build, the patched Wi-Fi driver and the static tools, so one image
    # carries the whole Plan A bring-up. No user data: /data comes from a
    # separate archive at run time.
    kit = stage / 'opt/autotop'
    for sub in ('modules', 'bin'):
        (kit / sub).mkdir(parents=True, exist_ok=True)
    wifi_ko = out / '8821cs.ko'
    for ko in (galcore, vdec_ko, pool_ko, disp_ko, goodix_ko, *audio_kos, wifi_ko):
        shutil.copy2(ko, kit / 'modules' / ko.name)
    for tool in ('mcuproxy', 'mculink', 'wavstat', 'evdump', 'evswipe', 'ratelog', 'profsum', 'peek-sysregs',
                 'launcherd', 'cpctl'):
        shutil.copy2(out / tool, kit / 'bin' / tool)
    # The logs as one file (the update page's link, the developer): a script among the app's tools.
    shutil.copy2(ROOT / 'recovery/plan-a/logs-pack', kit / 'bin/logs-pack')
    (kit / 'bin/logs-pack').chmod(0o755)
    # Option B: the image's own init (root = this squashfs), the session, the
    # state archive, and the wrapper D-Bus starts carplay through (carplay
    # reads its names only when its process starts; the wrapper gives it the
    # unit's name each time).
    for script in ('init-b', 'plana-up.sh', 'state-save', 'carplay-run', 'app-slot', 'state-mount', 'dev-access'):
        shutil.copy2(ROOT / 'recovery/plan-a' / script, kit / script)
        (kit / script).chmod(0o755)
    # Developer access (recovery/plan-a/dev-access): the SSH server, part of
    # the system (not of the app, so no app update can take it away), and the
    # public key of whoever may come in. Without local/keys/dev-authorized_keys
    # the image has no developer access and the launcher shows no row for it.
    sys_dir = kit / 'sys'
    sys_dir.mkdir(exist_ok=True)
    shutil.copy2(dropbear(Path('/work')), sys_dir / 'dropbearmulti')
    (sys_dir / 'dropbearmulti').chmod(0o755)
    for name in ('dropbear', 'dropbearkey', 'scp'):
        (sys_dir / name).unlink(missing_ok=True)
        (sys_dir / name).symlink_to('dropbearmulti')
    shutil.copy2(ROOT / 'recovery/plan-a/dev-shell', sys_dir / 'dev-shell')
    (sys_dir / 'dev-shell').chmod(0o755)
    # state-mount's attach for a partition it makes anew (busybox cannot set UBI's bad-block reserve).
    run('/work/toolchain/bin/arm-linux-gcc', '-std=c99', '-Os', '-static', '-Wall', '-Wextra', '-Werror',
        '-o', sys_dir / 'ubi-attach', ROOT / 'recovery/plan-a/ubi-attach.c')
    keys = ROOT / 'local/keys/dev-authorized_keys'
    (sys_dir / 'authorized_keys').unlink(missing_ok=True)
    if keys.is_file() and keys.read_text().strip():
        shutil.copyfile(keys, sys_dir / 'authorized_keys')
        (sys_dir / 'authorized_keys').chmod(0o644)
    # App updates (recovery/plan-a/app-slot): the key bundles are checked
    # against, this image's system level and the version of the app built
    # into it, and an empty directory a newer app is shown whole in.
    # The update key is the builder's own (make keys): an image trusts only the bundles its builder signs.
    update_key = ROOT / 'local/keys/update-key.pem'
    if not update_key.is_file():
        raise RuntimeError('no local/keys/update-key.pem: run "make keys" (docs/build.md)')
    shutil.copy2(update_key, kit / 'update-key.pem')
    for name in ('system-level', 'app-version'):
        shutil.copy2(ROOT / 'recovery/plan-a' / name, kit / name)
    (kit / 'app').mkdir(exist_ok=True)
    # The owner's icon for the car's tile in CarPlay (design/assets), in the
    # stock icon's form (120 x 120 RGBA); the wrapper names it to carplay.
    tile = ROOT / 'local/assets/carplay-icon@2x.png'    # the builder's own picture, else the neutral default
    run('python3', ROOT / 'tools/design_icon.py', tile if tile.is_file() else ROOT / 'design/assets/carplay-icon@2x.png',
        kit / 'carplay-icon.png')
    # The app as it is in this image, beside the image: what tools/app_bundle.py
    # signs into a bundle (the same entries app-slot puts in place).
    app = out / 'app'
    if app.exists():
        shutil.rmtree(app)
    app.mkdir()
    shutil.copytree(kit / 'bin', app / 'bin')
    for name in ('plana-up.sh', 'state-save', 'carplay-run', 'carplay-icon.png'):
        shutil.copy2(kit / name, app / name)
    service = stage / 'usr/share/dbus-1/services/com.arkmicro.carplay.service'
    text = service.read_text()
    if text.count('Exec=/usr/bin/carplay') != 1:
        raise RuntimeError('stock carplay D-Bus service file is not as expected')
    service.write_text(text.replace('Exec=/usr/bin/carplay', 'Exec=/opt/autotop/carplay-run'))
    # gocsdk's optional settings (read at its start): Secure Simple Pairing on,
    # so phones pair by confirming a code instead of typing stock's PIN. The
    # binary's default is off; other keys keep their defaults (docs/launcher-design.md).
    shutil.copy2(ROOT / 'recovery/plan-a/btDefSetting.json', stage / 'etc/goc/btDefSetting.json')
    image = out / 'stock-rootfs.sqfs'
    image.unlink(missing_ok=True)
    os.environ.setdefault('SOURCE_DATE_EPOCH', str(BOARD['source_date_epoch']))
    run('mksquashfs', stage, image, '-comp', 'gzip', '-noappend', '-no-xattrs', '-quiet', '-no-progress')
    print(f'Spike extras: {galcore.name}, {image.name} ({image.stat().st_size} bytes); '
          'galcore imports all exported')
    return [galcore, vdec_ko, pool_ko, disp_ko, goodix_ko, *audio_kos, image]


STOCK_KERNEL_SIZE = 0x32fed8   # stock U-Boot nandboot reads this many bytes
RESCUE_WINDOW = (0x160000, 0x7100000)  # the kernel and rootfs partitions together


def build_rescue(cross, make, kernel, work, out, source, listing):
    """The rescue installer (recovery/plan-a/init-rescue): this kernel with the
    bounded writer's window over kernel and rootfs, and an initramfs of its
    own: BusyBox, nand-restore (with its fixture suite run here) and rescue-ui."""
    build_writer(cross, out, work)
    ui = out / 'rescue-ui'
    run(cross + 'gcc', '-std=gnu11', '-D_GNU_SOURCE', '-Os', '-static', '-Wall', '-Wextra', '-Werror',
        '-o', ui, ROOT / 'recovery/plan-a/rescue-ui.c')
    init = ROOT / 'recovery/plan-a/init-rescue'
    keep = ('/bin/busybox', '/lib/ld-linux-armhf.so.3', '/lib/libc.so.6', '/lib/libcrypt.so.1')
    lines = [l for l in listing if not l.startswith('file ') or l.split()[1] in keep]
    lines += [f'file /init {init} 0755 0 0', f'file /usr/bin/nand-restore {out / "nand-restore"} 0755 0 0',
              f'file /usr/bin/rescue-ui {ui} 0755 0 0']
    cpio_list = work / 'rescue-initramfs.list'
    cpio_list.write_text('\n'.join(lines) + '\n')
    config = (kernel / '.config').read_text()
    config = re.sub(r'^CONFIG_INITRAMFS_SOURCE=.*$', f'CONFIG_INITRAMFS_SOURCE="{cpio_list}"', config, flags=re.M)
    (kernel / '.config').write_text(config)
    run(*make, 'olddefconfig')
    if f'CONFIG_INITRAMFS_SOURCE="{cpio_list}"' not in (kernel / '.config').read_text():
        raise RuntimeError('the rescue kernel does not name its initramfs')
    run(*make, '-j', str(min(os.cpu_count() or 2, 8)), 'zImage')
    dts = work / 'rescue.dts'
    with dts.open('w') as f:
        run(cross + 'gcc', '-E', '-P', '-x', 'assembler-with-cpp', '-nostdinc', '-undef', '-D__DTS__',
            '-I', source / 'include', '-I', ROOT / 'recovery', ROOT / 'recovery/bm168-rescue.dts', stdout=f)
    dtb = out / 'rescue.dtb'
    run(kernel / 'scripts/dtc/dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, dts)
    check_writer_dtb(kernel / 'scripts/dtc/dtc', dtb, RESCUE_WINDOW)
    text = subprocess.check_output([str(kernel / 'scripts/dtc/dtc'), '-I', 'dtb', '-O', 'dts', str(dtb)],
                                   stderr=subprocess.DEVNULL, text=True)
    # 64 MiB for Linux: what U-Boot loaded above stays untouched. No state
    # window: this kernel writes kernel and rootfs, never userdata.
    if 'mem=64M cma=0 rdinit=/init' not in text or 'autotop,state-window' in text:
        raise RuntimeError('the rescue DTB must give Linux 64 MiB, an initramfs init and no state window')
    payload = (kernel / 'arch/arm/boot/zImage').read_bytes() + dtb.read_bytes()
    if len(payload) > 8 * 1024 * 1024:
        raise RuntimeError('the rescue installer exceeds the 8 MiB load budget')
    image = out / 'rescue.bin'
    image.write_bytes(payload)
    print(f'Rescue installer: rescue.bin {len(payload)} bytes (BOOT.BIN on the rescue stick)')
    return [image, dtb]


def build_plana_b(cross, make, kernel, work, out, source, squashfs, state):
    """Option B kernel (no initramfs) and the rootfs partition's UBI image."""
    config = (kernel / '.config').read_text()
    config = re.sub(r'^CONFIG_INITRAMFS_SOURCE=.*$', 'CONFIG_INITRAMFS_SOURCE=""', config, flags=re.M)
    (kernel / '.config').write_text(config)
    run(*make, 'olddefconfig')
    if 'CONFIG_INITRAMFS_SOURCE=""' not in (kernel / '.config').read_text():
        raise RuntimeError('option B kernel still names an initramfs')
    run(*make, '-j', str(min(os.cpu_count() or 2, 8)), 'zImage')
    dts = work / 'plana-b.dts'
    with dts.open('w') as f:
        run(cross + 'gcc', '-E', '-P', '-x', 'assembler-with-cpp', '-nostdinc', '-undef', '-D__DTS__',
            *(['-DSTATE_WINDOW'] if state else []),
            '-I', source / 'include', '-I', ROOT / 'recovery', ROOT / 'recovery/bm168-plana-b.dts', stdout=f)
    dtb = out / 'plana-b.dtb'
    run(kernel / 'scripts/dtc/dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, dts)
    text = subprocess.check_output([str(kernel / 'scripts/dtc/dtc'), '-I', 'dtb', '-O', 'dts', str(dtb)],
                                   stderr=subprocess.DEVNULL, text=True)
    for needle in ('ubi.mtd=rootfs ubi.block=0,0 root=/dev/ubiblock0_0 rootfstype=squashfs ro',
                   'init=/opt/autotop/init-b', 'arm,pl310-cache'):
        if needle not in text:
            raise RuntimeError(f'option B DTB lacks {needle!r}')
    if 'rdinit' in text:
        raise RuntimeError('option B DTB must not name an initramfs init')
    # Every NAND partition read-only (the rootfs one always: UBI then attaches
    # it read-only); with the state window, userdata alone is writable.
    check_state_dtb(kernel / 'scripts/dtc/dtc', dtb, state)
    payload = (kernel / 'arch/arm/boot/zImage').read_bytes() + dtb.read_bytes()
    if len(payload) > STOCK_KERNEL_SIZE:
        raise RuntimeError(f'option B kernel {len(payload)} bytes exceeds stock kernel_size {STOCK_KERNEL_SIZE}')
    image = out / 'plana-b.bin'
    image.write_bytes(payload)
    # Rootfs partition image: volume 0 = the Plan A squashfs (dynamic, exact
    # size, no autoresize: nothing to do for UBI on a read-only attach). The
    # private session volume (plana-state) is added only at install time.
    leb = BOARD['leb_size']
    ini = work / 'plana-root.ini'
    ini.write_text(f'[plana-root]\nmode=ubi\nimage={squashfs}\nvol_type=dynamic\nvol_id=0\n'
                   f'vol_name=plana-root\nvol_size={-(-squashfs.stat().st_size // leb) * leb}\n')
    ubi = out / 'plana-root.ubi'
    run('ubinize', '-o', ubi, '-p', BOARD['peb_size'], '-m', BOARD['min_io_size'],
        '-s', BOARD['min_io_size'], '-O', BOARD['vid_header_offset'], '-Q', BOARD['ubi_image_sequence'] + 1, ini)
    if ubi.stat().st_size > BOARD['rootfs_size'] - 24 * BOARD['peb_size']:
        raise RuntimeError('option B UBI image exceeds the conservative partition budget')
    print(f'Option B: plana-b.bin {len(payload)} bytes (limit {STOCK_KERNEL_SIZE}), '
          f'plana-root.ubi {ubi.stat().st_size} bytes')
    return [image, dtb, ubi]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--readonly-storage', action='store_true')
    parser.add_argument('--capture', action='store_true',
                        help='read-only NAND plus a verified, writable capture stick')
    parser.add_argument('--writer', action='store_true',
                        help='bounded NAND writer for one physical window (see --window)')
    parser.add_argument('--window', help='START:SIZE physical write window for --writer')
    parser.add_argument('--wifi', action='store_true',
                        help='read-only NAND plus SDIO host, cfg80211 and the RTL8821CS module')
    parser.add_argument('--spike', action='store_true',
                        help='Plan A GPU/display spike: Wi-Fi profile plus LCDC, GPU IRQ, squashfs')
    parser.add_argument('--state', action='store_true',
                        help='with --plana-b: the stock userdata partition writable at run time '
                             '(recovery/patches-state); a separate build beside the read-only one')
    parser.add_argument('--plana-b', action='store_true',
                        help='Plan A option B: spike kernel (XZ, UBI) for a read-only NAND root, '
                             'plus a rehearsal image that roots on the U DISK squashfs')
    args = parser.parse_args()
    policy, sdk_archive = verified_sdk()
    capture, writer, plana_b = args.capture, args.writer, args.plana_b
    spike = args.spike or plana_b
    wifi = args.wifi or spike
    if sum((capture, writer, args.wifi, args.spike, plana_b)) > 1:
        raise RuntimeError('choose one of --capture, --writer, --wifi, --spike, --plana-b')
    storage = args.readonly_storage or capture or writer or wifi
    state = args.state
    if state and not plana_b:
        raise RuntimeError('--state is only valid with --plana-b')
    profile = ('plana-b-state' if state else 'plana-b' if plana_b else 'spike' if spike else 'wifi' if wifi else 'writer' if writer else 'capture' if capture else
               'readonly' if storage else 'console')
    window = None
    bootlogo = False
    if writer:
        if not args.window:
            raise RuntimeError('--writer requires --window START:SIZE')
        window = tuple(int(v, 0) for v in args.window.split(':'))
        # The boot picture's partition, whole, is the one window outside
        # kernel/rootfs, and makes a writer of its own kind.
        bootlogo = window == restore_manifest.BOOTLOGO
        if not restore_manifest.window_valid(*window, bootlogo=bootlogo):
            raise RuntimeError('write window must be erase-aligned and inside kernel/rootfs, '
                               'or the whole bootlogo partition')
    elif args.window:
        raise RuntimeError('--window is only valid with --writer')
    if ROOT != Path('/project'):
        raise RuntimeError('Use make recovery in the Linux build container')
    out = Path({'plana-b': '/out/plana-b', 'plana-b-state': '/out/plana-b-state', 'spike': '/out/recovery-spike', 'wifi': '/out/recovery-wifi', 'writer': '/out/recovery-writer', 'capture': '/out/recovery-capture',
                'readonly': '/out/recovery-readonly', 'console': '/out/recovery'}[profile])
    out.mkdir(exist_ok=True)
    (out / 'manifest.json').unlink(missing_ok=True)
    commit = policy['sanitized_commit']
    work = Path('/work/recovery-sanitized') / profile
    work.mkdir(parents=True, exist_ok=True)
    # Linux filenames collide on a case-insensitive Mac checkout. Build the
    # verified archive on the Linux volume, never a raw vendor checkout.
    source = work / 'sdk-linux'
    marker = work / 'sdk-commit'
    patches = sorted((ROOT / 'recovery/patches').glob('*.patch'))
    if not storage:
        patches = [p for p in patches if p.name.startswith('0001-')]
    if writer or state:
        patches += sorted((ROOT / 'recovery/patches-writer').glob('*.patch'))
    if state:
        patches += sorted((ROOT / 'recovery/patches-state').glob('*.patch'))
    if spike:
        patches += sorted((ROOT / 'recovery/patches-spike').glob('*.patch'))
    source_id = (policy['snapshot']['sha256'] + '\n' + LOCK['toolchain']['sha256'] +
                 '\n' + '\n'.join(f'{p.name} {sha(p)}' for p in patches))
    if not marker.exists() or marker.read_text().strip() != source_id.strip():
        marker.unlink(missing_ok=True)
        if source.exists():
            shutil.rmtree(source)
        # Archive timestamps are fixed; cached objects must not survive a new
        # source/patch/toolchain identity even if their mtimes are newer.
        if (work / 'kernel').exists():
            shutil.rmtree(work / 'kernel')
        source.mkdir()
        run('tar', 'xf', sdk_archive, '--strip-components=2', '--no-same-owner',
            '-C', source, '--wildcards', 'bm168-bsp/linux/*')
        for patch in patches:
            run('git', 'apply', patch, cwd=source)
        marker.write_text(source_id + '\n')
    if writer or state:
        # The kernel must compile exactly the format code tested on the host.
        if (source / 'drivers/mtd/nand/raw/bm168_nand_format.h').read_bytes() != \
                (ROOT / 'recovery/bm168_nand_format.h').read_bytes():
            raise RuntimeError('writer patch format header differs from recovery/bm168_nand_format.h')
    unpack('toolchain', Path('/work/toolchain'))
    cross = '/work/toolchain/bin/arm-linux-'
    image, reference = stock_image()
    extracted = work / 'extracted'
    if extracted.exists():
        shutil.rmtree(extracted)
    with (out / 'extraction.log').open('w') as log:
        run('ubireader_extract_files', '-k', '-x', '-o', extracted, image,
            stdout=log, stderr=subprocess.STDOUT)
    if re.search(r'\b(?:Warn|Error|Fatal)\b', (out / 'extraction.log').read_text(), re.I):
        raise RuntimeError('Baseline extraction reported an error; inspect extraction.log')
    roots = list(extracted.glob('*/rootfs'))
    if len(roots) != 1:
        raise RuntimeError('Expected one rootfs volume')
    root = roots[0]
    stock_verify(root, reference)
    files = ['bin/busybox', 'lib/ld-linux-armhf.so.3', 'lib/libc.so.6', 'lib/libcrypt.so.1']
    if storage:
        files.append('usr/bin/openssl')
    if wifi:
        files += stock_closure(root, ['usr/bin/hostapd', 'usr/bin/hostapd_cli'], cross, files)
    init = ROOT / ('recovery/init-plana-bt' if plana_b else 'recovery/init-spike' if spike else 'recovery/init-wifi' if wifi else 'recovery/init-writer' if writer else 'recovery/init-capture' if capture else
                   'recovery/init-readonly' if storage else 'recovery/init')
    runtime = work / 'runtime'
    if runtime.exists():
        shutil.rmtree(runtime)
    runtime.mkdir()
    inputs = {}
    listing = [f'dir /{d} 0755 0 0' for d in
               ('bin', 'lib', 'dev', 'proc', 'sys', 'tmp', 'root', 'usr', 'usr/bin', 'usr/lib',
                'etc', 'var', 'var/run', 'mnt', 'mnt/usb')]
    for name in files:
        path = (root / name).resolve()
        if not path.is_relative_to(root) or not path.is_file():
            raise RuntimeError(f'Invalid baseline file: {name}')
        inputs[name] = sha(path)
        listing.append(f'file /{name} {path} 0755 0 0')
        target = runtime / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    listing += ['nod /dev/console 0600 0 0 c 5 1', 'nod /dev/null 0666 0 0 c 1 3',
                f'file /init {init} 0755 0 0']
    if wifi:
        listing.append(f'file /etc/hostapd-bench.conf {ROOT / "recovery/wifi/hostapd-bench.conf"} 0600 0 0')
    for name in ('sh', 'ls', 'cat', 'mount', 'umount', 'dmesg', 'uname', 'reboot', 'sleep', 'ps'):
        listing.append(f'slink /bin/{name} busybox 0777 0 0')
    cpio_list = work / 'initramfs.list'
    cpio_list.write_text('\n'.join(listing) + '\n')
    run('qemu-arm', '-L', runtime, runtime / 'bin/busybox', 'sh', '-c', 'echo VENDOR_BUSYBOX_OK')
    run('qemu-arm', '-L', runtime, runtime / 'bin/busybox', 'sh', '-n', init)
    if storage:
        empty = work / 'empty'
        empty.write_bytes(b'')
        digest = subprocess.check_output(['qemu-arm', '-L', str(runtime),
                    str(runtime / 'usr/bin/openssl'), 'dgst', '-sha256', str(empty)], text=True)
        if sha(empty) not in digest:
            raise RuntimeError('Selected vendor hashing tool failed its runtime check')
        probe = out / 'nand-readcheck'
        run(cross + 'gcc', '-std=c99', '-D_GNU_SOURCE', '-Os', '-static',
            '-Wall', '-Wextra', '-Werror', '-o', probe,
            ROOT / 'recovery/nand-readcheck.c')
        # A regular file must fail before any NAND read can be attempted.
        rejected = subprocess.run(['qemu-arm', str(probe), str(empty)],
                                  capture_output=True, text=True)
        if rejected.returncode != 1 or 'MEMGETINFO' not in rejected.stderr:
            raise RuntimeError('Read-only NAND probe failed its non-MTD rejection check')
        listing.append(f'file /usr/bin/nand-readcheck {probe} 0755 0 0')
        check_applets(runtime, init)
        if capture:
            listing.append(f'file /usr/bin/nand-capture {build_capture(cross, out, work, empty)} 0755 0 0')
        if writer:
            listing.append(f'file /usr/bin/nand-restore {build_writer(cross, out, work, bootlogo)} 0755 0 0')
        if spike:
            # Peripheral checks (MFi identity, touch events, pad dump, audio
            # levels, MCU link: receive-only or stock's two read requests).
            for tool, src in (('mfi-info', 'recovery/plan-a/periph/mfi-info.c'),
                              ('wavstat', 'recovery/plan-a/periph/wavstat.c'),
                              ('mculink', 'recovery/plan-a/periph/mculink.c'),
                              ('mcuproxy', 'recovery/plan-a/periph/mcuproxy.c'),
                              ('profsum', 'recovery/plan-a/periph/profsum.c'),
                              ('threadsample', None),
                              ('audioprobe', None),
                              ('evdump', 'recovery/plan-a/periph/evdump.c'),
                              ('evswipe', 'recovery/plan-a/periph/evswipe.c'),
                              ('ratelog', 'recovery/plan-a/periph/ratelog.c'),
                              ('peek-sysregs', 'recovery/plan-a/peek-sysregs.c'),
                              ('gt911-poll', 'recovery/plan-a/periph/gt911-poll.c')):
                if src is None:
                    listing.append(f'file /usr/bin/{tool} {ROOT}/recovery/plan-a/periph/{tool}.sh 0755 0 0')
                    continue
                binary = out / tool
                run(cross + 'gcc', '-std=c99', '-D_GNU_SOURCE', '-Os', '-static',
                    '-Wall', '-Wextra', '-Werror', '-o', binary, ROOT / src, '-lm')
                listing.append(f'file /usr/bin/{tool} {binary} 0755 0 0')
            # New Launcher (docs/launcher-design.md): launcherd (with the
            # LVGL UI) and the cpctl test tool, from the same CMake build as
            # the UI; shipped in the kit.
            ui_bin = configure(work)
            for tool in ('launcherd', 'cpctl'):
                shutil.copy2(ui_bin.parent / tool, out / tool)
        cpio_list.write_text('\n'.join(listing) + '\n')
    if plana_b:
        # Rehearsal initramfs: BusyBox and its libraries and the init only;
        # the tools and the session live in the squashfs (/opt/autotop).
        keep = ('/bin/busybox', '/lib/ld-linux-armhf.so.3', '/lib/libc.so.6', '/lib/libcrypt.so.1', '/init')
        listing = [l for l in listing if not l.startswith('file ') or l.split()[1] in keep]
        listing.append('dir /newroot 0755 0 0')
        cpio_list.write_text('\n'.join(listing) + '\n')
    config = work / 'requested.config'
    base = (ROOT / 'recovery/kernel.config').read_text()
    if spike:
        # The binary galcore.ko fixes the kernel ABI: start from the SDK's
        # ark1668_defconfig and allow only checked, additive differences.
        sdk_config = work / 'sdk-defconfig'
        sdk_config.mkdir(exist_ok=True)
        run('make', '-C', str(source), f'O={sdk_config}', 'ARCH=arm', f'CROSS_COMPILE={cross}', 'ark1668_defconfig')
        base = (sdk_config / '.config').read_text() + (ROOT / 'recovery/spike-base.config').read_text()
    config.write_text(base +
                      ((ROOT / 'recovery/storage.config').read_text() if storage else '') +
                      ((ROOT / 'recovery/wifi.config').read_text() if wifi else '') +
                      ((ROOT / 'recovery/spike.config').read_text() if spike else '') +
                      ((ROOT / 'recovery/plana-b.config').read_text() if plana_b else '') +
                      f'\nCONFIG_INITRAMFS_SOURCE="{cpio_list}"\n')
    kernel = work / 'kernel'
    kernel.mkdir(exist_ok=True)
    os.environ.update(SOURCE_DATE_EPOCH=str(BOARD['source_date_epoch']),
                      KBUILD_BUILD_TIMESTAMP='Thu Feb 16 00:00:00 UTC 2023',
                      KBUILD_BUILD_USER='autotop', KBUILD_BUILD_HOST='recovery',
                      KBUILD_BUILD_VERSION='1')
    make = ['make', '-C', str(source), f'O={kernel}', 'ARCH=arm', f'CROSS_COMPILE={cross}',
            *(['KCFLAGS=-DBM168_BOOTLOGO_WINDOW'] if bootlogo else [])]
    shutil.copyfile(config, kernel / '.config')
    run(*make, 'olddefconfig')
    generated = (kernel / '.config').read_text()
    required = ('MMU', 'CPU_V7', 'ARCH_ARKMICRO', 'SOC_ARK1668', 'BLK_DEV_INITRD', 'ARM_APPENDED_DTB',
                'CMDLINE_FORCE', 'BINFMT_ELF', 'BINFMT_SCRIPT', 'SERIAL_ARKMICRO_CONSOLE',
                'DEVTMPFS', 'PROC_FS', 'SYSFS', 'VFP')
    if storage:
        required += ('BLOCK', 'MTD', 'MTD_OF_PARTS', 'MTD_NAND_ARKMICRO', 'USB_STORAGE',
                     'USB_MUSB_ARKMICRO', 'USB_MUSB_HOST', 'MUSB_PIO_ONLY', 'NOP_USB_XCEIV', 'VFAT_FS', 'GPIO_ARKMICRO')
    if wifi:
        required += ('MODULES', 'NET', 'INET', 'UNIX', 'PACKET', 'CFG80211', 'MMC', 'MMC_DW', 'MMC_DW_PLTFM')
    if spike:
        required += ('FB', 'BLK_DEV_LOOP', 'SQUASHFS', 'SQUASHFS_ZLIB', 'HIGH_RES_TIMERS')
    if spike:
        check_spike_abi((work / 'sdk-defconfig/.config').read_text(), generated)
        required = tuple(n for n in required if n not in ('MUSB_PIO_ONLY', 'CMDLINE_FORCE'))
        if 'CONFIG_ATAGS=y\n' in generated:
            raise RuntimeError('spike kernel must take its command line only from the DT')
        required += ('FB_ARK1668LCD',)
    if plana_b:
        required += ('KERNEL_XZ', 'MTD_UBI', 'MTD_UBI_BLOCK', 'SQUASHFS')
    for name in required:
        if f'CONFIG_{name}=y\n' not in generated:
            raise RuntimeError(f'Required kernel option missing: {name}')
    forbidden = ('MTD', 'BLOCK', 'MODULES', 'DEVMEM', 'DEVKMEM', 'MCU_SERIAL_ARKMICRO',
                 'ARKTOOL_SERIAL_ARKMICRO', 'HS_SERIAL_ARKMICRO', 'WATCHDOG', 'USB',
                 'MMC', 'FB', 'DRM', 'SOUND', 'INPUT', 'VT', 'I2C', 'SPI',
                 'GPIO_ARKMICRO', 'PINCTRL_ARKMICRO', 'ARK_CARBACK', 'ARK_MEMALLOC')
    if storage:
        forbidden = tuple(n for n in forbidden if n not in ('MTD', 'BLOCK', 'USB', 'GPIO_ARKMICRO'))
        forbidden += ('MTD_UBI', 'MTD_BLOCK', 'MTD_CMDLINE_PARTS', 'USB_GADGET', 'CHR_DEV_SG', 'BLK_DEV_BSG')
    if wifi:
        forbidden = tuple(n for n in forbidden if n not in ('MODULES', 'MMC'))
        forbidden += ('MMC_BLOCK', 'CFG80211_WEXT', 'MAC80211', 'MODULE_FORCE_LOAD', 'IPV6')
    if spike:
        # SDK base: only the storage-safety and display-ownership rules apply;
        # NAND write protection is enforced by the recovery driver patches.
        forbidden = ('MTD_UBI', 'MTD_BLOCK', 'FRAMEBUFFER_CONSOLE', 'LOGO', 'MMC_BLOCK', 'USB_GADGET')
        if plana_b:
            # UBI attaches the rootfs partition read-only (read-only MTD).
            forbidden = tuple(n for n in forbidden if n != 'MTD_UBI')
    for name in forbidden:
        if re.search(rf'^CONFIG_{name}=[ym]$', generated, re.M):
            raise RuntimeError(f'Forbidden driver enabled: {name}')
    run(*make, '-j', str(min(os.cpu_count() or 2, 8)), 'zImage')
    dts = work / 'console.dts'
    defines = [f'-DWRITE_WINDOW_START={window[0]:#x}', f'-DWRITE_WINDOW_SIZE={window[1]:#x}'] if writer else \
        ['-DSTATE_WINDOW'] if state else []
    with dts.open('w') as f:
        run(cross + 'gcc', '-E', '-P', '-x', 'assembler-with-cpp', '-nostdinc',
            '-undef', '-D__DTS__', *defines, '-I', source / 'include', '-I', ROOT / 'recovery',
            ROOT / ('recovery/bm168-plana-bt.dts' if plana_b else 'recovery/bm168-spike.dts' if spike else 'recovery/bm168-wifi.dts' if wifi else 'recovery/bm168-writer.dts' if writer else
                    'recovery/bm168-readonly.dts' if storage else 'recovery/bm168-console.dts'), stdout=f)
    dtb = out / 'bm168-console.dtb'
    run(kernel / 'scripts/dtc/dtc', '-I', 'dts', '-O', 'dtb', '-o', dtb, dts)
    if writer:
        check_writer_dtb(kernel / 'scripts/dtc/dtc', dtb, window)
    if plana_b:
        check_state_dtb(kernel / 'scripts/dtc/dtc', dtb, state)
    if spike:
        text = subprocess.check_output([str(kernel / 'scripts/dtc/dtc'), '-I', 'dtb', '-O', 'dts', str(dtb)],
                                       stderr=subprocess.DEVNULL, text=True)
        if 'bootargs = "console=ttyS0,115200n8 earlyprintk mem=180M cma=0 rdinit=/init' not in text:
            raise RuntimeError('spike DTB lacks the RAM-only boot arguments')
        # Decoder clock must be CPU PLL / 2 (SDK and stock); the bootloader's
        # value makes the decoder corrupt every frame.
        mfc = re.search(r'mfcclk \{(.*?)\};', text, re.S)
        if not mfc or 'index-value = <0x1>;' not in mfc.group(1) or 'div-value = <0x2>;' not in mfc.group(1):
            raise RuntimeError('spike DTB does not set mfcclk to CPU PLL / 2')
        # Peripheral stage: the pads and devices that must be present.
        for needle, why in (('arkmicro,ark1668-pinctrl', 'pinctrl'), ('serial@e4800000', 'ttyHS1'),
                            ('i2c@e4300000', 'I2C bus 0'), ('goodix,gt911', 'touch'),
                            ('apple_encpy_ic_rst', 'MFi reset hog'), ('bt-power', 'Bluetooth power pad'), ('hsserial1', 'ttyHS1 alias'),
                            ('arkmicro,ark-dma', 'DMA controller'), ('rx-tx', 'Wi-Fi SDIO DMA'), ('cap-sdio-irq', 'SDIO card interrupt'),
                            ('arkmicro,ark1668-i2s', 'I2S'), ('arkmicro,ark1668-sddac', 'SDDAC'),
                            ('arkmicro,drv_bd37033', 'BD37033'), ('ARK-SDDAC', 'stock card name'),
                            ('serial@e8000000', 'MCU link ttyS2'), ('serial2', 'ttyS2 alias'),
                            ('pwm-backlight', 'stock backlight path'), ('osd1-vp', 'stock GUI colour')):
            if needle not in text:
                raise RuntimeError(f'spike DTB lacks {why} ({needle})')
    module = build_wifi_module(cross, make, kernel, work, out) if wifi else None
    extra = build_spike_extras(cross, make, kernel, work, out, sdk_archive, root) if spike else []
    payload = (kernel / 'arch/arm/boot/zImage').read_bytes() + dtb.read_bytes()
    if len(payload) > 8 * 1024 * 1024:
        raise RuntimeError('Console candidate exceeds its conservative 8 MiB load budget')
    (out / 'autotop-recovery.bin').write_bytes(payload)
    shutil.copyfile(kernel / '.config', out / 'kernel.config')
    shutil.copyfile(kernel / 'usr/initramfs_data.cpio.gz', out / 'initramfs.cpio.gz')
    if plana_b:
        extra += build_plana_b(cross, make, kernel, work, out, source, extra[-1], state)
    if state:
        extra += build_rescue(cross, make, kernel, work, out, source, listing)
    manifest = {
        'kind': {'plana-b-state': 'Plan A option B with the state window (userdata writable at run time, '
                                  'everything else read-only): plana-b.bin, plana-root.ubi, autotop-recovery.bin',
                 'plana-b': 'Plan A option B: plana-b.bin (XZ kernel, root = UBI volume 0 on read-only NAND), '
                            'plana-root.ubi (rootfs partition image), autotop-recovery.bin (RAM rehearsal '
                            'rooted on the U DISK squashfs)',
                 'spike': 'RAM Plan A GPU/display spike: read-only NAND, LCDC and galcore modules on USB',
                 'wifi': 'RAM Wi-Fi probe: read-only NAND, SDIO host, RTL8821CS module on USB',
                 'writer': 'RAM bounded NAND writer, one armed physical window',
                 'capture': 'RAM read-only NAND acquisition to a verified capture stick',
                 'readonly': 'RAM read-only USB/NAND probe',
                 'console': 'RAM console probe, no storage drivers'}[profile],
        'boot_tested': False,
        'restoration_supported': False, 'sdk_commit': commit,
        'write_window': ({'start': f'{window[0]:#x}', 'size': f'{window[1]:#x}',
                          'blocks': window[1] // 131072} if writer else None),
        'sdk_upstream_commit': policy['upstream_commit'],
        'sdk_tree': policy['sanitized_tree'],
        'sdk_archive_sha256': policy['snapshot']['sha256'],
        'recovery_patches_sha256': {p.name: sha(p) for p in patches},
        'baseline_rootfs_sha256': sha(stock_image()[0]), 'vendor_files_sha256': inputs,
        'toolchain': LOCK['toolchain'],
        'project_inputs_sha256': {str(p.relative_to(ROOT)): sha(p) for p in
                                 [*sorted((ROOT / 'recovery').rglob('*')), Path(__file__),
                                  ROOT / 'tools/build.py', ROOT / 'tools/Dockerfile',
                                  ROOT / 'sources.lock.json', ROOT / LOCK['arkmicro_sdk']['sanitization_manifest'],
                                  ROOT / 'Makefile'] if p.is_file()},
        'artifacts': {p.name: {'bytes': p.stat().st_size, 'sha256': sha(p),
                              'crc32': f'{zlib.crc32(p.read_bytes()):08x}'} for p in
                      [out / 'autotop-recovery.bin', dtb, out / 'kernel.config', out / 'initramfs.cpio.gz'] +
                      ([out / 'nand-readcheck'] if storage else []) +
                      ([out / 'nand-capture'] if capture else []) +
                      ([out / 'nand-restore'] if writer else []) +
                      ([module] if wifi else []) + extra},
        'development_dependency_exceptions': ([{
            'file': 'GALCORE.KO', 'origin': 'SDK buildroot-external/package/libgal/software/lib/galcore.ko',
            'sha256': GALCORE_SHA256, 'purpose': 'Vivante GPU kernel driver for the RAM spike; binary only',
            'audit_disposition': 'retained-reference-not-runtime-approved',
            'use': 'loaded from USB in a RAM development session; never embedded in an image'}] if spike else []),
        'wifi_driver': ({k: LOCK['rtl8821cs'][k] for k in ('url', 'commit', 'path', 'tree', 'driver_version', 'sha256')}
                        | {'patches_sha256': {q.name: sha(q) for q in sorted((ROOT / 'recovery/wifi/patches').glob('*.patch'))}}
                        if wifi else None),
        'limitations': (['Writer: erase/program only inside write_window, only when armed via '
                         'autotop_writer; untested on hardware; review the restore manifest '
                         'with the owner before arming'] if writer else []) + [
                        'This exact candidate has not been hardware-tested',
                        'S-Loader ECC not supported; no NAND filesystem mounts' if storage else
                        'Candidate has no NAND, MCU, display or USB support',
                        'Restoration still needs a separately validated storage-capable environment'],
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Built {out}/autotop-recovery.bin; coordinate a RAM-only boot before use.')


if __name__ == '__main__':
    main()
