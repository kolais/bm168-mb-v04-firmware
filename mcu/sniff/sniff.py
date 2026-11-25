#!/usr/bin/env python3
"""RAM-only CAN recorder for the unit's GD32 MCU, driven over SWD (Black Magic
Probe + arm-none-eabi-gdb). See docs/mcu-can-sniffer.md.

    sniff.py build                 compile, host-test, build the RAM image
    sniff.py check                 read-only: VTOR, stock handler words, SRAM size
    sniff.py install               write the RAM image, verify, then point VTOR at it
    sniff.py snapshot NAME         read header + table, save NAME.bin/.json
    sniff.py clear                 ask the recorder to clear masks and counters
    sniff.py remove                point VTOR back at the flash vector table
    sniff.py resume                attach and detach only (resumes a halted MCU)
    sniff.py diff A B [--noise N]  compare two snapshots (N: snapshot whose masks are noise)

Every SWD command attaches (the MCU halts, about a second), does its work and
detaches (the MCU resumes). Nothing is ever written to flash, option bytes or
peripheral registers other than VTOR. Outputs are private (backups/).
"""
import argparse, json, struct, subprocess, sys, glob, os, hashlib, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DUMP = ROOT / 'local/stock/gd32-flash.bin'   # the builder's own dump of the MCU's flash (SWD)
DUMP_SHA = '6eb98cdd'
OUT = ROOT / 'backups' / time.strftime('%Y-%m-%d') / 'mcu-sniff'
APP = 0x08002000
VTOR = 0xE000ED08
RAM_VEC, RAM_CODE, RAM_HDR, RAM_TABLE = 0x20005000, 0x20005200, 0x20005700, 0x20005800
NSLOT, SLOT, HDR = 1024, 24, 64
VEC_WORDS = 128
IRQ_CAN0_RX0, IRQ_CAN1_RX0 = 36, 80          # vector indices
STOCK = {IRQ_CAN0_RX0: 0x08005199, IRQ_CAN1_RX0: 0x080051a1}
STOCK_WORD = 0xf00cb510                       # push {r4,lr}; bl ... at both handlers
CFLAGS = ['-mcpu=cortex-m4', '-mthumb', '-mfloat-abi=soft', '-Os', '-ffreestanding', '-nostdlib',
          '-fno-builtin', '-fno-tree-loop-distribute-patterns', '-Wall', '-Wextra', '-Werror']


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    dump = DUMP.read_bytes()
    if not hashlib.sha256(dump).hexdigest().startswith(DUMP_SHA):
        sys.exit('flash dump hash differs')
    test = OUT / 'sniff_test'
    run(['cc', '-std=c11', '-Wall', '-Wextra', '-DHOST_TEST', '-o', str(test), str(HERE / 'test_host.c')])
    print(run([str(test)]).stdout.strip())
    elf = OUT / 'sniff.elf'
    run(['arm-none-eabi-gcc', *CFLAGS, '-T', str(HERE / 'link.ld'), '-o', str(elf), str(HERE / 'sniff.c')])
    syms = {}
    for line in run(['arm-none-eabi-nm', str(elf)]).stdout.splitlines():
        a, _, n = line.split()
        syms[n] = int(a, 16)
    assert syms['hdr'] == RAM_HDR and syms['table'] == RAM_TABLE, syms
    text = OUT / 'sniff.text'
    run(['arm-none-eabi-objcopy', '-O', 'binary', '-j', '.text', str(elf), str(text)])
    code = text.read_bytes()
    assert len(code) <= RAM_HDR - RAM_CODE
    vec = list(struct.unpack_from(f'<{VEC_WORDS}I', dump, APP - 0x08000000))
    for irq, stock in STOCK.items():
        assert vec[irq] == stock, (irq, hex(vec[irq]))
    vec[IRQ_CAN0_RX0] = syms['sniff_can0_rx0'] | 1
    vec[IRQ_CAN1_RX0] = syms['sniff_can1_rx0'] | 1
    image = struct.pack(f'<{VEC_WORDS}I', *vec)
    image += b'\0' * (RAM_CODE - RAM_VEC - len(image)) + code
    image += b'\0' * (RAM_HDR - RAM_VEC - len(image)) + b'\0' * HDR
    (OUT / 'sniff-ram.bin').write_bytes(image)
    meta = {'base': hex(RAM_VEC), 'bytes': len(image), 'sha256': hashlib.sha256(image).hexdigest(),
            'hooks': {k: hex(v) for k, v in syms.items() if k.startswith('sniff_')}, 'code_bytes': len(code)}
    (OUT / 'sniff-ram.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(json.dumps(meta, indent=2))


def gdb_port():
    if os.environ.get('AUTOTOP_GDB'):
        return os.environ['AUTOTOP_GDB']
    ports = sorted(glob.glob('/dev/cu.usbmodem*1'))
    if len(ports) != 1:
        sys.exit(f'set AUTOTOP_GDB: probe GDB port not unique: {ports}')
    return ports[0]


def gdb(body, name):
    """Attach, source body (GDB CLI), then detach as a separate -ex step: GDB runs
    every -ex even after an error in an earlier one, so a failing body cannot
    leave the MCU halted. The detach is confirmed from GDB's output."""
    OUT.mkdir(parents=True, exist_ok=True)
    script = OUT / f'{name}.gdb'
    script.write_text(body + '\n')
    cmd = ['arm-none-eabi-gdb', '-nx', '-batch',
           '-ex', 'set pagination off', '-ex', 'set confirm off', '-ex', 'set mem inaccessible-by-default off',
           '-ex', f'target extended-remote {gdb_port()}', '-ex', 'monitor swdp_scan', '-ex', 'attach 1',
           '-ex', f'source {script}', '-ex', 'detach', '-ex', 'quit 0']
    t0 = time.monotonic()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    log = OUT / f'{time.strftime("%H%M%S")}-{name}.log'
    log.write_text(r.stdout + '\n--- stderr\n' + r.stderr)
    detached = 'detached' in (r.stdout + r.stderr).lower()
    print(f'{name}: gdb exit {r.returncode}, {time.monotonic() - t0:.1f} s, '
          f'{"detached" if detached else "DETACH NOT CONFIRMED - run: sniff.py resume"}, log {log.relative_to(ROOT)}')
    return r


def resume():
    """Attach and detach only: resumes a halted MCU."""
    gdb('printf "W pc 0x%08x\\n", $pc', 'resume')


def words(out):
    vals = {}
    for line in out.splitlines():
        if line.startswith('W '):
            _, k, v = line.split()
            vals[k] = int(v, 0)
    return vals


PROBE = '\n'.join([
    'printf "W vtor 0x%08x\\n", *(unsigned *)0xE000ED08',
    'printf "W h0 0x%08x\\n", *(unsigned *)0x08005198',
    'printf "W h1 0x%08x\\n", *(unsigned *)0x080051a0',
    f'printf "W v36 0x%08x\\n", *(unsigned *)({APP} + 4 * {IRQ_CAN0_RX0})',
    f'printf "W v80 0x%08x\\n", *(unsigned *)({APP} + 4 * {IRQ_CAN1_RX0})',
    'printf "W idsize 0x%08x\\n", *(unsigned *)0x1FFFF7E0',
    'printf "W msp 0x%08x\\n", $sp',
    'printf "W pc 0x%08x\\n", $pc',
    f'printf "W rv36 0x%08x\\n", *(unsigned *)({RAM_VEC} + 4 * {IRQ_CAN0_RX0})',
    f'printf "W magic 0x%08x\\n", *(unsigned *){RAM_HDR}',
])


def stock_ok(v):
    return (v.get('h0') == STOCK_WORD and v.get('h1') == STOCK_WORD and
            v.get('v36') == STOCK[IRQ_CAN0_RX0] and v.get('v80') == STOCK[IRQ_CAN1_RX0])


def check():
    v = words(gdb(PROBE, 'check').stdout)
    for k, x in v.items():
        print(f'  {k:6s} 0x{x:08x}')
    if 'idsize' in v:
        print(f'  flash {v["idsize"] & 0xffff} KiB, SRAM {v["idsize"] >> 16} KiB')
    print('  stock handlers and vectors:', 'as expected' if stock_ok(v) else 'DIFFERENT')
    print('  VTOR:', {APP: 'flash (stock)', RAM_VEC: 'RAM (recorder active)'}.get(v.get('vtor', 0), 'unexpected'))
    return v


def install():
    image = (OUT / 'sniff-ram.bin').read_bytes()
    v = check()
    if not stock_ok(v) or v.get('vtor') != APP:
        sys.exit('not the expected stock state: nothing written')
    if (v.get('idsize', 0) >> 16) and (v['idsize'] >> 16) * 1024 < RAM_TABLE + NSLOT * SLOT - 0x20000000:
        sys.exit('SRAM smaller than the table needs: nothing written')
    if not (0x20003400 <= v.get('msp', 0) <= 0x20004a90):
        sys.exit('stack pointer outside the stock stack: nothing written')
    rb = OUT / 'readback.bin'
    body = '\n'.join([
        f'if *(unsigned *)0xE000ED08 != {APP}', '  echo ABORT vtor\\n', '  detach', '  quit 2', 'end',
        f'restore {OUT / "sniff-ram.bin"} binary {RAM_VEC}',
        f'dump binary memory {rb} {RAM_VEC} {RAM_VEC + len(image)}'])
    r = gdb(body, 'install-write')
    if r.returncode != 0 or 'ABORT' in r.stdout:
        sys.exit('write step aborted; VTOR untouched')
    if rb.read_bytes() != image:
        sys.exit('read-back differs from the image; VTOR untouched (RAM above the stock stack only)')
    print('  RAM image written and read back identical')
    body = '\n'.join([
        f'if *(unsigned *)0xE000ED08 != {APP}', '  echo ABORT vtor\\n', '  detach', '  quit 2', 'end',
        f'if *(unsigned *)({RAM_VEC} + 4 * {IRQ_CAN0_RX0}) != {struct.unpack_from("<I", image, 4 * IRQ_CAN0_RX0)[0]}',
        '  echo ABORT ram\\n', '  detach', '  quit 2', 'end',
        f'set *(unsigned *)0xE000ED08 = {RAM_VEC}',
        'printf "W vtor 0x%08x\\n", *(unsigned *)0xE000ED08'])
    r = gdb(body, 'install-vtor')
    v = words(r.stdout)
    if r.returncode != 0 or v.get('vtor') != RAM_VEC:
        sys.exit(f'VTOR not switched (exit {r.returncode}); run "check"')
    print('  VTOR -> RAM vector table: recorder active')


def remove():
    body = '\n'.join([f'if *(unsigned *)0xE000ED08 == {RAM_VEC}', f'  set *(unsigned *)0xE000ED08 = {APP}', 'end',
                      'printf "W vtor 0x%08x\\n", *(unsigned *)0xE000ED08'])
    v = words(gdb(body, 'remove').stdout)
    print('  VTOR:', hex(v.get('vtor', 0)), '(stock)' if v.get('vtor') == APP else '(CHECK)')


def clear():
    body = '\n'.join([f'if *(unsigned *)0xE000ED08 == {RAM_VEC}', f'  set *(unsigned *)({RAM_HDR} + 16) = 1', 'end',
                      f'printf "W req 0x%08x\\n", *(unsigned *)({RAM_HDR} + 16)'])
    v = words(gdb(body, 'clear').stdout)
    print('  clear requested' if v.get('req') == 1 else '  recorder not active: nothing done')


def decode(raw):
    h = struct.unpack_from('<16I', raw, 0)
    head = dict(zip(['magic', 'version', 'frames', 'full', 'clear_req', 'clears', 'nslot', 'used',
                     'ext0', 'ext1', 'bus0', 'bus1'], h[:12]))
    slots = []
    for i in range(NSLOT):
        off = RAM_TABLE - RAM_HDR + i * SLOT  # raw starts at the header
        key, dlc, used = struct.unpack_from('<HBB', raw, off)
        if not used:
            continue
        data = raw[off + 4:off + 12]
        mask = raw[off + 12:off + 20]
        frames, changes = struct.unpack_from('<HH', raw, off + 20)
        slots.append({'bus': (key >> 14) & 1, 'ext': key >> 15, 'id': key & (0x3fff if key >> 15 else 0x7ff),
                      'dlc': dlc, 'data': data[:max(dlc, 0)].hex(), 'mask': mask[:max(dlc, 0)].hex(),
                      'frames': frames, 'changes': changes})
    slots.sort(key=lambda s: (s['bus'], s['ext'], s['id']))
    return head, slots


def snapshot(name):
    raw = OUT / f'{name}.bin'
    body = f'dump binary memory {raw} {RAM_HDR} {RAM_TABLE + NSLOT * SLOT}'
    r = gdb(body, f'snapshot-{name}')
    if r.returncode != 0 or not raw.exists():
        sys.exit('snapshot failed')
    head, slots = decode(raw.read_bytes())
    (OUT / f'{name}.json').write_text(json.dumps({'header': head, 'slots': slots}, indent=1) + '\n')
    ok = head['magic'] == 0x534e4631
    print(f'  recorder {"active" if ok else "NOT initialised"}: frames {head["frames"]} (bus0 {head["bus0"]}, '
          f'bus1 {head["bus1"]}), IDs {head["used"]}, dropped {head["full"]}, clears {head["clears"]}')


def diff(a, b, noise=None):
    load = lambda n: {(s['bus'], s['ext'], s['id']): s for s in json.loads((OUT / f'{n}.json').read_text())['slots']}
    A, B = load(a), load(b)
    N = load(noise) if noise else {}
    def noisy(k):
        s = N.get(k)
        return bytes.fromhex(s['mask']) if s else b''
    print(f'IDs only in {b}:', ['%d:%03x' % (k[0], k[2]) for k in sorted(set(B) - set(A))])
    print(f'IDs only in {a}:', ['%d:%03x' % (k[0], k[2]) for k in sorted(set(A) - set(B))])
    for k in sorted(set(A) & set(B)):
        da, db = bytes.fromhex(A[k]['data']), bytes.fromhex(B[k]['data'])
        nm = noisy(k)
        diffs = [i for i in range(min(len(da), len(db))) if da[i] != db[i] and not (i < len(nm) and nm[i])]
        if diffs or len(da) != len(db):
            print(f'bus{k[0]} {k[2]:03x}: ' + ' '.join(f'b{i} {da[i]:02x}->{db[i]:02x}' for i in diffs) +
                  f'  (frames {B[k]["frames"]}, changes {B[k]["changes"]})')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for c in ('build', 'check', 'install', 'clear', 'remove', 'resume'):
        sub.add_parser(c)
    s = sub.add_parser('snapshot'); s.add_argument('name')
    d = sub.add_parser('diff'); d.add_argument('a'); d.add_argument('b'); d.add_argument('--noise')
    a = ap.parse_args()
    {'build': build, 'check': check, 'install': install, 'clear': clear, 'remove': remove, 'resume': resume}.get(a.cmd, lambda: None)()
    if a.cmd == 'snapshot':
        snapshot(a.name)
    elif a.cmd == 'diff':
        diff(a.a, a.b, a.noise)


if __name__ == '__main__':
    main()
