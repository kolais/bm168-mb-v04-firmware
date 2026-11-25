# Screens

The launcher is a detour, not a home. The driver's two homes are CarPlay and the car's own BMW interface; the launcher appears when asked for, or when no phone connects by itself, and leaves as soon as its job is done.

## Flow

1. **Ignition on.** The BMW picture is shown. What follows depends on "Connect at ignition" (General, on by default) and on the car's driver profile:
   - **On, and the profile has a phone**: the unit connects that phone in the background. When CarPlay is up the picture switches to CarPlay by itself. Nothing of the launcher is drawn on the way.
   - **On, and the profile has no phone, is Guest, or is not known**: nothing connects. Home is ready behind the BMW picture and waits.
   - **Off**: nothing connects, whatever the profile. Home is ready behind the BMW picture and waits.
2. **Enter.** A touch on the screen while the BMW picture is shown opens the launcher on **Home**. A long press of MENU on the iDrive controller switches between the BMW picture and the unit's picture: CarPlay if a session is up, otherwise Home.
3. **Home.** Paired phones as tiles. Push a tile to connect to it; push the connected one to go to CarPlay.
4. **Connecting.** One line and a spinner for as long as the unit is trying the phone. On success the picture switches to CarPlay.
5. **Settings.** Reached from the cog. Lists only.
6. **Leave.** On Home the controller's Back key, or MENU, returns to the BMW picture. Home has no Back on the screen.
7. **From CarPlay.** CarPlay's car icon and a short press of MENU go to the BMW picture, not to the launcher. From there a touch opens Home, where the connected phone's tile leads back to CarPlay, and a long press of MENU returns to CarPlay directly.

## Driver profile

The car tells the unit which driver profile is active: the profile of the key that unlocked the car, or the one chosen in the BMW menu. A profile can have one phone.

- **A profile with a phone** connects it at ignition, when "Connect at ignition" is on.
- **A profile without a phone** waits on Home. The header chip says which: hollow ring and "No phone for profile 2". The phone chosen there becomes that profile's phone.
- **Guest** waits on Home with the chip "Guest". The guest chooses a phone or pairs one; the choice is used for this drive and never kept.
- **A profile the car has not announced** is treated as a profile without a phone, except that nothing is remembered.
- **Changing the profile in the BMW menu while driving**: to a profile with a phone, the unit switches to that phone through Connecting; to Guest, CarPlay ends and Home waits; to a profile without a phone, the running session stays.
- Choosing another phone on Home while the profile has one is for this drive only. The profile keeps its phone.
- Each phone's list in Paired phones has a **Driver profile** row that opens a list: "None", "Profile 1", "Profile 2", "Profile 3". Giving a phone a profile that another phone holds moves the profile to it.

## Which input leads

Every screen works with both the knob and touch, but each is shaped for one of them.

| Screen | Shaped for | Why |
|---|---|---|
| Home | Both equally | Tiles are large touch targets and a short knob run. |
| Connecting | Neither | Nothing to operate. |
| Settings lists, confirmations | The knob | Used on the move, eyes mostly on the road: one column, one ring, the car's own list pattern. Touch works on every row but nothing depends on it. |
| Text entry | Whichever hand opened it | Dial for the knob, keyboard for touch. |
| Software update | Touch, while stationary | Done parked, reading the screen, with the phone in hand. Running text and large buttons are acceptable here and nowhere else. |

When a new screen is added, decide which row it belongs to first. A knob-led screen is a list. A touch-led screen may use text and buttons, and must only be reachable for a task done while parked.

## Home

- **Top band**, the same on every launcher screen: at the right Wi-Fi, Bluetooth and the clock, and nothing else that is fixed.
- **The cog**: a 60px circle under the tiles on the driver's side, on Home only. The status, the outer edge of the last tile and the cog end on one line, `space-5` from the screen's edge; there is no empty strip beside the tiles.
- **No Back on the screen.** The driver came here from the BMW picture and leaves with the controller. A "car" button like CarPlay's may be added later.
- **In the band on Home**: `title-36` "Phones" at the left, and before the Wi-Fi mark one status chip: green disc and "Anna's iPhone connected", or hollow ring and "No phone connected". While Home waits for a choice the hollow-ring chip gives the reason instead: "No phone for profile 2" or "Guest" (see Driver profile).
- **Tiles**: 264 × 320, `radius-lg`, four to a page with `space-3` between them, in reading order, from 64px to 384px down the screen. A page that is not full stands at the driver's side: with right-hand drive its tiles take the last places, above the cog. Each has a 56px phone icon at the top and, at the bottom, the phone's name in `name-28` (two lines at most, then an ellipsis) and one state word in `label-20`.
- A phone is in exactly one state. When more than one applies, the first in this list wins:
  - **Connected**: CarPlay is linked to it now. The tile is filled `blue`. At most one.
  - **Unreachable**: the last attempt to connect to it failed. A red square before the word. Clears at the next attempt or the next ignition.
  - **Last used**: the phone most recently connected, when it is not connected now; the word is `ink` where "Paired" is `ink-muted`. At most one. It is a label and a starting point for the ring; the phone tried at ignition is the driver profile's.
  - **Paired**: known, and none of the above.
- **Pair a phone** is always the last tile, with a dotted `outline` border and a plus. It exists even when no phone is paired, so Home is never empty.
- **Page dots** sit centred under the tiles: 8px discs 12px apart, `ink` for the current page, `outline` for the rest. They show only position; they are not controls. Hide them when there is one page.

Moving around:

- The knob moves the ring tile by tile. Crossing a page edge slides the row one whole page. Past the first tile, the ring goes to the cog; turning the other way past "Pair a phone" bumps. With right-hand drive the cog is on the right and the order mirrors, so the cog is always reached past the tile nearest to it.
- When Home opens, the ring is on the connected phone; if none, on the active profile's phone; if none, on the last used phone; if none, on the first tile.
- Touch: tap a tile to choose it; swipe the row sideways to change page; tap the cog for Settings.

## Connecting

- A 136px spinner with a 56px phone icon inside it, and one `title-36` line beneath: "Connecting to Work iPhone". Nothing else in the content area.
- The spinner is a quarter arc in `blue` (blue is the phone) on a faint `outline` track, turning once per second. It turns smoothly in both modes: it is small, so both can afford it. (Lite stepped it twelve times per turn until the owner saw it at the bench.)
- Nothing takes the ring. Under the line stands **Cancel**, the one button on the screen and so the system's solid one: a pill filled `ink` with dark words, 64px high, at least 200px wide; the knob's Back does the same. Both return to Home. Cancel is there while the unit is trying or pairing, not on "Couldn't connect" and not during an update.
- The screen has no time limit of its own. It stays for as long as the unit is trying the phone and follows the unit's attempts: a connection usually takes 15 to 20 seconds, and longer when an attempt has to be repeated.
- When an attempt fails and the unit tries again, the line changes to "Still trying Work iPhone". The spinner keeps turning.
- Success: the `connected` chime, the content fades to `ground`, the picture switches to CarPlay.
- Failure, when the unit has given up on the phone: the blue arc goes and the track stays (a turning arc says "working"), the line changes to "Couldn't connect to Work iPhone" for 3 seconds, the `lost` sound plays, and Home returns with the ring on that tile, now marked "Unreachable".
- Pairing uses the same screen with a different line: "Choose “BMW 28114” in your iPhone's Bluetooth settings".
- The line is one line. When it is too long the name is cut with an ellipsis; the words around it never are.

## Over CarPlay

While CarPlay has the screen the launcher draws one thing only, the switch banner.

- **When**: CarPlay has just come up and another phone is paired. With more than two phones the banner offers the one used before this one.
- **What**: a `surface` card, 1000 × 112, with `edge-light`, centred 16px below the top edge. Its ends are half-circles, so the button's round end keeps the same 24px from the card's edge all the way round. On the left a 56px phone mark and the current phone's name in `name-28`; on the right what a tap does, drawn as the system's solid button: 64px high, filled `ink`, "Switch to Work iPhone" in `name-28`. The card is opaque in both modes: CarPlay moves behind it, and nothing may move behind glass. A long name is cut with an ellipsis; "Switch to" never is. (As first built it was 820 × 96 with a quiet `surface-raised` pill; at the bench on 2026-11-22 the owner found it small, took the pill for greyed text and tapped the middle of the banner.)
- **How long**: 8 seconds, then it goes without a trace. In Full it fades over `dur-screen`; in Lite it cuts.
- **Input**: touch only. A finger on the banner or within 24px of it is the banner's; everywhere else it is CarPlay's. The whole banner is the switch: a tap anywhere on it switches phones through Connecting. A tap in the 24px around it sends the banner away and goes no further. The knob belongs to CarPlay while CarPlay is in front, so the banner never takes the amber ring, and nothing on it is `blue`: it leads away from the phone that is in CarPlay now.
- "Offer other phone in CarPlay" in General turns it off. It is never shown under the Guest profile.

## Settings

Settings is a list of four groups; each opens a list. As in the car's own settings there is no help text: no summary under a group and no explanation panel. A label must say what the row does on its own. Lists run across the screen, as the car's own: the label at the left, the value or the amount at the right. Every list ends in a **Back** row, a chevron and the word: the way up by touch, and one more stop for the knob, which has its Back key too. A confirmation has "Cancel" for it. A finger on the row that has the ring changes nothing until it lifts.

| Group | Rows |
|---|---|
| General | Name, Connect at ignition, Offer other phone in CarPlay, Paired phones |
| Display | Brightness, Steering side, Visual effects |
| Sound | Media volume, Call volume, Navigation volume, Launcher sounds, Equaliser (Bass, Mid, Treble) |
| System | System info (Kernel, Processor, Panel, Software update), Developer access (only in an image built with a developer's key; Off unless switched on), Reboot |

Row kinds:

- **Opens a list**: chevron on the right, no value unless it is a real setting ("Equaliser · Flat").
- **Choice**: the value on the right; a push steps to the next value ("On" → "Off", "Left" → "Right", "Off" → "Actions" → "All"). Use this for two or three values; more than three opens a list.
- **Amount**: the number, then a minus, a bar and a plus as one tight group at the row's right edge, the arrangement of the car's own tone settings. The columns are fixed from the right (plus 60px, bar 200px, minus 60px, then the number in 120px, right-aligned), so every bar on a screen starts and ends at the same x whatever the label. An amount that runs either side of zero (bass, treble) fills from the middle of the bar and has a centre mark; its number carries its sign ("+2 dB", "−3 dB", "0 dB"). With the knob: push to start adjusting (the row fill deepens to `amber-press`), turn to change, push or Back to keep. With touch: tap minus or plus. The change applies as it is made.
- **Text**: the current text on the right; a push or tap opens text entry.
- **Action**: a verb and no value ("Connect", "Disconnect"); a push does it.
- **Information**: a value and no chevron; pushing it bumps.
- **Confirmed action**: anything that cannot be undone takes two deliberate steps. First a row that names the action ("Reboot", "Forget this phone"). Pushing it opens a two-row list titled with the action and what it affects: "Cancel" first with the ring on it, then the action as "… now". The row that names the action never performs it, and a row that names only a thing (a phone) never leads straight to the confirmation.
  - Reboot: System → "Reboot" → list "Reboot": "Cancel", "Reboot now".
  - Forgetting a phone: General → Paired phones → the phone → its own list → "Forget this phone" → list "Forget Work iPhone": "Cancel", "Forget now". Afterwards the list of phones returns without it and its tile is gone from Home. Forgetting the connected phone ends its CarPlay session. With no phones left, the list shows one information row, "No phones paired".

"Name" is one text row. It sets the name the unit shows to phones over both Bluetooth and Wi-Fi; there are not two names to keep in step.

"Connect at ignition" is a choice, "On" or "Off", and ships "On". Off means the unit connects no phone by itself: Home waits behind the BMW picture until the driver brings it in and pushes a tile (see Flow).

"Offer other phone in CarPlay" is a choice, "On" or "Off", and ships "On". It controls the switch banner (see Over CarPlay).

Each phone in Paired phones opens a list titled with its name. The ring starts on Connect (or Disconnect), not on Status, so one push does the likely thing:

- **Status**: the same word as on its tile: "Connected", "Unreachable", "Last used" or "Paired". Information only. The Paired phones list shows the same word beside each name.
- **Connect**, or **Disconnect** when this is the connected phone. Connect goes to the Connecting screen, as its tile on Home does. Disconnect acts at once with the `off` sound, the row becomes "Connect" and Status becomes "Last used"; it can be undone with one push, so it is not confirmed.
- **Driver profile**: the profile this phone belongs to, or "None". Opens a list of four (see Driver profile), in which the current one carries a check mark and the ring; a push on another chooses it and returns.
- **Forget this phone**: confirmed, as above. Forgetting a phone frees its profile. Back in the list its row is still there for a beat, dimmed, and closes while the rows below move up into the ring (Lite: it stands a third of a second and is gone); the `off` sound plays when the unit has dropped it. If the unit could not drop it, its row is back, the ring nudges, `deny` sounds and a line in the top band says "Couldn’t forget …" for three seconds: the one place a message is used.

"Steering side" moves the cog, and the side the tiles stand on, at once. "Visual effects" switches between Full and Lite at once.

## Instruction page

Used when an action needs explaining before it starts and is done while parked. Software update, under System info, is the first.

- `title-36` in the header band, as on every screen.
- Up to four numbered steps in `body-24`, `ink`, in a column no wider than 860px. Each step is one sentence.
- Two buttons at the bottom on the driver's side, `space-4` apart: **Cancel**, then **Start**, filled `ink`. They are pills sized for a finger: 64px high and at least 200px wide, larger than the 48px pills elsewhere. Neither is `blue`; blue is for the phone.
- This page is touch-led. The knob still works so nobody is stranded: the ring starts on Cancel, the knob moves between the two buttons and bumps at either end, push does the focused one, and Back is the same as Cancel.
- If the car's speed is available to the unit, the Software update row is disabled while the car is moving. If the car starts to move while the page is open, the page closes to the list it came from.
- Start on Software update opens the receiving screen (below).

These two are the only launcher screens with running text. Do not use them where a list would do.

## Receiving screen

Software update comes over the unit's own Wi-Fi, from the driver's phone (owner, 2026-11-23). After Start the unit listens, and says how to reach it:

It has two steps (owner, 2026-11-24):

- **Step 1, the Wi-Fi.** A code for the phone's camera at the left (216px, dark on a white card with a 12px margin whatever the theme) joins the unit's network without typing. Beside it in `body-24`: "Point the phone's camera at the code and join the Wi-Fi"; under it in `label-20` `ink-muted` the network's name and password for joining by hand, and that the next step follows when the phone has joined. Two 64px pills as on the instruction page: **Cancel** and **Next**, filled `ink`, with the ring on Next. The unit goes on by itself when a phone joins; Next is for a phone that was on the Wi-Fi already (the one in CarPlay is).
- **Step 2, the address.** No code: the address is plain HTTP, and a phone's browser opens that only when it is typed, never from a code (tried: "the request was for an HTTP URL with HTTPS-Only enabled"). So the address stands in `display-48`, with its "http://", under "In the phone's browser, type this address"; then "Choose the update file there and send it", and one line in `name-28` that says what the unit is doing: "Waiting for the update file", "Receiving the file: 43%", "Checking the file", "Not taken: the signature is not ours". A file that is not taken leaves the screen as it is: another may follow. One pill, **Cancel**, with the ring.
- Cancel, by push, Back or a finger, ends the listening and returns to the list the instruction page was opened from.
- A file that is taken shows the spinner screen with the line "Updating. Keep the ignition on". Back does nothing there. The unit restarts and runs the new software.
- Nothing listens before Start, or after this screen is left for any reason (Cancel, CarPlay coming up, the car taking the picture).

## Text entry

One screen, two forms, chosen by the hand that opened it.

- Opened with a knob push: the **dial**. Opened with a tap: the **keyboard**.
- It keeps adapting. A touch anywhere while the dial is shown swaps to the keyboard; any knob turn or push while the keyboard is shown swaps to the dial. The swap types nothing and keeps the text.
- The field sits in the header band beside the title in both forms, so it does not move when they swap.

Dial:

- Characters stand on a full circle of radius 180px: A at the lower left, clockwise to Z at the lower right, in `name-28`, `ink-muted`; the one under the ring is `ink`, at the same weight. The five functions sit along the bottom with wider spacing, in `label-20`: numbers, case, delete, space, OK. The dial opens on capitals.
- The characters do not move. The amber ring (52px disc, `amber-wash` fill) travels round the circle, `dur-step` per detent. The centre of the dial repeats the focused character at `display-48`, and names a function ("Delete", "Done").
- Turn to move, push to type. The dial wraps round; it is the one place focus does.
- "Numbers" replaces the letters with digits and punctuation; "Capitals" and "Small letters" switch case.

Keyboard:

- QWERTY in four rows. Letter keys are 103 × 60 with `space-2` between, `radius-sm`, `surface-raised`, `row-32` characters. Shift, delete, "123" and space are `surface`; "Done" is filled `ink`.
- A key darkens to `outline` while the finger is down and types on release.
- "123" swaps to a layer of digits and punctuation. That layer has no capitals: where shift stands there is an empty key. The keyboard opens on small letters; shift stays on until it is tapped again.

Both:

- Each character typed or deleted plays `key`. Deleting from an empty field bumps; typing into a full one denies.
- "OK" or "Done" saves and returns to the list. The knob's Back, or **Cancel** (a pill after the field), leaves without saving. An empty field saves nothing: the name stays as it was.
- A name has 24 characters at most: letters, digits, the space, and the marks on the dial and the keyboard.
