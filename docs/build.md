# Building

Requirements: Docker that can run Linux/AMD64 images (Docker Desktop on
Apple Silicon does, by emulation), Make, Python 3.11 or newer, OpenSSL, git,
curl, and the internet for the first downloads (each pinned by hash in
`sources.lock.json` or `board/sdk-sanitization.json`). Builds run in a container with no network; scratch
files live in a Docker volume.

## Once

    make keys        your update signing key and developer shell key
    make session     your unit's session files, from local/stock/

`make keys` puts the private halves in `~/.config/bm168-mb-v04/` and the
public halves in `local/keys/`. **Back the private halves up.** An image
takes only app updates signed with the key it was built with, and lets in
only the shell key it was built with. For an image without any developer
access: `DEV_ACCESS=0 make keys`.

`make session` writes `local/state/`: `DATA.TAR` and `GOC.TAR` (your unit's
CarPlay and Bluetooth data as stock left them) and `PLANA.CFG`. Edit
`PLANA.CFG`:

| Line | Meaning |
| --- | --- |
| `AP_SSID=` | the unit's Wi-Fi name until a name is set on the unit |
| `AP_PSK=` | its first Wi-Fi passphrase (the unit makes new ones by itself) |
| `LAUNCHER=autotop` | run this repository's launcher |
| `PROFILE_PHONES=11:1 00:2` | optional: the car's driver profile (as the MCU patch reports it) to the place of a phone in the pairing list |
| `CONNECT_AT_IGNITION=`, `OFFER_OTHER_PHONE=`, `VISUAL_EFFECTS=` | optional: 0 or 1, the settings' first values |

## The image

    make plana-b-state

This fetches the published kernel tree and cleans it (`board/sdk-*`,
`tools/sanitize_sdk.py`), builds the kernel, the Wi-Fi driver, the launcher
and tools, and makes the root filesystem from your stock capture plus this
repository. In `dist/plana-b-state/`:

| File | What |
| --- | --- |
| `plana-b.bin` | the kernel as U-Boot starts it |
| `stock-rootfs.sqfs` | the root filesystem |
| `rescue.bin` | the rescue installer |
| `app/` | the part an app update replaces |
| `manifest.json` | what went in, by hash |

Then

    make rescue NOTE="my first build"

gives the two files of the rescue stick in `dist/rescue/`: `BOOT.BIN` and
`RESCUE.IMG`. `RESCUE.IMG` contains your session files: treat it like them.

## App updates

    make app-bundle VERSION=9

signs `dist/plana-b-state/app/` into `dist/autotop-app-9.bundle`. The number
only grows, and it must be larger than `recovery/plan-a/app-version`, the
version built into the image. What is "app" and what is "system" is
explained at the top of `recovery/plan-a/app-slot`.

## Your own artwork

The defaults carry no name and no mark. Yours go in `local/`
([local/README.md](../local/README.md)).

## Reproducibility

The same inputs give the same bytes: timestamps are pinned, and the kernel,
root filesystem and installer are compared by hash between builds here.
