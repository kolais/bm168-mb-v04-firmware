#!/usr/bin/env python3
"""Make a panel-sized H.264 test stream shaped like a projection stream.

CarPlay/Android Auto send screen-sized, real-time H.264 (no B-frames), and
stock libarkcmn lays the projection layer out at screen size, so decoder +
display tests need a 1280x480 stream rather than the SDK's 720x1280 pieces.

Writes OUT/000.h264, 001.h264, ... (one Annex B NAL unit per file, like the
SDK pieces) and OUT/reference-fnv.txt with "F n luma chroma" FNV-1a sums of
ffmpeg's NV12 decode, comparable with probe-mfc / probe-vdec output.
"""
import argparse
import pathlib
import subprocess


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=pathlib.Path)
    ap.add_argument('--width', type=int, default=1280)
    ap.add_argument('--height', type=int, default=480)
    ap.add_argument('--seconds', type=int, default=5)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    stream = a.out / 'stream.h264'
    subprocess.run([
        'ffmpeg', '-v', 'error', '-f', 'lavfi',
        '-i', f'testsrc2=size={a.width}x{a.height}:rate=30:duration={a.seconds}',
        '-c:v', 'libx264', '-profile:v', 'high', '-pix_fmt', 'yuv420p', '-bf', '0',
        '-g', '600', '-tune', 'zerolatency', '-bitexact', '-flags', '+bitexact',
        '-x264-params', 'threads=1:sliced-threads=0', '-f', 'h264', str(stream)], check=True)
    data = stream.read_bytes()
    starts = []
    i = 0
    while True:
        i = data.find(b'\x00\x00\x01', i)
        if i < 0:
            break
        starts.append(i - 1 if i and data[i - 1] == 0 else i)
        i += 3
    starts.append(len(data))
    for n, (s, e) in enumerate(zip(starts, starts[1:])):
        (a.out / f'{n:03d}.h264').write_bytes(data[s:e])
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(stream), '-f', 'rawvideo', '-pix_fmt', 'nv12', '-'],
                         check=True, capture_output=True).stdout
    ys = a.width * a.height
    fs = ys * 3 // 2
    with (a.out / 'reference-fnv.txt').open('w') as f:
        for k in range(len(raw) // fs):
            fr = raw[k * fs:(k + 1) * fs]
            f.write(f'F {k} {fnv(fr[:ys]):08x} {fnv(fr[ys:]):08x}\n')
    stream.unlink()
    print(f'{len(starts) - 1} pieces, {len(raw) // fs} reference frames in {a.out}')


if __name__ == '__main__':
    main()
