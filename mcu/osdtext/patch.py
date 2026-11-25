#!/usr/bin/env python3
"""The MCU's "BMW / Check Signal" text, the right way round (docs/mcu-osd-text.md).

    patch.py build       host test, compile the RAM page writer, build page 33 and the expected flash image
    patch.py check       read-only: the six words to change, the type flag, the spare page, the flash controller
    patch.py rehearse    the page writer on the erased spare page 54: write, read back, erase, read back
    patch.py program     page 33 with the forward text, written by the page writer in RAM
    patch.py restore     page 33 as it was
    patch.py verify      read back all 128 KiB and say which image it is

`rehearse`, `program` and `restore` write flash: one page each, never the boot
loader, the settings pages or option bytes. The page is erased and programmed
by the MCU itself from its RAM (loader.c): no SWD transfer happens while the
page is incomplete. If the page could not be made right, the MCU is left in a
loop in RAM, not in its firmware.
"""
import argparse, hashlib, importlib.util, json, struct, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'backups' / time.strftime('%Y-%m-%d') / 'mcu-osdtext'
spec = importlib.util.spec_from_file_location('profile_patch', ROOT / 'mcu/profile/patch.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
profile.OUT = OUT                       # its read-only sessions log here

FLASH, SIZE, PAGE = 0x08000000, 0x20000, 0x800
PAGE_TEXT = 0x08010800                  # page 33: the three places that draw the text
PAGE_SPARE = 0x0801b000                 # page 54: erased, referenced by nothing
FLAG = 0x2000049c                       # the firmware's type flag: 1 forward text, else reversed; never written
FORWARD, REVERSED = 0x0801421d, 0x08014210   # "Check Signal", "langiS kcehC"
SITES = (0x080109d4, 0x08010cec, 0x08010dec)  # pointer to the reversed text; "WMB" follows each
PROFILE_SHA = 'fd0e2930e7405e7dd41fcb0d1b6d1a6ba103724712cb8ca36a21cf10361b445d'  # flash as programmed 2026-11-21
RAM, BUF, STACK = 0x2000e000, 0x2000e400, 0x2000eff0
CFLAGS = ['-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft', '-Os', '-ffreestanding', '-nostdlib',
          '-fno-builtin', '-fno-tree-loop-distribute-patterns', '-Wall', '-Wextra', '-Werror']
STATUS = ['ok', 'checksum of the data in RAM wrong (nothing touched)', 'page refused (nothing touched)',
          'flash controller stayed locked (nothing touched)', 'erase error', 'page not erased after the erase',
          'program error', 'read-back differs', 'flash controller stayed busy']


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True)


def loader_sum(data):
    a, b = 1, 0
    for i in range(0, len(data), 2):
        a = (a + (data[i] | data[i + 1] << 8)) & 0xffffffff
        b = (b + a) & 0xffffffff
    return ((b << 16) ^ a) & 0xffffffff


def images():
    """(flash before this patch, flash after it): stock plus the profile patch, and that with page 33 changed."""
    before = (OUT / 'flash-before.bin').read_bytes()
    return before, (OUT / 'flash-patched.bin').read_bytes()


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    test = OUT / 'loader_test'
    run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-o', str(test), str(HERE / 'test_host.c')])
    print(run([str(test)]).stdout.strip())
    elf = OUT / 'loader.elf'
    run(['arm-none-eabi-gcc', *CFLAGS, '-T', str(HERE / 'link.ld'), '-o', str(elf), str(HERE / 'loader.c')])
    syms = {l.split()[2]: int(l.split()[0], 16) for l in run(['arm-none-eabi-nm', str(elf)]).stdout.splitlines()}
    assert syms['entry'] == RAM, syms
    blob = OUT / 'loader.bin'
    run(['arm-none-eabi-objcopy', '-O', 'binary', '-j', '.text', str(elf), str(blob)])
    code = blob.read_bytes()
    assert len(code) <= BUF - RAM, len(code)
    dis = run(['arm-none-eabi-objdump', '-d', str(elf)]).stdout
    bkpt = [int(l.split(':')[0], 16) for l in dis.splitlines() if '\tbkpt\t' in l]
    assert len(bkpt) == 1 and RAM < bkpt[0] < syms['park'], bkpt
    # the flash as it is in the MCU: stock with the profile patch of 2026-11-21
    profile.print = lambda *_a, **_k: None                  # its own report is not wanted here
    try:
        profile.build()
    finally:
        del profile.print
    before = (OUT / 'flash-patched.bin').read_bytes()       # written by the profile build
    assert hashlib.sha256(before).hexdigest() == PROFILE_SHA, 'the profile patch no longer builds to what is in the MCU'
    (OUT / 'flash-before.bin').write_bytes(before)
    after = bytearray(before)
    for site in SITES:
        o = site - FLASH
        assert struct.unpack_from('<I', before, o - 8)[0] == FORWARD and before[o - 4:o] == b'BMW\0', hex(site)
        assert struct.unpack_from('<I', before, o)[0] == REVERSED and before[o + 4:o + 8] == b'WMB\0', hex(site)
        struct.pack_into('<I', after, o, FORWARD)
        after[o + 4:o + 8] = b'BMW\0'
    for text, at in ((b'Check Signal\0', FORWARD), (b'langiS kcehC\0', REVERSED)):
        assert before[at - FLASH:at - FLASH + len(text)] == text
    diff = [i for i in range(SIZE) if after[i] != before[i]]
    assert len(diff) == 9 and {(FLASH + i) & ~(PAGE - 1) for i in diff} == {PAGE_TEXT}, len(diff)
    assert before[PAGE_SPARE - FLASH:PAGE_SPARE - FLASH + PAGE] == b'\xff' * PAGE
    (OUT / 'flash-patched.bin').write_bytes(bytes(after))
    o = PAGE_TEXT - FLASH
    (OUT / 'page33-patched.bin').write_bytes(bytes(after[o:o + PAGE]))
    (OUT / 'page33-before.bin').write_bytes(before[o:o + PAGE])
    meta = {'loader_bytes': len(code), 'loader_sha256': hashlib.sha256(code).hexdigest(),
            'entry': hex(RAM), 'bkpt': hex(bkpt[0]), 'park': hex(syms['park']),
            'changed_bytes': len(diff), 'changed_at': [hex(FLASH + i) for i in diff],
            'page33_before_sha256': hashlib.sha256(before[o:o + PAGE]).hexdigest(),
            'page33_patched_sha256': hashlib.sha256(bytes(after[o:o + PAGE])).hexdigest(),
            'patched_flash_sha256': hashlib.sha256(bytes(after)).hexdigest()}
    (OUT / 'build.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(json.dumps(meta, indent=2))


def meta():
    return json.loads((OUT / 'build.json').read_text())


def check(quiet=False):
    lines = [f'printf "W ptr{i} 0x%08x\\n", *(unsigned int *){s:#x}\nprintf "W txt{i} 0x%08x\\n", *(unsigned int *){s + 4:#x}'
             for i, s in enumerate(SITES)]
    lines += [f'printf "W flag 0x%02x\\n", *(unsigned char *){FLAG:#x}',
              f'printf "W spare0 0x%08x\\n", *(unsigned int *){PAGE_SPARE:#x}',
              f'printf "W spareN 0x%08x\\n", *(unsigned int *){PAGE_SPARE + PAGE - 4:#x}',
              f'printf "W ctl0 0x%08x\\n", *(unsigned int *){profile.CTL0:#x}',
              f'printf "W hook 0x%08x\\n", *(unsigned int *){profile.HOOK_SITE:#x}',
              'printf "W xpsr 0x%08x\\n", $xpsr', 'printf "W pc 0x%08x\\n", $pc']
    v = profile.words(profile.gdb('\n'.join(lines), 'check').stdout)
    wmb, bmw = struct.unpack('<I', b'WMB\0')[0], struct.unpack('<I', b'BMW\0')[0]
    text = ('REVERSED (as shipped)' if all(v.get(f'ptr{i}') == REVERSED and v.get(f'txt{i}') == wmb for i in range(3)) else
            'FORWARD (patched)' if all(v.get(f'ptr{i}') == FORWARD and v.get(f'txt{i}') == bmw for i in range(3)) else 'UNEXPECTED')
    if not quiet:
        print(f"  the three text places: {text}")
        print(f"  type flag at {FLAG:#x}: {v.get('flag')}")
        print(f"  spare page 54 first/last word 0x{v.get('spare0', 0):08x} / 0x{v.get('spareN', 0):08x}")
        print(f"  FMC CTL0 0x{v.get('ctl0', 0):08x} ({'locked' if v.get('ctl0', 0) & 0x80 else 'UNLOCKED'}); "
              f"profile hook site 0x{v.get('hook', 0):08x}; halted at pc 0x{v.get('pc', 0):08x}, exception {v.get('xpsr', 0) & 0x1ff}")
    return v, text


def write_page(name, page, data):
    """One page by the RAM page writer, in one debugger session. data None: erase only.
    Returns True when the page reads back as wanted and the firmware was resumed."""
    m = meta()
    want = data if data is not None else b'\xff' * PAGE
    ram = (OUT / 'loader.bin').read_bytes().ljust(BUF - RAM, b'\0') + want
    ramf, rrb, prb = OUT / f'{name}-ram.bin', OUT / f'{name}-ram-readback.bin', OUT / f'{name}-page-readback.bin'
    ramf.write_bytes(ram)
    for f in (rrb, prb):
        f.unlink(missing_ok=True)
    regs = [f'r{i}' for i in range(13)] + ['sp', 'lr', 'pc', 'xpsr']
    session = [f'set $dbg = *(unsigned int *){profile.DBGMCU_CTL0:#x}',
               f'set *(unsigned int *){profile.DBGMCU_CTL0:#x} = $dbg | 0x300',
               *[f'set $s_{r} = ${r}' for r in regs], 'set $saved = 1',
               'printf "W pc 0x%08x\\n", $s_pc', 'printf "W xpsr 0x%08x\\n", $s_xpsr',
               f'printf "W ctl_before 0x%08x\\n", *(unsigned int *){profile.CTL0:#x}',
               f'restore {ramf} binary {RAM:#x}',
               f'dump binary memory {rrb} {RAM:#x} {RAM + len(ram):#x}',
               f'shell cmp -s {rrb} {ramf}',
               'if $_shell_exitcode != 0', '  printf "W ram_mismatch 1\\n"', 'else',
               '  set $try = 0', '  while $try < 3 && $done == 0', '    set $started = 1',
               f'    set $r0 = {page:#x}', f'    set $r1 = {BUF if data is not None else 0:#x}',
               f'    set $r2 = {PAGE // 2 if data is not None else 0}',
               f'    set $r3 = {loader_sum(want) if data is not None else 0:#x}',
               f'    set $sp = {STACK:#x}', f'    set $lr = {int(m["park"], 16) | 1:#x}',
               '    set $xpsr = ($s_xpsr & 0xf9ff03ff) | 0x01000000', f'    set $pc = {RAM:#x}',
               '    continue',
               '    printf "W status%d 0x%08x\\n", $try, $r0', '    printf "W stat%d 0x%08x\\n", $try, $r1',
               '    printf "W end%d 0x%08x\\n", $try, $pc',
               f'    if $r0 == 0 && $pc == {m["bkpt"]}', '      set $done = 1', '    end',
               '    set $try = $try + 1', '  end', 'end',
               'if $done == 1', f'  dump binary memory {prb} {page:#x} {page + PAGE:#x}', 'end',
               f'printf "W ctl_after 0x%08x\\n", *(unsigned int *){profile.CTL0:#x}']
    # Sourced after the session even if a command in it failed: the firmware goes on only if
    # nothing was started or the page is right; otherwise the MCU is parked in RAM.
    finish = ['if $saved == 1', '  if $started == 0 || $done == 1',
              *[f'    set ${r} = $s_{r}' for r in regs], '    printf "W resumed 1\\n"', '  else',
              f'    set $sp = {STACK:#x}', f'    set $pc = {m["park"]}', '    printf "W parked 1\\n"', '  end',
              f'  set *(unsigned int *){profile.DBGMCU_CTL0:#x} = $dbg', 'end']
    sf, ff = OUT / f'{name}.gdb', OUT / f'{name}-finish.gdb'
    sf.write_text('\n'.join(session) + '\n')
    ff.write_text('\n'.join(finish) + '\n')
    cmd = ['arm-none-eabi-gdb', '-nx', '-batch', '-ex', 'set pagination off', '-ex', 'set confirm off',
           '-ex', 'set mem inaccessible-by-default off', '-ex', 'set $saved = 0', '-ex', 'set $started = 0',
           '-ex', 'set $done = 0', '-ex', f'target extended-remote {profile.gdb_port()}', '-ex', 'monitor swdp_scan',
           '-ex', 'attach 1', '-ex', f'source {sf}', '-ex', f'source {ff}', '-ex', 'detach', '-ex', 'quit 0']
    t0 = time.monotonic()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    log = OUT / f'{time.strftime("%H%M%S")}-{name}.log'
    log.write_text(r.stdout + '\n--- stderr\n' + r.stderr)
    w = profile.words(r.stdout)
    print(f'{name}: {time.monotonic() - t0:.1f} s, log {log.relative_to(ROOT)}')
    for t in range(3):
        if f'status{t}' in w:
            s = w[f'status{t}']
            print(f"  run {t + 1}: {STATUS[s] if s < len(STATUS) else hex(s)}, FMC status 0x{w.get(f'stat{t}', 0):08x}, "
                  f"ended at 0x{w.get(f'end{t}', 0):08x}")
    if w.get('ram_mismatch'):
        print('  the page writer did not arrive in RAM whole: nothing run, nothing written')
    good = prb.exists() and prb.read_bytes() == want
    state = 'firmware resumed' if w.get('resumed') else 'MCU PARKED in RAM (not in its firmware)' if w.get('parked') else 'END STATE NOT CONFIRMED'
    print(f"  page {page:#x}: {'reads back as wanted' if good else 'NOT confirmed'}; flash controller "
          f"{'locked' if w.get('ctl_after', 0) & 0x80 else 'NOT confirmed locked'}; {state}")
    return good and bool(w.get('resumed'))


def rehearse():
    v, _ = check()
    if v.get('spare0') != 0xffffffff or v.get('spareN') != 0xffffffff or not v.get('ctl0', 0) & 0x80:
        sys.exit('the spare page is not erased or the flash controller is not locked: nothing written')
    data = (OUT / 'page33-patched.bin').read_bytes()
    if not write_page('rehearse-write', PAGE_SPARE, data):
        sys.exit('rehearsal write did not end well: stop here')
    if not write_page('rehearse-erase', PAGE_SPARE, None):
        sys.exit('rehearsal erase did not end well: stop here')
    print('  rehearsal passed: the spare page was written, read back, and is erased again')


def program(restore=False):
    v, text = check()
    want = 'FORWARD (patched)' if restore else 'REVERSED (as shipped)'
    if text != want or not v.get('ctl0', 0) & 0x80:
        sys.exit(f'not the expected state ({text}): nothing written')
    name = 'page33-before.bin' if restore else 'page33-patched.bin'
    if not write_page('restore' if restore else 'program', PAGE_TEXT, (OUT / name).read_bytes()):
        sys.exit('PAGE 33 NOT CONFIRMED: do not power-cycle; run "patch.py check" and decide from its log')
    print('  page 33 written; the text is drawn anew the next time the MCU shows it')


def verify():
    rb = OUT / 'readback-flash.bin'
    profile.gdb(f'dump binary memory {rb} {FLASH:#x} {FLASH + SIZE:#x}', 'verify', timeout=600)
    got = rb.read_bytes()
    before, after = images()
    same = lambda a, b: a[:0x1f000] == b[:0x1f000]      # settings pages may change at run time
    state = ('PATCHED: the profile patch and the forward text' if same(got, after) else
             'BEFORE: the profile patch, the text as shipped' if same(got, before) else 'NEITHER')
    print(f'  flash read back ({len(got)} bytes, settings pages excluded): {state}')
    return state


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['build', 'check', 'rehearse', 'program', 'restore', 'verify'])
    a = ap.parse_args()
    {'build': build, 'check': check, 'rehearse': rehearse, 'program': program,
     'restore': lambda: program(restore=True), 'verify': verify}[a.cmd]()


if __name__ == '__main__':
    main()
