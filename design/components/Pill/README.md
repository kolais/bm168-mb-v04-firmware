Fully rounded button and status chip: the phone-side control, shaped like CarPlay's own.

**The consumer provides** a label in `label-20`, sentence case, two or three words that say what happens ("Open CarPlay", not "OK").

Button variants:

- **Primary** (`blue` fill, `on-blue` label): the one control on a screen that leads into CarPlay. At most one per screen.
- **Quiet** (`surface-raised` fill, `ink` label, `edge-light`): every other action.
- **Disabled**: no fill, 1px `outline` border, `ink-faint` label.
- **Focus**: a 3px `amber` ring, 4px outside the pill. The fill does not change, so blue still says "phone" while amber says "knob".

Visible height is `pill-h` (48px); the touch area is the full 60px `band`.

Status chips (40px, not focusable) pair a shape with a word so they read without colour: green disc "Connected", yellow diamond "Connecting", red square "Unreachable", hollow ring "Not paired".
