#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate design/tokens.css and launcher/ui/tokens.h from design/tokens.json.

tokens.json is the single source of the launcher's design values. The
stylesheet gives the HTML previews under design/components/ their custom
properties (--ground, --band, ...) and type classes (.row-32, ...); the
header gives launcherd's LVGL code the same values as T_... constants.

  tools/design_tokens.py           write both files
  tools/design_tokens.py --check   fail if a committed file is out of date
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DESIGN = ROOT / "design"
HEADER = ROOT / "launcher/ui/tokens.h"
FAMILIES = ("color", "spacing", "radius", "shadow", "size", "duration")


def render(tokens: dict) -> str:
    out = [f"/* {tokens['name']} — generated from tokens.json by tools/design_tokens.py; do not edit */", ":root {"]
    seen = set()
    for family in FAMILIES:
        for tok in tokens.get(family, {}).get("tokens", []):
            name, value = tok["name"], tok["value"]
            if name in seen:
                raise SystemExit(f"duplicate token name: {name}")
            seen.add(name)
            if isinstance(value, dict):  # per-theme colour: this system has one theme
                value = next(iter(value.values()))
            out.append(f"  --{name}: {value};")
    for key, stack in tokens["type"]["families"].items():
        out.append(f"  --font-{key}: {stack};")
    out.append("}")
    for group in tokens["type"]["groups"]:
        for style in group["styles"]:
            rules = [
                f"font-family: var(--font-{group['family']})",
                f"font-size: {style['fontSize']}",
                f"line-height: {style['lineHeight']}",
                f"font-weight: {style['fontWeight']}",
            ]
            if "letterSpacing" in style:
                rules.append(f"letter-spacing: {style['letterSpacing']}")
            out.append(f".{style['name']} {{ {'; '.join(rules)}; }}")
    return "\n".join(out) + "\n"


def c_name(name: str) -> str:
    return "T_" + re.sub(r"[^A-Za-z0-9]", "_", name).upper()


def colour(value: str):
    """(0xRRGGBB, opacity 0-255) of a hex or rgba() colour, or None."""
    m = re.fullmatch(r"#([0-9a-fA-F]{6})", value)
    if m:
        return int(m.group(1), 16), 255
    m = re.search(r"rgba\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([0-9.]+)\s*\)", value)
    if m:
        r, g, b = (int(m.group(i)) for i in (1, 2, 3))
        return (r << 16) | (g << 8) | b, round(float(m.group(4)) * 255)
    return None


def render_header(tokens: dict) -> str:
    out = [
        f"/* {tokens['name']} - generated from design/tokens.json by tools/design_tokens.py; do not edit */",
        "#ifndef UI_TOKENS_H",
        "#define UI_TOKENS_H",
        "",
        "/* Colours: 0xRRGGBB; a translucent colour also has _OPA (0-255). Shadows give their colour only. */",
    ]
    for family in ("color", "shadow"):
        for tok in tokens.get(family, {}).get("tokens", []):
            value = tok["value"]
            if isinstance(value, dict):
                value = next(iter(value.values()))
            parsed = colour(value)
            if parsed is None:
                raise SystemExit(f"{tok['name']}: colour {value!r} not understood")
            rgb, opa = parsed
            out.append(f"#define {c_name(tok['name'])} 0x{rgb:06x}")
            if opa != 255:
                out.append(f"#define {c_name(tok['name'])}_OPA {opa}")
    out += ["", "/* Lengths in px, durations in ms. */"]
    for family in ("spacing", "radius", "size", "duration"):
        for tok in tokens.get(family, {}).get("tokens", []):
            m = re.fullmatch(r"(\d+)(px|ms)", tok["value"])
            if not m:
                raise SystemExit(f"{tok['name']}: value {tok['value']!r} not understood")
            out.append(f"#define {c_name(tok['name'])} {m.group(1)}")
    out += ["", "/* Type styles: size and line height in px, weight, letter spacing in px (rounded from em). */"]
    for group in tokens["type"]["groups"]:
        for style in group["styles"]:
            n = c_name(style["name"])
            size = int(style["fontSize"].removesuffix("px"))
            line = int(style["lineHeight"].removesuffix("px"))
            em = float(style.get("letterSpacing", "0em").removesuffix("em"))
            out.append(f"#define {n}_SIZE {size}")
            out.append(f"#define {n}_LINE {line}")
            out.append(f"#define {n}_WEIGHT {style['fontWeight']}")
            out.append(f"#define {n}_SPACE {round(em * size)}")
    out += ["", "#endif"]
    return "\n".join(out) + "\n"


def main() -> int:
    tokens = json.loads((DESIGN / "tokens.json").read_text())
    targets = {DESIGN / "tokens.css": render(tokens), HEADER: render_header(tokens)}
    if "--check" in sys.argv[1:]:
        stale = [p for p, text in targets.items() if not p.exists() or p.read_text() != text]
        for p in stale:
            print(f"{p.relative_to(ROOT)} is out of date: run tools/design_tokens.py", file=sys.stderr)
        return 1 if stale else 0
    for p, text in targets.items():
        p.write_text(text)
        print(f"wrote {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
