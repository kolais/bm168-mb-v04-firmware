# bm168-mb-v04-firmware

Replacement firmware for an aftermarket CarPlay display for BMW built on the
**BM168-MB-V04** board (Arkmicro ARK1668 SoC, GD32 MCU, 10.25" 1280 × 480
panel), sold under the AUTOTOP name among others.

It keeps the vendor's CarPlay and Bluetooth programs, taken from your own
unit, and replaces what is around them:

- a current Linux kernel build (4.19, from the SoC vendor's published tree)
  in place of the stock 3.4, with the flash read-only except for one small
  state area;
- a new launcher (LVGL): CarPlay first, phones chosen by tile or by the
  car's driver profile, a passenger's phone kept off hands-free and off the
  unit's Wi-Fi, settings, the unit's own name;
- app updates over the unit's own Wi-Fi from a phone's browser, signed with
  your key, with an automatic way back if the new one does not start;
- a rescue stick that reinstalls kernel and root filesystem by itself;
- an optional developer shell over the unit's Wi-Fi;
- optional small patches to the MCU firmware.

## Read this first

- **It has been done on one unit**: the author's, stock version
  `BM168_V3.1.21-20230216`. The build refuses a capture of another stock
  version. Nothing here has been tried on a second unit.
- **You need a serial console** on the board (3.3 V UART, soldered or
  clipped) to capture your unit's stock software and to start the first
  install. The MCU patches need SWD. If that is not something you do, this
  is not for you yet.
- **It can leave the unit unusable.** Keep the captures of your own unit:
  they are the only way back to stock. There is no warranty of any kind.
- **No vendor software is in this repository.** You capture it from your
  own unit; see [docs/stock-capture.md](docs/stock-capture.md).

## From a stock unit to this firmware

1. Wire the console and get to know the board: [docs/hardware.md](docs/hardware.md).
2. Capture your unit's flash: [docs/stock-capture.md](docs/stock-capture.md).
3. Make your keys and build: [docs/build.md](docs/build.md).
4. Install with the rescue stick: [docs/install.md](docs/install.md).
5. Afterwards: updates over Wi-Fi and the developer shell,
   [docs/install.md](docs/install.md#app-updates) and
   [docs/developer-access.md](docs/developer-access.md).

## What is where

| Path | What |
| --- | --- |
| `launcher/` | the launcher: `launcherd.c` (what it decides), `unit.c` (the unit it runs on), `ui/` (LVGL screens), tests |
| `design/` | the launcher's design: tokens, screens, components |
| `recovery/` | kernel patches, device trees and configurations; the installer, writer and capture systems; `plan-a/`: the image's start-up, state, update and developer-access scripts |
| `tools/` | the build, image and bundle tools; `bench/`: console and bench power supply helpers |
| `mcu/` | the MCU patches and the tool that writes them over SWD |
| `board/` | the board's facts, the stock file list, and how the published vendor tree is cleaned before use |
| `local/` | **yours**: keys, captures, session files, your own artwork. Never committed. |

Comments in the code often name a file under `docs/` that is not here:
those are the author's working notes ([docs/NOTES.md](docs/NOTES.md)).

## Tests

    make ui-test          launcher scenarios and navigation, on the host
    make system-test      the image's start-up scripts and a real log-in to the developer shell
    make app-slot-test    app updates: every path of install, trial and fallback
    make rescue-test      the rescue installer against a model of the flash
    launcher/test_update.sh, launcher/test_passphrase.sh

The last four need one image build first (they run the unit's own shell
from your capture under emulation).

## Licence

MIT for our own code, GPL-2.0 for the kernel changes; details, third-party
work and a note on names in [NOTICE.md](NOTICE.md).
