#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The default picture for the car's tile on the phone's CarPlay home screen:
design/assets/carplay-icon@2x.png (120 x 120) and @3x (180 x 180).

No name and nobody's mark: the launcher's ground with its strands of light
and one amber strand. A builder's own picture goes in
local/assets/carplay-icon@2x.png and is used in its place by the build
(tools/recovery.py). Needs a Chromium-family browser on the host:

  tools/design_tile.py [--browser PATH]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

from design_icons import find_browser

ROOT = Path(__file__).resolve().parent.parent
GROUND, RAISED, INK, AMBER = '#030405', '#11151a', '#c9cdd2', '#d64913'


def svg(size):
    strands = ''.join(
        f'<path d="M-4 {70 + 3.2 * i:.1f} C {40 + 4 * i} {72 + 3 * i:.1f}, {70 + 3 * i} {96 - 9.5 * i:.1f}, 124 {100 - 11 * i:.1f}" '
        f'stroke="{INK}" stroke-width="0.9" fill="none" opacity="{0.35 + 0.08 * (3 - abs(3 - i)):.2f}"/>'
        for i in range(7))
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 120 120" style="display:block">
<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{RAISED}"/><stop offset="1" stop-color="{GROUND}"/></linearGradient>
<linearGradient id="a" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="{AMBER}" stop-opacity="0.2"/><stop offset="0.5" stop-color="#ffb070"/><stop offset="1" stop-color="{AMBER}" stop-opacity="0.5"/></linearGradient>
<filter id="b"><feGaussianBlur stdDeviation="2.4"/></filter></defs>
<rect width="120" height="120" fill="url(#g)"/>
{strands}
<path d="M-4 62 C 40 66, 76 78, 124 92" stroke="{AMBER}" stroke-width="3.5" fill="none" opacity="0.5" filter="url(#b)"/>
<path d="M-4 62 C 40 66, 76 78, 124 92" stroke="url(#a)" stroke-width="1.4" fill="none"/>
</svg>'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--browser')
    browser = find_browser(ap.parse_args().browser)
    with tempfile.TemporaryDirectory() as tmp:
        page = Path(tmp) / 'tile.html'
        for scale, size in ((2, 120), (3, 180)):
            page.write_text(f'<!doctype html><html><body style="margin:0;background:{GROUND}">{svg(size)}</body></html>')
            out = ROOT / f'design/assets/carplay-icon@{scale}x.png'
            subprocess.run([browser, '--headless', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                            f'--window-size={size},{size}', f'--screenshot={out}', page.as_uri()],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print(f'wrote {out.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
