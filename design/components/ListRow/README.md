One 60px band of a list: the car-side building block, modelled on NBT's menu rows. Use it for every setting, every paired phone and every menu entry.

**The consumer provides** a label (`row-32`, `ink`), and optionally a value (`row-32`, `ink-muted`, right-aligned), a leading 40px icon, and a trailing chevron when the row opens another screen.

States, top to bottom in the preview:

- **Rest**: no fill, 1px `hairline` under it.
- **Focus** (the iDrive knob is here): `amber-wash` fill inside a 3px `amber` ring, `radius-sm`. The value brightens to `ink`. Font weight does not change.
- **Pressed** (finger down): `surface-raised` fill. Never amber: touch does not move the knob focus until release.
- **Disabled**: label and value in `ink-faint`; the knob skips it.

Rules:

- Seven rows per screen under the header band, never more. Text is large and rows are tight, as in NBT: `row-32` in a 60px band. A longer list scrolls by whole rows so a row is never cut by the screen edge.
- One line per row. Truncate the label with an ellipsis before the value; never scroll text sideways.
- Rotating the knob moves focus one row per detent with no wrap-around at the ends, as NBT does.
