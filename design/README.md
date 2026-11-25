The launcher is the screen between two systems the driver already knows: BMW's NBT iDrive menus and Apple CarPlay. It should feel like neither took over. Build every screen from the rules below.

## The idea

**Car skeleton, phone skin.** Structure comes from NBT: horizontal bands, left-anchored titles, plain lists, one unmistakable focus. Surface comes from CarPlay: rounded tiles, a glass edge, iOS system colours, a status column on the driver's side.

Four rules carry most of it:

1. **Amber is the knob.** `amber` marks where the iDrive controller is, and nothing else. One amber element per screen. A finger press never turns anything amber.
2. **Blue is the phone.** `blue` marks the control that leads into CarPlay and the phone that is in CarPlay now. Nothing car-side is blue.
3. **Shape tells origin.** Car-side things (menu rows, settings) are near-square: `radius-sm`. Phone-side things (phones, cards, buttons) are round: `radius-lg` and `radius-pill`.
4. **Eight bands of 60.** The 480px panel is eight `band` units, the row pitch of an NBT menu. The header is one band, every row is one band, every touch target is one band tall. Row text is large for its row: `row-32` in 60px.

## Colour

- The screen is `ground`. A band that must stand off it may use `ground-raised`. Cards and tiles are `surface`; pressed and nested things are `surface-raised`.
- Text is `ink`; secondary text is `ink-muted`; disabled text is `ink-faint` (not on `surface-raised`).
- `green`, `yellow` and `red` report link state only, always beside a word and a distinct shape (disc, diamond, square). They are never decoration and never a button fill.
- There is one look, and it is dark. No light theme and no separate day and night themes: NBT has neither, and a launcher that changed between two dark pictures would flash. The colours are tuned low enough for night driving and still hold their contrast by day; brightness is the backlight's job, set in Display.
- Dividers use `hairline` and are decorative. A border that means something (empty slot, disabled control) uses `outline`.

## Type

One family, Inter, because it sits beside CarPlay's San Francisco without a change of voice and holds NBT's Helvetica proportions. Three weights only: 300 for the clock digits, 400 for reading, 600 for naming.

- Use the eight styles as given: `clock-96`, `display-48`, `title-36`, `name-28`, `row-32`, `body-24`, `label-20`, `eyebrow-20`. Do not add sizes: the eight styles are the system, and on the unit each size and weight is a font of its own.
- Nothing is smaller than 20px. That is the legibility floor for a glance at arm's length on this panel.
- One `display-48` line per screen. The screen title is `title-36`, top-left in the header band.
- Numerals are tabular everywhere, so the clock and values do not jitter.
- Sentence case for everything except `eyebrow-20`, which is uppercase.
- Focus never changes weight or size.

## Layout

- Panel: `screen-w` × `screen-h` (1280 × 480). Content has the whole width. Wi-Fi, Bluetooth and the clock stand at the right of the top band on every screen; Home keeps a 112px strip on the driver's side free for the Settings cog (see `components/StatusBar`). There is no Back button on the screen.
- Screen gutter and card padding: `space-5`. Row and pill inner padding: `space-4`. Gap between tiles: `space-3`.
- A list shows seven rows under the header band and scrolls by whole rows.
- Phone-side screens (Home) are a single row of tiles. Car-side screens (Settings) are a single list across the screen, with no help text.
- Keep the header band, the status in it and the first row's baseline in the same place on every screen.

## Glass, drawn

CarPlay's glass is blurred live; this hardware should not try. A glass surface is a flat fill with `edge-light` along its top edge and a `radius-lg` corner. Lite mode fills it with opaque `surface`. Full mode fills it with `glass` over one wallpaper baked at build time. Neither mode blurs at runtime or casts drop shadows. See Two modes.

## Focus and touch

Both inputs are first-class and they look different on purpose.

- **Knob focus**: a 3px (`ring`) solid `amber` ring. On rows it sits inside the row with an `amber-wash` fill. On tiles and pills it sits 4px outside and the fill is untouched.
- **Touch press**: the fill steps up to `surface-raised` while the finger is down. No ring.
- After a touch, the knob focus moves to the touched element on release, so the next turn of the knob continues from there.
- Exactly one element has knob focus on every screen that has controls. Choose the default so that a single push does the likely thing. The connecting screen has no controls and no ring.
- Focus does not wrap at list ends. The text-entry dial is the exception: it is a circle.

## Motion

These are the Full-mode timings. Lite replaces each with a cut; see Two modes. What each input triggers, and its sound, is in Feedback.

- Focus move: 120ms, ease-out. The ring travels; the content does not.
- Screen change: 200ms. Content cross-fades and slides 24px in the direction of travel (deeper is leftward, back is rightward, as in NBT). The status in the band holds still.
- Into CarPlay and back to BMW: fade the content to `ground` over 200ms, then switch the picture. Every hand-off is a cut on black.
- Animate opacity and position only, two objects at a time. Nothing loops and nothing moves without input, except a connecting indicator.

## Words

- Name things as the car and the phone do: "Driver profile", "Phone", "CarPlay", "Stock system".
- Labels are two or three words, sentence case, no full stop: "Open CarPlay", "Choose another phone", "Back to BMW".
- A button says what happens. Never "OK", "Yes" or "Continue" alone.
- States are one word where possible: "Connected", "Last used", "Paired", "Unreachable".
- Errors say what is wrong and what to do: "No phone for profile 2. Choose one to remember."
- No exclamation marks, no emoji, no brand logos. Write "CarPlay" and "BMW" as plain text.

## Icons

- Use Lucide outlines: 24-unit grid, 2-unit round stroke, drawn at `icon` (40px) in rows and tiles and 28px in the top band. In use so far: phone, plus, chevrons, Wi-Fi, Bluetooth, the cog, and a check for the current option in a list. Their weight sits close to SF Symbols at regular.
- Icons take the colour of their label. Never tint an icon `amber`.
- Do not ship SF Symbols or BMW pictograms: neither is licensed for third-party firmware.
- In LVGL, pre-render icons as 8-bit alpha maps and recolour them with the text colour.

## Building it in LVGL

- Fonts: Inter Light, Regular and SemiBold are built into `launcherd` and drawn from their outlines at seven sizes: 96/300 (the clock), 48/600, 36/600, 32/400, 28/600, 24/400, 20/600. Nothing is pre-rendered, so a phone's name in Greek or Cyrillic needs no extra asset; a character Inter lacks shows as a box.
- Tabular numerals are an OpenType feature this renderer does not apply. Where digits must not jitter (the clock, amounts), give each digit a fixed cell.
- The focus ring on tiles and rows is one object of its own, 3px `amber`, so it can travel between items; on the cog and on pills it is an LVGL outline (`outline_width` 3, `outline_pad` 4). Do not use shadows for glow.
- `edge-light` is a 1px top border in white at the token's opacity.
- There is one LVGL theme, `launcher/ui/theme.c`, and it takes every value from `tokens.json` (through the generated `launcher/ui/tokens.h`). A screen never asks which render mode is on: what differs between Full and Lite is decided in the theme. Do not dim the UI with an overlay or by text opacity. Brightness changes the backlight only.

## In this folder

This folder is the launcher's design system and its source of truth. It was drawn as a claude.ai design-system artifact (2026-11-20/21) and moved here on 2026-11-21 with the owner's flow decisions applied; the artifact is not kept in step.

| File | What |
|---|---|
| `README.md` | This brand book: the idea, colour, type, layout, focus, motion, words, icons. |
| [`screens.md`](screens.md) | Flow, driver profile, and every screen: Home, Connecting, the switch banner over CarPlay, Settings, the instruction page, text entry. |
| [`modes.md`](modes.md) | Full and Lite render modes. |
| [`feedback.md`](feedback.md) | What every input does, and the sounds. |
| [`tokens.json`](tokens.json) | Every value: colours, type styles, spacing, radii, sizes, durations. Change values here only. |
| `tokens.css` | Generated from `tokens.json` by `tools/design_tokens.py` (`make design`), together with `launcher/ui/tokens.h` for the code; the previews load it. |
| `components/<Name>/README.md` | Rules for one building block. |
| `components/<Name>/preview.html` | Tile, ListRow, Pill and Modes drawn in a browser, and FeedbackLab, a list to drive for the feel of the feedback rules and sounds. Open the file directly. The Tile and FeedbackLab previews still draw the frame of before 2026-11-22 (a rail down one side); the frame is judged from the built pictures. Inter comes from Google Fonts, so offline the previews fall back to the system font. |

The browser prototype of the whole launcher was retired on 2026-11-21: it showed the flow as first drawn. The screens are reviewed as pictures drawn by the code that ships: `make ui-snap` writes them to `build/snap/out/`.

How the design meets this unit (the MCU, `launcherd`, what is measured and what is still open) is in [`docs/launcher-design.md`](../docs/launcher-design.md).
