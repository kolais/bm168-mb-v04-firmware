#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render the launcher's icons to 8-bit alpha maps: launcher/ui/icons.c and icons.h.

Icons are Lucide outlines (https://lucide.dev, ISC licence): a 24-unit grid
and a 2-unit round stroke, as design/README.md asks. LVGL draws an alpha map
in the colour of its label, so one map serves every state.

The output is committed; this tool is only needed to add or resize an icon.
It needs a Chromium-family browser (the rasteriser) and ImageMagick (PNG to
raw grey) on the host:

  tools/design_icons.py [--browser PATH]
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "launcher/ui"

# name: SVG body on the 24-unit grid (Lucide 0.x paths).
ICONS = {
    "phone": '<rect x="5" y="2" width="14" height="20" rx="2"/><path d="M12 18h.01"/>',
    "plus": '<path d="M5 12h14M12 5v14"/>',
    "chevron_left": '<path d="m15 18-6-6 6-6"/>',
    "chevron_right": '<path d="m9 18 6-6-6-6"/>',
    "wifi": '<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/><path d="M5 12.86a10 10 0 0 1 14 0"/>'
            '<path d="M8.5 16.43a5 5 0 0 1 7 0"/>',
    "bluetooth": '<path d="m7 7 10 10-5 5V2l5 5L7 17"/>',
    # Lucide "bluetooth-connected": the rune with a short line at either side.
    "bluetooth_connected": '<path d="m7 7 10 10-5 5V2l5 5L7 17"/><path d="M18 12h3"/><path d="M3 12h3"/>',
    # A running projection session: Lucide "circle-play". A builder's own
    # mark for it goes in local/icons/carplay.svg (an SVG body on this grid)
    # and is used in its place (see LOCAL below).
    "carplay": '<circle cx="12" cy="12" r="10"/><polygon points="10 8 16 12 10 16 10 8"/>',
    "check": '<path d="M20 6 9 17l-5-5"/>',
    "settings": '<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0'
                '-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0'
                '-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2'
                'h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39'
                'a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73'
                'l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/>'
                '<circle cx="12" cy="12" r="3"/>',
}

# (icon, size in px): the sizes the screens use (design/README.md, Icons; design/screens.md).
SIZES = [
    ("phone", 56), ("plus", 56),            # Home tiles, the connecting spinner
    ("chevron_left", 32), ("settings", 32),  # rail buttons
    ("bluetooth", 28), ("bluetooth_connected", 28),  # top band: Bluetooth on; a phone on it
    ("carplay", 24),                         # Home's chip: a CarPlay session runs
    ("chevron_right", 28), ("check", 28),    # list rows: opens a list; the current option
]

# Icons a builder may replace with their own drawing: name -> its SVG body's
# file. The replacement is rendered at the same size into a file beside it
# (NAME_SIZE.inc, the alpha map's initialiser) that icons.c includes in place
# of the default when the build finds it (CMakeLists.txt).
LOCAL = {"carplay": ROOT / "local/icons/carplay.svg"}

BROWSERS = [
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "chromium", "chromium-browser", "google-chrome",
]


def find_browser(arg):
    for cand in ([arg] if arg else BROWSERS):
        path = cand if Path(cand).is_file() else shutil.which(cand)
        if path:
            return path
    raise SystemExit("no Chromium-family browser found; pass --browser PATH")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--browser")
    args = ap.parse_args()
    browser = find_browser(args.browser)
    if not shutil.which("magick"):
        raise SystemExit("ImageMagick (magick) not found")

    # One sheet: each icon in its own cell, white on black, so grey = alpha.
    cell = max(size for _, size in SIZES)
    local = [(name, size) for name, size in SIZES if name in LOCAL and LOCAL[name].is_file()]
    sheet = [(name, size, ICONS[name]) for name, size in SIZES] + \
            [(name, size, LOCAL[name].read_text().strip()) for name, size in local]
    width, height = cell * len(sheet), cell
    cells = "".join(
        f'<svg style="position:absolute;left:{i * cell}px;top:0" width="{size}" height="{size}" '
        f'viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round" '
        f'stroke-linejoin="round">{body}</svg>'
        for i, (name, size, body) in enumerate(sheet))
    html = f'<!doctype html><html><body style="margin:0;background:#000">{cells}</body></html>'
    with tempfile.TemporaryDirectory() as tmp:
        page, png = Path(tmp) / "sheet.html", Path(tmp) / "sheet.png"
        page.write_text(html)
        subprocess.run([browser, "--headless", "--disable-gpu", "--hide-scrollbars",
                        "--force-device-scale-factor=1", f"--window-size={width},{height}",
                        f"--screenshot={png}", page.as_uri()],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        grey = subprocess.run(["magick", str(png), "-colorspace", "Gray", "-depth", "8", "gray:-"],
                              check=True, capture_output=True).stdout
    if len(grey) != width * height:
        raise SystemExit(f"sheet is {len(grey)} bytes, expected {width}x{height}")

    c = ["/* Launcher icons as 8-bit alpha maps - generated by tools/design_icons.py; do not edit.",
         " * Lucide outlines (https://lucide.dev), ISC licence. */",
         '#include "icons.h"', ""]
    h = ["/* Generated by tools/design_icons.py; do not edit. */",
         "#ifndef UI_ICONS_H", "#define UI_ICONS_H", '#include "lvgl.h"', ""]
    def rows_of(i, name, size):
        rows = [grey[y * width + i * cell: y * width + i * cell + size] for y in range(size)]
        if not any(any(r) for r in rows):
            raise SystemExit(f"{name}: rendered empty")
        return ["\t" + ",".join(str(b) for b in r) + "," for r in rows]

    for k, (name, size) in enumerate(local):
        inc = LOCAL[name].with_name(f"{name}_{size}.inc")
        if inc.exists() and inc.stat().st_mtime >= LOCAL[name].stat().st_mtime:
            continue    # as it was rendered when the drawing last changed: a browser's edges vary a little
        inc.write_text("\n".join(rows_of(len(SIZES) + k, name, size)) + "\n")
        print(f"wrote {inc.relative_to(ROOT)} (this builder's own {name} mark)")
    for i, (name, size) in enumerate(SIZES):
        sym = f"icon_{name}_{size}"
        c.append(f"static const uint8_t {sym}_map[{size * size}] = {{")
        if name in LOCAL:   # the builder's own drawing, if the build has one
            c += [f"#ifdef ICON_{name.upper()}_LOCAL", f"#include ICON_{name.upper()}_LOCAL", "#else"]
        c += rows_of(i, name, size)
        if name in LOCAL:
            c.append("#endif")
        c += ["};",
              f"const lv_image_dsc_t {sym} = {{",
              f"\t.header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = {size}, .h = {size}, "
              f".stride = {size} }},",
              f"\t.data_size = sizeof({sym}_map),",
              f"\t.data = {sym}_map,",
              "};", ""]
        h.append(f"extern const lv_image_dsc_t {sym};")
    h += ["", "#endif"]
    (OUT / "icons.c").write_text("\n".join(c))
    (OUT / "icons.h").write_text("\n".join(h) + "\n")
    print(f"wrote launcher/ui/icons.c, icons.h ({len(SIZES)} icons)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
