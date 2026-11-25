#!/usr/bin/env python3
"""Measure CarPlay CPU cost on the running Plan A unit over the console.

Samples /proc/stat (CPU and context switches), interrupt counts, wlan0
packets and the AUTOTOP kernel switches before and after a window, prints
CPU per packet and saves the raw output.

    carplay_cpu.py --evidence DIR --tag NAME [--secs 18] [--set PARAM=VALUE ...]

--set writes run-time parameters first, e.g. mmc_core.spin_us=50 or
ark_dw_mmc.dma_threshold=16 (module.param, written to
/sys/module/<module>/parameters/<param>). The owner scrolls CarPlay during
the window.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
CONSOLE = [sys.executable, str(ROOT / 'tools/bench/console.py')]
PARAMS = ['mmc_core/parameters/spin_us', 'mmc_core/parameters/spin_hits',
          'mmc_core/parameters/spin_misses', 'ark_dw_mmc/parameters/dma_threshold',
          'ark_dw_mmc/parameters/xfers_dma', 'ark_dw_mmc/parameters/xfers_pio']


def shell(evidence, cmd, timeout):
    r = subprocess.run(CONSOLE + ['--evidence', evidence, 'shell', '--timeout', str(timeout), cmd],
                       capture_output=True, text=True)
    return r.stdout + r.stderr


def snapshot_cmd(mark):
    b = '/bin/busybox'
    reads = ' '.join(f'$(cat {p} 2>/dev/null || echo -)' for p in PARAMS)
    return (f'echo {mark}; cat /proc/uptime; {b} grep -E "^cpu |^ctxt" /proc/stat; '
            f'{b} grep -E "dw-mci|dw_dmac|vdec" /proc/interrupts; '
            f'{b} grep wlan0 /proc/net/dev; cd /sys/module; echo P {reads}')


def parse(block):
    up = float(re.search(r'^(\d+\.\d+) \d+\.\d+$', block, re.M).group(1))
    cpu = list(map(int, re.search(r'^cpu\s+(.*)$', block, re.M).group(1).split()))
    ctxt = int(re.search(r'^ctxt (\d+)', block, re.M).group(1))
    irq = {n: int(v) for v, n in re.findall(r'^\s*\d+:\s+(\d+)\s+VIC\s+\d+\s+Edge\s+(\S+)', block, re.M)}
    rx = re.search(r'wlan0:\s*(\d+)\s+(\d+)', block)
    params = re.search(r'^P (.*)$', block, re.M).group(1).split()
    return up, cpu, ctxt, irq, int(rx.group(1)), int(rx.group(2)), params


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--evidence', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--secs', type=int, default=18)
    ap.add_argument('--set', action='append', default=[])
    a = ap.parse_args()
    for s in a.set:
        m = re.fullmatch(r'([a-z0-9_]+)\.([a-z0-9_]+)=(\d+)', s)
        if not m:
            sys.exit(f'bad --set {s!r}')
        out = shell(a.evidence, f'echo {m.group(3)} > /sys/module/{m.group(1)}/parameters/{m.group(2)}; '
                                f'cat /sys/module/{m.group(1)}/parameters/{m.group(2)}', 30)
        print(f'set {s}:', 'ok' if re.search(rf'^{m.group(3)}\s*$', out, re.M) else 'FAILED')
    out = shell(a.evidence, snapshot_cmd('=A='), 30)
    time.sleep(a.secs)
    out = (out + shell(a.evidence, snapshot_cmd('=B='), 30)).replace('\r', '')
    path = pathlib.Path(a.evidence) / f'cpu-{a.tag}.txt'
    path.write_text(out)
    try:
        b0 = out[out.rindex('\n=A=\n'):out.rindex('\n=B=\n')]
        b1 = out[out.rindex('\n=B=\n'):]
        u0, c0, x0, i0, by0, p0, q0 = parse(b0)
        u1, c1, x1, i1, by1, p1, q1 = parse(b1)
        a.secs = u1 - u0
    except (ValueError, AttributeError) as e:
        sys.exit(f'could not parse output ({e}); raw output in {path}')
    d = [y - x for x, y in zip(c0, c1)]
    tot = sum(d) or 1
    busy = 100 * (tot - d[3]) / tot
    pk = (p1 - p0) / a.secs
    rate = lambda n: (i1.get(n, 0) - i0.get(n, 0)) / a.secs
    print(f'{a.tag}: busy {busy:.0f}% (user {100*d[0]/tot:.0f} sys {100*d[2]/tot:.0f} '
          f'softirq {100*d[6]/tot:.0f}), {pk:.0f} pkt/s, {(by1-by0)*8/a.secs/1e6:.1f} Mbit/s, '
          f'{rate("e0900000.vdec"):.1f} frames/s, SDIO {rate("dw-mci"):.0f}/s, DMA {rate("dw_dmac"):.0f}/s, '
          f'ctxt {(x1-x0)/a.secs:.0f}/s')
    if pk:
        print(f'  CPU per 100 pkt/s: {busy / pk * 100:.1f}%, context switches per packet: '
              f'{(x1 - x0) / a.secs / pk:.1f}')
    names = [p.split('/')[-1] for p in PARAMS]
    print('  params after:', ', '.join(f'{n}={v}' for n, v in zip(names, q1)))
    try:
        hits = int(q1[1]) - int(q0[1]); miss = int(q1[2]) - int(q0[2])
        print(f'  spin hits {hits}, misses {miss} in the window')
    except ValueError:
        pass
    print(f'  raw: {path}')


if __name__ == '__main__':
    main()
