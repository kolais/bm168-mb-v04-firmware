# Two modes

The launcher ships in two render modes so the same design runs on hardware that can composite and animate, and on hardware that can only redraw small areas. **Lite is the design. Full only adds to it.** Layout, sizes, type, colour meaning, focus order and words are identical in both; a screen is specified once.

## What differs

| | Full | Lite |
|---|---|---|
| Background | One wallpaper, blurred and darkened at build time | Plain `ground` |
| Tile and card fill | `glass` over the wallpaper | Opaque `surface` |
| Pressed fill, quiet pill | `glass-raised` | Opaque `surface-raised` |
| Top edge | `edge-light` | `edge-light` |
| Focus | 3px `amber` ring plus `focus-glow` | 3px `amber` ring |
| Focus move | Ring travels, 120ms ease-out | Ring jumps |
| Press | Fill fades in over 80ms | Fill switches |
| List scroll | Rows slide one band, 160ms | Rows jump one band |
| Screen change | Cross-fade with a 24px slide, 200ms | Cut |
| Into CarPlay, back to BMW | Content fades to `ground` over 200ms, then the picture switches | One frame of `ground`, then the picture switches |
| Connecting | The spinner arc turns smoothly, once per second | The same |
| Alpha blending | Text, icons, glass fills, fades | Text and icons only |
| Colour depth | 24-bit or better | Safe at 16-bit: flat fills only, no gradients |
| Frame budget | Every animation holds 30 frames per second or it is removed | Each input redraws only the old and new focus areas |

## Rules for both

- Never put information in an effect. Glow, glass and motion are absent in Lite, so state must already be clear from the ring, the fill, the shape and the word.
- Do not design a screen that needs the wallpaper to look finished. Check every screen in Lite first.
- The header band, with its fixed things, and the rows do not use `glass` in either mode: they stay on `ground` and `ground-raised` so list text always sits on a known colour.

## Rules for Full

- The wallpaper is one static image. Blur and darken it at build time so that no pixel is brighter than `#303030`; with that ceiling `ink` and `ink-muted` hold 4.5:1 on `glass`. `ink-faint` does not: on glass, show disabled items with an `outline` border and `ink-muted` text.
- Nothing moves behind glass. Glass never overlaps a scrolling list or another glass surface.
- `focus-glow` is a pre-rendered nine-patch image tinted `amber`, not a computed shadow.
- Animate two objects at most at once, opacity and position only.

## Choosing the mode

Pick the mode per device from one measurement, not from the chip's name: run the 200ms full-content cross-fade on the unit. If it holds 30 frames per second, ship Full; otherwise ship Lite. Expose it as a single setting ("Display" → "Visual effects": On or Off) so the driver can drop to Lite, and fall back to Lite automatically while CarPlay video is being decoded in the background.
