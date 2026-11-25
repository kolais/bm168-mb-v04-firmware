#!/usr/bin/env python3
"""One repeatable CarPlay scrolling run on the unit (Plan A or stock).

    stroke_run.py --evidence DIR --tag NAME [--evswipe PATH] [--ratelog PATH]

Injects 75 identical drags in the list body (760,420 <-> 760,120, 600 ms,
250 ms apart; evswipe), records a 20 ms timeline of wlan0 packets and decoder
interrupts (ratelog) for 62 s, samples CPU in the middle (carplay_cpu.py) and
diffs the TCP counters across the run. The owner keeps a long list (Songs) on
the CarPlay screen and does not touch it. Prints the frame gaps of 150 ms or
more and what the network delivered during each.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
PY = sys.executable
CONSOLE = [PY, str(ROOT / 'tools/bench/console.py')]


def shell(ev, cmd, timeout=30):
    r = subprocess.run(CONSOLE + ['--evidence', ev, 'shell', '--timeout', str(timeout), cmd],
                       capture_output=True, text=True)
    return (r.stdout + r.stderr).replace('\r', '')


def counters(text):
    d, lines = {}, text.splitlines()
    for a, b in zip(lines, lines[1:]):
        a, b = a.split(), b.split()
        if (len(a) > 1 and len(a) == len(b) and a[0] == b[0] and a[0].endswith(':')
                and not a[1].lstrip('-').isdigit() and b[1].lstrip('-').isdigit()):
            d.update({a[0] + k: int(v) for k, v in zip(a[1:], b[1:])})
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--evidence', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--evswipe', default='/usr/bin/evswipe')
    ap.add_argument('--ratelog', default='/tmp/ratelog')
    a = ap.parse_args()
    ev = pathlib.Path(a.evidence)
    ev.mkdir(parents=True, exist_ok=True)
    snap = 'cat /proc/net/netstat /proc/net/snmp'
    before = shell(a.evidence, snap)
    out = shell(a.evidence, f'({a.ratelog} 62 20 > /tmp/rl.txt 2>&1 &); '
                f'({a.evswipe} 760 420 760 120 600 75 250 > /tmp/evswipe.log 2>&1 &); echo started')
    if 'started' not in out:
        sys.exit('could not start: ' + out[-300:])
    time.sleep(12)
    cpu = subprocess.run([PY, str(ROOT / 'tools/bench/carplay_cpu.py'), '--evidence', a.evidence,
                          '--tag', a.tag, '--secs', '18'], capture_output=True, text=True).stdout
    time.sleep(28)
    after = shell(a.evidence, snap)
    raw = shell(a.evidence, 'cat /tmp/rl.txt; cat /tmp/evswipe.log', 120)
    (ev / f'stroke-{a.tag}.txt').write_text(before + '\n=====\n' + after + '\n=====\n' + raw)
    print(cpu.strip().splitlines()[0] if cpu.strip() else 'cpu sample failed')
    A, B = counters(before), counters(after)
    keys = ['Tcp:InSegs', 'TcpExt:TCPOFOQueue', 'TcpExt:TCPDSACKOldSent', 'TcpExt:DelayedACKLost',
            'TcpExt:TCPDSACKOfoSent', 'Udp:InDatagrams']
    print('TCP:', ', '.join(f'{k.split(":")[1]} {B.get(k, 0) - A.get(k, 0)}' for k in keys))
    rows = [tuple(map(int, l.split())) for l in raw.splitlines() if re.fullmatch(r'\d+ \d+ \d+ \d+ \d+', l)]
    if len(rows) < 100:
        sys.exit(f'timeline too short ({len(rows)} samples)')
    span = (rows[-1][0] - rows[0][0]) / 1000
    print(f'timeline {span:.1f} s: {(rows[-1][3] - rows[0][3]) / span:.1f} frames/s, '
          f'{(rows[-1][1] - rows[0][1]) / span:.0f} packets/s, {(rows[-1][2] - rows[0][2]) * 8 / span / 1e6:.1f} Mbit/s')
    gaps, lt, ld = [], rows[0][0], rows[0][3]
    for r in rows[1:]:
        if r[3] != ld:
            if r[0] - lt >= 150:
                gaps.append((lt, r[0]))
            lt, ld = r[0], r[3]
    for s, e in gaps:
        seg = [r for r in rows if s <= r[0] <= e]
        print(f'  no frames {s / 1000:6.2f}-{e / 1000:6.2f} s ({e - s} ms): {seg[-1][1] - seg[0][1]} packets, '
              f'{(seg[-1][2] - seg[0][2]) / 1024:.1f} KB')
    print(f'{len(gaps)} frame gaps >= 150 ms; raw {ev}/stroke-{a.tag}.txt')


if __name__ == '__main__':
    main()
