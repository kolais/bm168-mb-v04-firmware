# Licences, other people's work, and names

## This repository

Everything here is under the MIT licence (`LICENSE`), with two exceptions:

- **Kernel changes** (`recovery/patches*/`, the device trees `recovery/*.dts*`
  and the kernel configurations `recovery/*.config`) are changes to the Linux
  kernel and are under **GPL-2.0-only** (`LICENSES/GPL-2.0.txt`), as the
  kernel is.
- `recovery/bm168_nand_format.h` is compiled into both the kernel and our own
  tools and is under **GPL-2.0 OR MIT**, as its first line says.

## What the build downloads (pinned in `sources.lock.json`, not copied here)

| What | From | Licence |
| --- | --- | --- |
| Linux 4.19 kernel with the SoC vendor's changes | `yogihybo/linux-arkmicro` on GitHub, kernel only, after the cleaning in `board/sdk-sanitization.json` | GPL-2.0 |
| RTL8821CS Wi-Fi driver | `rockchip-linux/kernel` on GitHub | GPL-2.0 |
| LVGL 9.2.2 | lvgl.io | MIT |
| Inter 4.1 (the typeface, compiled into the launcher) | rsms.me/inter | SIL Open Font License 1.1 |
| Dropbear 2025.89 (the developer shell's SSH server) | matt.ucc.asn.au/dropbear | MIT-style, see its `LICENSE` |
| Bootlin toolchains | toolchains.bootlin.com | GPL and others (build tools only) |

## Included here

- **Icons** (`launcher/ui/icons.c`, drawn by `tools/design_icons.py`): outlines
  from Lucide (lucide.dev), ISC licence, © Lucide contributors.
- **Word list** (`launcher/ap_words.h`): the Electronic Frontier Foundation's
  "short word list #1" (eff.org/dice), CC BY 3.0 US.

## What is not here and never will be

The unit's stock software: the vendor's Linux programs and libraries (among
them the CarPlay and Bluetooth stacks), its U-Boot, and its MCU firmware.
The build takes them from a capture of **your own unit** and checks them
against a list of file names and SHA-256 hashes (`board/stock-files.sha256`).
The MCU patches (`mcu/`) are our own code plus the addresses it is put at;
they are applied to your own dump of the MCU's flash.

## Names

This is an independent hobby project. It is not affiliated with, endorsed by
or supported by BMW, Apple, the unit's maker or seller, or the SoC's maker.
BMW, CarPlay and the other names used to say what the hardware is and what it
connects to belong to their owners. The word "autotop" in file names, paths
and messages comes from the name the unit is sold under and is used only to
tell this unit's files apart; it implies no connection.

There is no warranty. This replaces the software of a device fitted in a car;
it can leave the device unusable. Read `README.md` before doing anything.
