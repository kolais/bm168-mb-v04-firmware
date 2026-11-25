#!/usr/bin/env python3
"""Per-thread CPU over a window on the unit (Plan A or stock), over the console.

    thread_cpu.py EVIDENCE TAG SECS          capture now
    thread_cpu.py EVIDENCE TAG --from FILE   re-parse a saved capture

Snapshots /proc/*/task/*/stat and the context-switch counters into /tmp on
the unit, then prints per-second user/system CPU, page faults and voluntary/
involuntary context switches per thread. Stock's carplay logs to the console;
lines it breaks are skipped.
"""
import re, subprocess, sys, time, pathlib
ROOT = pathlib.Path(__file__).resolve().parents[2]
ev, tag = sys.argv[1], sys.argv[2]
FROM = sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == '--from' else None
secs = 0 if FROM else float(sys.argv[3])
PY = str(ROOT / '.venv/bin/python'); CON = str(ROOT / 'tools/bench/console.py')
def sh(cmd, t=60):
    r = subprocess.run([PY, CON, '--evidence', ev, 'shell', '--timeout', str(t), cmd], capture_output=True, text=True)
    return r.stdout + r.stderr
snap = ('cd /proc; for t in [0-9]*/task/[0-9]*; do echo "@ $t $(cat $t/stat 2>/dev/null) '
        '$(/bin/busybox grep ctxt $t/status 2>/dev/null)"; done > /tmp/%s; cat uptime >> /tmp/%s; echo ok')
if FROM:
    p = pathlib.Path(FROM); out = p.read_text()
else:
    print(sh(snap % ('tA', 'tA'))[-80:].strip().splitlines()[-2:])
    time.sleep(secs)
    print(sh(snap % ('tB', 'tB'))[-80:].strip().splitlines()[-2:])
    out = sh('cat /tmp/tA; echo =====; cat /tmp/tB', 120).replace('\r', '')
    p = pathlib.Path(ev) / f'threads-{tag}.txt'; p.write_text(out)
a, b = out.split('=====\n')[-2:] if out.count('=====\n') >= 2 else out.split('=====\n')
def parse(block):
    th = {}
    block = re.sub(r'\n(nonvoluntary_ctxt)', r' \1', block)
    for line in block.splitlines():
        m = re.match(r'@ (\d+)/task/(\d+) \d+ \((.*)\) (.*?)(?: voluntary_ctxt_switches:\s*(\d+)\s+nonvoluntary_ctxt_switches:\s*(\d+))?$', line)
        if not m: continue
        f = m.group(4).split()
        # fields after comm: state(0) ppid(1) ... minflt(7) majflt(9) utime(11) stime(12)
        try:
            th[m.group(2)] = (m.group(1), m.group(3), int(f[7]), int(f[9]), int(f[11]), int(f[12]),
                              int(m.group(5) or 0), int(m.group(6) or 0))
        except (ValueError, IndexError):
            pass   # line broken by console log output (stock carplay logs to ttyS0)
    up = [float(x.split()[0]) for x in block.splitlines() if re.match(r'^\d+\.\d+ \d+\.\d+$', x)]
    return th, up[-1] if up else None
A, ua = parse(a); B, ub = parse(b)
dt = ub - ua
rows = []
for tid, v in B.items():
    if tid not in A: continue
    u = A[tid]; d = [v[i] - u[i] for i in range(2, 8)]
    if any(d): rows.append((d[2] + d[3], v[0], tid, v[1], d))
rows.sort(reverse=True)
print(f'window {dt:.1f} s; per-second rates; CPU % of one core (HZ=100 ticks)')
print(f'{"thread":18} {"pid":>5} {"usr%":>5} {"sys%":>5} {"minflt/s":>9} {"majflt/s":>8} {"vcs/s":>7} {"ivcs/s":>7}')
tot = [0]*6
for _, pid, tid, name, d in rows[:25]:
    print(f'{name[:18]:18} {pid:>5} {d[2]/dt:5.1f} {d[3]/dt:5.1f} {d[0]/dt:9.0f} {d[1]/dt:8.1f} {d[4]/dt:7.0f} {d[5]/dt:7.0f}')
for _, pid, tid, name, d in rows:
    tot = [x + y for x, y in zip(tot, d)]
print(f'{"ALL threads":18} {"":>5} {tot[2]/dt:5.1f} {tot[3]/dt:5.1f} {tot[0]/dt:9.0f} {tot[1]/dt:8.1f} {tot[4]/dt:7.0f} {tot[5]/dt:7.0f}')
print('raw:', p)
