#!/usr/bin/env python3
"""Driver-profile patch for the unit's GD32 MCU, programmed over SWD (Black
Magic Probe + arm-none-eabi-gdb). See docs/mcu-profile-patch.md.

    patch.py build            host test, compile, build the two flash images, self-checks
    patch.py check            read-only: stock bytes at the hook, patch page erased, FMC locked, WWDGT state
    patch.py program          write the hook code into erased page 53, then page 14 with the new `bl`
    patch.py verify           read back all 128 KiB; must equal stock except the patched bytes
    patch.py restore          page 14 back to stock, page 53 erased (stock image again)
    patch.py inject PROFILE   bench only: push one 0x23A frame (e.g. 11) into the bus-0 ring
    patch.py state            read the hook's diagnostics from SRAM (0x2000F000)

Only `program` and `restore` write flash: two pages (14: 0x08007000, 53:
0x0801a800), never the boot loader, the settings pages or option bytes.
Each SWD session attaches (MCU halts), works, then always locks the flash
controller, restores DBGMCU and detaches, even if a step failed.
"""
import argparse, hashlib, json, os, struct, subprocess, sys, time, glob
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DUMP = ROOT / 'local/stock/gd32-flash.bin'   # the builder's own dump of the MCU's flash (SWD)
DUMP_SHA = '6eb98cdd'
OUT = ROOT / 'backups' / time.strftime('%Y-%m-%d') / 'mcu-profile'
FLASH, SIZE, PAGE = 0x08000000, 0x20000, 0x800
HOOK_SITE, STOCK_GATEWAY = 0x08007264, 0x080051a8
HOOK_ADDR = 0x0801a800
PAGE_HOOK = HOOK_SITE & ~(PAGE - 1)      # page 14
PAGE_CODE = HOOK_ADDR                    # page 53
STATE = 0x2000F000
FMC = 0x40022000
KEY0, STAT0, CTL0, ADDR0 = FMC + 0x04, FMC + 0x0C, FMC + 0x10, FMC + 0x14
DBGMCU_CTL0 = 0xE0042004
RING0 = 0x20000fd0                       # bus-0 receive ring: +2 head, +3 tail, +4 16 x 20-byte frames
CFLAGS = ['-mcpu=cortex-m4', '-mthumb', '-mfloat-abi=soft', '-Os', '-ffreestanding', '-nostdlib',
          '-fno-builtin', '-fno-tree-loop-distribute-patterns', '-Wall', '-Wextra', '-Werror']


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True)


def bl(src, dst):
    """Thumb-2 BL (T1) at src to dst, as 4 bytes."""
    off = dst - (src + 4)
    assert -(1 << 24) <= off < (1 << 24) and off % 2 == 0
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (~(i1 ^ s)) & 1, (~(i2 ^ s)) & 1
    hw1 = 0xF000 | (s << 10) | ((off >> 12) & 0x3ff)
    hw2 = 0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7ff)
    return struct.pack('<HH', hw1, hw2)


def stock():
    d = DUMP.read_bytes()
    if not hashlib.sha256(d).hexdigest().startswith(DUMP_SHA) or len(d) != SIZE:
        sys.exit('stock flash dump missing or different')
    return d


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    d = stock()
    test = OUT / 'profile_test'
    run(['cc', '-std=c11', '-Wall', '-Wextra', '-DHOST_TEST', '-o', str(test), str(HERE / 'test_host.c')])
    print(run([str(test)]).stdout.strip())
    elf = OUT / 'profile.elf'
    run(['arm-none-eabi-gcc', *CFLAGS, '-T', str(HERE / 'link.ld'), '-o', str(elf), str(HERE / 'profile.c')])
    syms = {l.split()[2]: int(l.split()[0], 16) for l in run(['arm-none-eabi-nm', str(elf)]).stdout.splitlines()}
    assert syms['profile_hook'] == HOOK_ADDR, syms
    blob = OUT / 'hook.bin'
    run(['arm-none-eabi-objcopy', '-O', 'binary', '-j', '.text', str(elf), str(blob)])
    code = blob.read_bytes()
    code += b'\xff' * (len(code) % 2)
    assert len(code) <= PAGE
    # self-checks against the stock image
    site = HOOK_SITE - FLASH
    assert bl(HOOK_SITE, STOCK_GATEWAY) == d[site:site + 4], 'BL encoder does not reproduce the stock call'
    assert d[PAGE_CODE - FLASH:PAGE_CODE - FLASH + PAGE] == b'\xff' * PAGE, 'page 53 not erased in stock'
    patched = bytearray(d)
    patched[site:site + 4] = bl(HOOK_SITE, HOOK_ADDR)
    patched[PAGE_CODE - FLASH:PAGE_CODE - FLASH + len(code)] = code
    diff = [i for i in range(SIZE) if patched[i] != d[i]]
    pages = sorted({(FLASH + i) & ~(PAGE - 1) for i in diff})
    assert pages == [PAGE_HOOK, PAGE_CODE], [hex(p) for p in pages]
    (OUT / 'flash-patched.bin').write_bytes(bytes(patched))
    (OUT / 'page14-patched.bin').write_bytes(bytes(patched[PAGE_HOOK - FLASH:PAGE_HOOK - FLASH + PAGE]))
    (OUT / 'page14-stock.bin').write_bytes(d[PAGE_HOOK - FLASH:PAGE_HOOK - FLASH + PAGE])
    meta = {'hook_bytes': len(code), 'hook_sha256': hashlib.sha256(code).hexdigest(),
            'bl_stock': d[site:site + 4].hex(), 'bl_patched': bl(HOOK_SITE, HOOK_ADDR).hex(),
            'changed_bytes': len(diff), 'pages': [hex(p) for p in pages],
            'patched_flash_sha256': hashlib.sha256(bytes(patched)).hexdigest()}
    (OUT / 'build.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(json.dumps(meta, indent=2))


def gdb_port():
    if os.environ.get('AUTOTOP_GDB'):
        return os.environ['AUTOTOP_GDB']
    ports = sorted(glob.glob('/dev/cu.usbmodem*1'))
    if len(ports) != 1:
        sys.exit(f'set AUTOTOP_GDB: probe GDB port not unique: {ports}')
    return ports[0]


def gdb(body, name, timeout=300, flash=False):
    """Attach; source body; detach as a separate -ex step (runs even after an
    error). Flash sessions also hold both watchdogs in DBGMCU while halted and,
    after the body, always lock the flash controller (CTL0 = LK, which clears
    PG/PER) and restore DBGMCU. Read-only sessions touch neither register."""
    OUT.mkdir(parents=True, exist_ok=True)
    script = OUT / f'{name}.gdb'
    script.write_text(body + '\n')
    cmd = ['arm-none-eabi-gdb', '-nx', '-batch',
           '-ex', 'set pagination off', '-ex', 'set confirm off', '-ex', 'set mem inaccessible-by-default off',
           '-ex', f'target extended-remote {gdb_port()}', '-ex', 'monitor swdp_scan', '-ex', 'attach 1']
    if flash:
        cmd += ['-ex', f'set $dbg = *(unsigned int *){DBGMCU_CTL0:#x}',
                '-ex', f'set *(unsigned int *){DBGMCU_CTL0:#x} = $dbg | 0x300']
    cmd += ['-ex', f'source {script}']
    if flash:
        cmd += ['-ex', f'set *(unsigned int *){CTL0:#x} = 0x80',
                '-ex', f'set *(unsigned int *){DBGMCU_CTL0:#x} = $dbg']
    cmd += ['-ex', 'detach', '-ex', 'quit 0']
    t0 = time.monotonic()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    log = OUT / f'{time.strftime("%H%M%S")}-{name}.log'
    log.write_text(r.stdout + '\n--- stderr\n' + r.stderr)
    ok = 'detached' in (r.stdout + r.stderr).lower()
    print(f'{name}: gdb exit {r.returncode}, {time.monotonic() - t0:.1f} s, '
          f'{"detached" if ok else "DETACH NOT CONFIRMED"}, log {log.relative_to(ROOT)}')
    if not ok:
        sys.exit('detach not confirmed: run "patch.py check" (attaches and detaches) and look at the log')
    return r


def words(out):
    v = {}
    for line in out.splitlines():
        if line.startswith('W '):
            _, k, x = line.split()
            v[k] = int(x, 0)
    return v


def check(quiet=False):
    body = '\n'.join([
        f'printf "W site 0x%08x\\n", *(unsigned int *){HOOK_SITE:#x}',
        f'printf "W code0 0x%08x\\n", *(unsigned int *){PAGE_CODE:#x}',
        f'printf "W codeN 0x%08x\\n", *(unsigned int *){PAGE_CODE + PAGE - 4:#x}',
        f'printf "W ctl0 0x%08x\\n", *(unsigned int *){CTL0:#x}',
        f'printf "W stat0 0x%08x\\n", *(unsigned int *){STAT0:#x}',
        'printf "W wwdgt 0x%08x\\n", *(unsigned int *)0x40002C00',
        'printf "W vtor 0x%08x\\n", *(unsigned int *)0xE000ED08',
        f'printf "W magic 0x%08x\\n", *(unsigned int *){STATE:#x}'])
    v = words(gdb(body, 'check').stdout)
    d = stock()
    st = struct.unpack_from('<I', d, HOOK_SITE - FLASH)[0]
    pt = struct.unpack('<I', bl(HOOK_SITE, HOOK_ADDR))[0]
    site = {st: 'stock bl gateway', pt: 'PATCHED bl profile_hook'}.get(v.get('site', -1), 'UNEXPECTED')
    if not quiet:
        print(f"  hook site 0x{v.get('site', 0):08x} ({site})")
        print(f"  page 53 first/last word 0x{v.get('code0', 0):08x} / 0x{v.get('codeN', 0):08x}")
        print(f"  FMC CTL0 0x{v.get('ctl0', 0):08x} ({'locked' if v.get('ctl0', 0) & 0x80 else 'UNLOCKED'}), STAT0 0x{v.get('stat0', 0):08x}")
        print(f"  WWDGT CTL 0x{v.get('wwdgt', 0):08x} ({'enabled' if v.get('wwdgt', 0) & 0x80 else 'disabled'}), VTOR 0x{v.get('vtor', 0):08x}")
    return v, site


def program_lines(base, data, erase_page=None):
    """GDB lines: unlock, clear flags, optional page erase, halfword program, report STAT0."""
    L = [f'set *(unsigned int *){KEY0:#x} = 0x45670123', f'set *(unsigned int *){KEY0:#x} = 0xCDEF89AB',
         f'set *(unsigned int *){STAT0:#x} = 0x34']
    if erase_page is not None:
        L += [f'set *(unsigned int *){CTL0:#x} = 0x2', f'set *(unsigned int *){ADDR0:#x} = {erase_page:#x}',
              f'set *(unsigned int *){CTL0:#x} = 0x42',
              f'while (*(unsigned int *){STAT0:#x} & 1)', 'end',
              f'printf "W erase_stat 0x%08x\\n", *(unsigned int *){STAT0:#x}',
              f'set *(unsigned int *){STAT0:#x} = 0x34']
    L.append(f'set *(unsigned int *){CTL0:#x} = 0x1')
    for i in range(0, len(data), 2):
        hw = data[i] | data[i + 1] << 8
        if hw == 0xffff:
            continue
        L += [f'set *(unsigned short *){base + i:#x} = {hw:#06x}', f'while (*(unsigned int *){STAT0:#x} & 1)', 'end']
    L += [f'printf "W prog_stat 0x%08x\\n", *(unsigned int *){STAT0:#x}', f'set *(unsigned int *){CTL0:#x} = 0x80']
    return L


def program():
    v, site = check()
    if site != 'stock bl gateway' or v.get('code0') != 0xffffffff or v.get('codeN') != 0xffffffff:
        sys.exit('not the expected stock state: nothing written')
    code = (OUT / 'hook.bin').read_bytes()
    code += b'\xff' * (len(code) % 2)
    # 1) hook code into erased page 53: unreferenced until step 2, so harmless if interrupted
    rb = OUT / 'readback-code.bin'
    r = gdb('\n'.join(program_lines(HOOK_ADDR, code) + [f'dump binary memory {rb} {HOOK_ADDR:#x} {HOOK_ADDR + len(code):#x}']), 'program-code', flash=True)
    w = words(r.stdout)
    if w.get('prog_stat', 1) & 0x14 or rb.read_bytes() != code:
        sys.exit(f'hook code did not program cleanly (STAT0 0x{w.get("prog_stat", 0):08x}); page 14 untouched')
    print(f'  hook code: {len(code)} bytes programmed and read back identical')
    # 2) page 14: erase and program with the patched bl (the only erase)
    page = (OUT / 'page14-patched.bin').read_bytes()
    rb = OUT / 'readback-page14.bin'
    r = gdb('\n'.join(program_lines(PAGE_HOOK, page, erase_page=PAGE_HOOK) +
                      [f'dump binary memory {rb} {PAGE_HOOK:#x} {PAGE_HOOK + PAGE:#x}']), 'program-page14', flash=True)
    w = words(r.stdout)
    if rb.read_bytes() != page:
        sys.exit(f'PAGE 14 READ-BACK DIFFERS (erase 0x{w.get("erase_stat", 0):08x}, program 0x{w.get("prog_stat", 0):08x}): '
                 'run "patch.py restore" before any reset')
    print(f'  page 14 erased and programmed, read back identical (erase STAT0 0x{w.get("erase_stat", 0):08x}, '
          f'program STAT0 0x{w.get("prog_stat", 0):08x})')
    print('  patch in flash; it runs from the next MCU reset or power cycle (verify first)')


def verify():
    rb = OUT / 'readback-flash.bin'
    gdb(f'dump binary memory {rb} {FLASH:#x} {FLASH + SIZE:#x}', 'verify', timeout=600)
    got, d = rb.read_bytes(), stock()
    patched = (OUT / 'flash-patched.bin').read_bytes()
    # settings pages (0x0801f000, 0x0801f800) may change at run time: compare the rest
    skip = range(0x1f000, 0x20000)
    same = lambda a, b: all(a[i] == b[i] for i in range(SIZE) if i not in skip)
    state = 'PATCHED image' if same(got, patched) else 'STOCK image' if same(got, d) else 'NEITHER stock nor patched'
    print(f'  flash read back ({len(got)} bytes, settings pages excluded): {state}')
    print(f'  settings pages vs dump: {"unchanged" if got[0x1f000:] == d[0x1f000:] else "changed (normal after settings writes)"}')


def restore():
    d = stock()
    page = d[PAGE_HOOK - FLASH:PAGE_HOOK - FLASH + PAGE]
    rb = OUT / 'restore-page14.bin'
    gdb('\n'.join(program_lines(PAGE_HOOK, page, erase_page=PAGE_HOOK) +
                [f'dump binary memory {rb} {PAGE_HOOK:#x} {PAGE_HOOK + PAGE:#x}']), 'restore-page14', flash=True)
    if rb.read_bytes() != page:
        sys.exit('page 14 restore read-back differs: retry restore')
    print('  page 14 back to stock (read back identical)')
    rb = OUT / 'restore-page53.bin'
    gdb('\n'.join(program_lines(PAGE_CODE, b'', erase_page=PAGE_CODE) +
                  [f'dump binary memory {rb} {PAGE_CODE:#x} {PAGE_CODE + PAGE:#x}']), 'restore-page53', flash=True)
    print('  page 53 erased' if rb.read_bytes() == b'\xff' * PAGE else '  page 53 NOT fully erased: retry restore')


def inject(profile):
    """Bench test: one standard 0x23A frame, data <profile> ff 00 ff ff fc ff ff, into the bus-0 ring."""
    p = int(profile, 16)
    frame = struct.pack('<IIBBB8sB', 0x23A, 0, 0, 0, 8, bytes([p, 0xff, 0x00, 0xff, 0xff, 0xfc, 0xff, 0xff]), 0)
    assert len(frame) == 20
    lines = ['set $h = *(unsigned char *)' + hex(RING0 + 2)]
    for i in range(0, 20, 4):
        w = struct.unpack_from('<I', frame, i)[0]
        lines.append(f'set *(unsigned int *)({RING0 + 4:#x} + $h * 20 + {i}) = {w:#010x}')
    lines += ['if $h >= 15', f'  set *(unsigned char *){RING0 + 2:#x} = 0', 'else',
              f'  set *(unsigned char *){RING0 + 2:#x} = $h + 1', 'end',
              'printf "W head 0x%02x\\n", $h']
    v = words(gdb('\n'.join(lines), f'inject-{profile}').stdout)
    print(f'  0x23A frame with profile {profile} placed in ring slot {v.get("head")}; the main loop takes it on resume')


def state():
    f = OUT / 'state.bin'
    gdb(f'dump binary memory {f} {STATE:#x} {STATE + 44:#x}', 'state')
    raw = f.read_bytes()
    names = ['magic', 'version', 'last_frame', 'session_start', 'sessions', 'last_report', 'reports', 'seen', 'latency', 'frames']
    v = dict(zip(names, struct.unpack_from('<10I', raw, 0)))
    v['profile'], v['fresh'], v['reported_profile'], v['reported_fresh'] = raw[40:44]
    if v['magic'] != 0x50524f46:
        print('  hook state not initialised (the hook has not run since power-on)')
    else:
        print('  ' + ', '.join(f'{k} {v[k]:#x}' if k.endswith('profile') else f'{k} {v[k]}' for k in names[1:] + ['profile', 'fresh', 'reported_profile', 'reported_fresh']))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for c in ('build', 'check', 'program', 'verify', 'restore', 'state'):
        sub.add_parser(c)
    sub.add_parser('inject').add_argument('profile', choices=['00', '11', '22', 'aa'])
    a = ap.parse_args()
    if a.cmd == 'inject':
        inject(a.profile)
    else:
        {'build': build, 'check': check, 'program': program, 'verify': verify, 'restore': restore, 'state': state}[a.cmd]()


if __name__ == '__main__':
    main()
