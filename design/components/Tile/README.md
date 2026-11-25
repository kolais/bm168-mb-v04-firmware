One paired phone on Home: a 264 × 320 card with CarPlay's corner and a drawn glass edge.

**The consumer provides** the phone's name (`name-28`, two lines at most) and its state. The 56px phone icon is fixed.

A phone is in exactly one state, shown as one word under the name. When more than one applies, the first in this list wins:

| State | Word | Tile | When |
|---|---|---|---|
| Connected | "Connected" | Filled `blue`, text `on-blue` | CarPlay is linked to this phone now. At most one. |
| Unreachable | red square, "Unreachable" | `surface`, word `ink-muted` | The last attempt to connect to it failed. Clears at the next attempt or the next ignition. |
| Last used | "Last used" | `surface`, word `ink` | The phone most recently connected, when it is not connected now. At most one. A label only: the phone tried at ignition is the driver profile's. |
| Paired | "Paired" | `surface`, word `ink-muted` | Known, and none of the above. |

- **Focus**: 3px `amber` ring, 4px outside, on any state (shown here on "Last used").
- **Pressed**: `surface-raised` fill.
- **Pair a phone**: a dotted `outline` (3px round dots at a 9px pitch, the same round the corners), no fill, a plus for an icon. Always the last tile.

Lite fills are shown. Full replaces `surface` with `glass` over the wallpaper.
