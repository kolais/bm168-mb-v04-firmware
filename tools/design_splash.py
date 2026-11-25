#!/usr/bin/env python3
"""Splash candidates: 1280 x 480, the launcher's ground, ink and amber, Inter, and strands of light."""
import subprocess, sys
from pathlib import Path
REPO = Path(__file__).resolve().parent.parent
OUT = REPO / 'local/assets/splash'    # a builder's own boot picture: not part of the repository
OUT.mkdir(parents=True, exist_ok=True)
W, H = 1280, 480
GROUND, INK, MUTED, AMBER, WASH = '#030405', '#c9cdd2', '#959da8', '#d64913', '#3d0f04'
FONT = REPO / 'build/snap/inter'


def strands(n, yl0, yl1, yr0, yr1, bend=0.0, width=1.5, opacity=0.75):
    out = []
    for i in range(n):
        t = i / (n - 1)
        yl, yr = yl0 + (yl1 - yl0) * t, yr0 + (yr1 - yr0) * t
        out.append(f'<path d="M-20 {yl:.1f} C {W * (0.34 + 0.16 * t):.0f} {yl + bend:.1f}, {W * (0.50 + 0.20 * t):.0f} {yr - bend * 0.5:.1f}, {W + 20} {yr:.1f}" '
                   f'stroke="url(#strand)" stroke-width="{width}" fill="none" opacity="{opacity * (0.55 + 0.45 * (1 - abs(2 * t - 1))):.2f}"/>')
    return '\n'.join(out)


def amber(y0, y1, c0, c1):
    d = f'M-20 {y0} C {W * 0.35:.0f} {c0}, {W * 0.6:.0f} {c1}, {W + 20} {y1}'
    return (f'<path d="{d}" stroke="{AMBER}" stroke-width="7" fill="none" opacity="0.55" filter="url(#glow)" stroke-linecap="round" mask="url(#amberfade)"/>'
            f'<path d="{d}" stroke="url(#amber)" stroke-width="2.2" fill="none" stroke-linecap="round"/>')


def defs(cx, cy):
    return f'''<defs>
<linearGradient id="strand" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{W}" y2="0">
<stop offset="0" stop-color="{INK}" stop-opacity="0.38"/><stop offset="0.30" stop-color="{INK}" stop-opacity="0.80"/>
<stop offset="0.62" stop-color="#ffffff" stop-opacity="0.95"/><stop offset="1" stop-color="{INK}" stop-opacity="0.30"/></linearGradient>
<linearGradient id="amber" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{W}" y2="0">
<stop offset="0.10" stop-color="{AMBER}" stop-opacity="0"/><stop offset="0.38" stop-color="{AMBER}"/>
<stop offset="0.58" stop-color="#ffb070"/><stop offset="0.80" stop-color="{AMBER}"/><stop offset="1" stop-color="{AMBER}" stop-opacity="0.15"/></linearGradient>
<linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{W}" y2="0">
<stop offset="0.12" stop-color="#000"/><stop offset="0.5" stop-color="#fff"/><stop offset="0.9" stop-color="#000"/></linearGradient>
<mask id="amberfade"><rect width="{W}" height="{H}" fill="url(#fade)"/></mask>
<radialGradient id="wash" gradientUnits="userSpaceOnUse" cx="{cx}" cy="{cy}" r="420" gradientTransform="translate(0 {cy * 0.45:.0f}) scale(1 0.55)">
<stop offset="0" stop-color="{WASH}" stop-opacity="0.95"/><stop offset="0.5" stop-color="{WASH}" stop-opacity="0.35"/><stop offset="1" stop-color="{WASH}" stop-opacity="0"/></radialGradient>
<linearGradient id="word" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f2f4f6"/><stop offset="1" stop-color="{MUTED}"/></linearGradient>
<filter id="glow" x="-10%" y="-200%" width="120%" height="500%"><feGaussianBlur stdDeviation="9"/></filter>
</defs>'''


def page(name, body):
    html = f'''<!doctype html><html><head><style>
@font-face{{font-family:Inter;font-weight:300;src:url(file://{FONT}/Inter-Light.ttf)}}
@font-face{{font-family:Inter;font-weight:400;src:url(file://{FONT}/Inter-Regular.ttf)}}
@font-face{{font-family:Inter;font-weight:600;src:url(file://{FONT}/Inter-SemiBold.ttf)}}
body{{margin:0;background:{GROUND}}} text{{font-family:Inter}}</style></head><body>
<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
<rect width="{W}" height="{H}" fill="{GROUND}"/>
{body}
</svg></body></html>'''
    (OUT / f'{name}.html').write_text(html)
    subprocess.run(['/Applications/Chromium.app/Contents/MacOS/Chromium', '--headless', '--disable-gpu', '--hide-scrollbars',
                    '--force-device-scale-factor=1', f'--window-size={W},{H}', f'--screenshot={OUT / (name + ".png")}',
                    (OUT / f'{name}.html').as_uri()], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def word(x, y, a, b, anchor='middle', big=76, small=64):
    return (f'<text x="{x}" y="{y}" text-anchor="{anchor}" fill="url(#word)">'
            f'<tspan font-weight="600" font-size="{big}" letter-spacing="-1">{a}</tspan>'
            f'<tspan font-weight="300" font-size="{small}" dx="14" fill="{MUTED}">{b}</tspan></text>')


A, B = (sys.argv[1], sys.argv[2]) if len(sys.argv) > 2 else ('', '')    # the words on it: the builder's choice
# A: the wordmark over a bundle of strands that cross and open, one of them amber
page('a', defs(770, 345) + f'<rect width="{W}" height="{H}" fill="url(#wash)"/>' +
     strands(9, 318, 356, 452, 250, bend=18) + amber(300, 428, 318, 352) + word(W / 2, 190, A, B))
# B: the title where a screen's title stands; the strands large and low
page('b', defs(860, 330) + f'<rect width="{W}" height="{H}" fill="url(#wash)"/>' +
     strands(13, 250, 330, 470, 150, bend=40, width=1.3) + amber(236, 404, 270, 330) +
     word(32, 62, A, B, anchor='start', big=40, small=40))
# C: no strand over the word: the strands pass behind the ring a focused tile wears on Home
ring = (f'<rect x="{W / 2 - 170}" y="{H / 2 - 92}" width="340" height="184" rx="22" fill="{GROUND}" stroke="{AMBER}" stroke-width="4"/>')
page('c', defs(640, 240) + f'<rect width="{W}" height="{H}" fill="url(#wash)"/>' +
     strands(11, 212, 268, 400, 80, bend=26) + amber(196, 300, 224, 262) + ring +
     f'<text x="{W / 2}" y="{H / 2 + 22}" text-anchor="middle" fill="url(#word)">'
     f'<tspan font-weight="600" font-size="56" letter-spacing="-1">{A}</tspan>'
     f'<tspan font-weight="300" font-size="48" dx="10" fill="{MUTED}">{B}</tspan></text>')
print('rendered a, b, c')
