# Feedback

Every input is answered, by sight first and by sound where the hardware allows. This is the main requirement of the launcher: the driver must never wonder whether a turn, a push or a tap was received.

## The three beats

1. **Acknowledge.** Within one frame of the input, something changes where the driver is already looking: the ring leaves, the fill steps up. This beat is never animated in and never skipped, in either mode.
2. **Change.** The thing that was asked for happens: focus lands, a value flips, a screen opens.
3. **Arrive.** The screen settles with exactly one amber ring, ready for the next input.

Rules:

- An input that can do nothing still gets an answer. Turning past the end of a list bumps. Tapping a disabled item denies. Silence means "not received", so never leave it.
- Input is never queued behind an animation. A new detent retargets the ring from wherever it is. A push during a screen change acts on the arriving screen.
- If the result takes longer than 300ms, the control that was pressed shows the connecting indicator in place. Do not blank the screen or show a separate spinner.
- Knob and touch produce the same result and the same sound for the same action. Only the acknowledge beat differs: the ring answers the knob, the fill answers the finger.

## What each input does

| Input | Name | Full | Lite | Sound |
|---|---|---|---|---|
| Knob, one detent | Step | Ring travels to the next item, `dur-step`, ease-out | Ring jumps | `tick` |
| Knob, detent past the last or first item | Bump | Ring nudges 6px in the turned direction and returns, `dur-bump` | Ring thickens from 3px to 5px for 100ms | `bump` |
| Knob, detent that scrolls the list | Step | Rows slide one band, `dur-scroll`; the ring stays on its item | Rows jump one band | `tick` |
| Knob push, or tap, on an item that opens a screen | Open | Ring fill brightens for `dur-ack`, then content slides in from the right, `dur-screen` | Fill switches for 100ms, then cut | `select` |
| Knob push, or tap, on a toggle | Flip | Ring fill brightens; the value cross-fades, `dur-ack` | Value switches | `on` or `off` |
| Back button, or knob tilt left | Back | Content slides out to the right, `dur-screen`; the ring returns to the item that was opened | Cut; same ring position | `back` |
| Back at the top level | Bump | Header title nudges 6px right and returns | Ring thickens for 100ms | `bump` |
| Finger down | Press | Fill steps to `surface-raised` (or `glass-raised`) within one frame | Same | none |
| Finger up on the same item | Tap | Ring appears on the item without travelling, then Open or Flip | Same | as Open or Flip |
| Finger slides off before lifting | Cancel | Fill returns; nothing happens | Same | none |
| Tap on a disabled item | Deny | Label shakes 4px twice, 160ms | `outline` border for 200ms | `deny` |
| Knob push, or tap, on an amount | Adjust | Row fill deepens to `amber-press` and stays; each detent moves the bar | Same, without the fade | `select`, then `tick` per step |
| Knob, detent that crosses a page of tiles | Step | The tile row slides one page, `dur-screen`; the ring travels with it | The row jumps one page | `tick` |
| Finger swipe across the tiles | Page | The tile row slides one page; the ring lands on its first tile | The row jumps one page | `tick` |
| Dial push, or key tap | Type | The character appears at the caret; the ring fill or the key flashes | Same, without the fade | `key` |
| Touch while the dial is shown, or knob while the keyboard is shown | Swap | Cross-fade to the other form, `dur-screen`; the field stays put | Cut | none |
| Phone link comes up | Connected | Status chip turns to green disc "Connected" | Same | `connected` |
| Phone link drops | Lost | Header chip turns to hollow ring "No phone connected"; the tile becomes "Last used" | Same | `lost` |
| A connecting attempt fails and the unit tries again | Retry | The line changes to "Still trying …"; the spinner keeps turning | Same | none |
| The unit gives up on a phone | Unreachable | The line changes to "Couldn't connect to …" for 3 seconds; Home returns with the tile marked by a red square and "Unreachable" | Same | `lost` |

Fast turning: when detents arrive closer together than `dur-step`, shorten the travel to 60ms; closer than 60ms, jump. Play at most 20 `tick` sounds per second.

## Sound

The iDrive knob already clicks in the hand, so sound confirms results more than movement. Sounds are short, dry and quiet; none is a melody except the two link sounds.

| Sound | Length | Recipe | Character |
|---|---|---|---|
| `tick` | 15ms | 1800 Hz sine, instant attack, exponential decay | A dry click; identical every time |
| `key` | 15ms | The `tick` sample | Typing; plays at Actions and All |
| `bump` | 70ms | Sine falling from 160 Hz to 90 Hz, exponential decay | A soft, low stop |
| `select` | 90ms | 660 Hz sine for 30ms, then 990 Hz for 60ms, each decaying | Two quick rising taps |
| `back` | 90ms | 990 Hz for 30ms, then 660 Hz for 60ms | `select` reversed |
| `on` | 60ms | 990 Hz sine, decaying | One higher tap |
| `off` | 60ms | 660 Hz sine, decaying | One lower tap |
| `deny` | 130ms | 200 Hz triangle, two 40ms pulses, 50ms apart | Two dull knocks |
| `connected` | 320ms | 660 Hz then 880 Hz sine, 160ms each, soft attack | A rising two-note chime |
| `lost` | 320ms | 880 Hz then 660 Hz | The same, falling |

Rules:

- Three levels in "Sound" → "Launcher sounds": **Off**, **Actions** (everything except the per-detent `tick`), **All**. Ship with Actions.
- Ship the sounds as pre-rendered 16-bit mono samples. Play them about 18 dB below the current listening level. Do not duck or pause music for them.
- A late sound is worse than none. Measure input-to-sound delay on the unit: above 50ms, remove `tick`; above 150ms, keep only `connected` and `lost`.
- No launcher sound plays while CarPlay owns the screen or during a call.
- Sound never carries information alone. Every sound has a visible counterpart in the table above.

As built (2026-11-23): `tools/design_sounds.py` renders the table into `launcher/sounds.c` (`make design`), 16-bit mono at 48 kHz, 18 dB below full scale. On the unit one `aplay` of the stock root is kept running and plays what the launcher writes to it, through the unit's own mixer (`plug:dmix`), so a sound lies over CarPlay's audio. The launcher plays nothing while CarPlay or the car has the screen. The level is kept with the unit's state. Forget uses `deny` when the phone could not be forgotten. Not yet heard: the bench has no speaker path, so the delay and the level are for the car session.

## When hardware runs short

Drop effects in this order, and stop as soon as the frame budget holds. The acknowledge beat is never dropped.

1. Wallpaper and glass fills (use Lite surfaces).
2. Screen cross-fade (keep the slide), then the slide (cut).
3. List scroll slide.
4. Ring travel (jump).
5. `focus-glow`.

What always remains: the ring moves and the pressed fill appears within one frame, the bump and deny answers exist, and one sound level can be chosen.
