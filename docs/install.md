# Installing, updating, going back

Everything here was done on the author's unit, over several sessions and
not always in this order. It has not been repeated from a factory-fresh
unit in one go. Have your capture ([stock-capture.md](stock-capture.md))
safe before you start, and keep the console connected for the first
install.

## What gets written

| Partition | By | With |
| --- | --- | --- |
| kernel, rootfs | the rescue installer | `RESCUE.IMG` |
| userdata | the running system, in a directory of its own (`autotop/`), beside stock's files | state archives, a kept app update |
| U-Boot's environment | you, at the U-Boot prompt, **optional** | two variables |
| bootlogo | a writer built for it, **optional** | your boot picture |
| MCU flash | `mcu/` over SWD, **optional** | two small patches |

S-Loader, U-Boot itself and the vendor's data partition are never written.

## First install

Put `dist/rescue/BOOT.BIN` and `RESCUE.IMG` ([build.md](build.md)) on a
FAT32 stick, stick in the **CARPLAY** socket. Power on, stop U-Boot at its
prompt:

    usb start
    mw.l 0x4000000 0 1
    fatload usb 0:1 0x4000000 RESCUE.IMG
    fatload usb 0:1 0x1000000 BOOT.BIN
    bootz 0x1000000

The first `fatload` takes about a minute (118,562,816 bytes); check both
byte counts. The installer runs from RAM, checks the image against its
hashes, compares it with the flash, arms its writer for the kernel and
rootfs partitions only, writes and verifies each block, and disarms. The
panel and the console show progress; the end is `AUTOTOP_RESCUE_DONE RESULT
PASS` after about five minutes. If power fails in between, do the same
again: U-Boot and the stick are all it needs.

Pull the stick and cycle power. Stock's own `bootcmd` starts the new kernel
from the flash. First start: the launcher's Home; pair a phone there.

A `RESCUE.IMG` made with `make rescue PLAN_ONLY=1` does everything but
write: a good first run.

## A stick that installs by itself (optional)

Two variables in U-Boot's stored environment make U-Boot try a stick in the
CARPLAY socket before the flash, so a rescue needs no console:

    setenv usbboot 'usb start; mw.l 0x1000024 0 1; mw.l 0x4000000 0 1; fatload usb 0:1 0x4000000 RESCUE.IMG; fatload usb 0:1 0x1000000 BOOT.BIN; bootz 0x1000000'
    setenv bootcmd 'run usbboot; run nandboot'
    printenv usbboot bootcmd
    saveenv

`saveenv` writes the environment partition: read what `printenv` shows
first. Without a stick, or with one that has no `BOOT.BIN`, the unit starts
from the flash as before. To undo: `setenv bootcmd 'run nandboot'`, `setenv
usbboot`, `saveenv`.

Then: stick in, power on, hands off. About a minute of loading, four to
five of installing, "DONE" on the panel; or, if the stick holds what the
flash holds, "already installed" after two minutes and nothing written.
Nothing on the stick is signed: whoever can reach that socket can start
their own software, as they could before with stock U-Boot.

## App updates

The launcher, its tools and the session scripts ("the app") update without
touching the image: Settings → System → System info → Software update →
Start. The unit shows a code that joins a phone to its Wi-Fi, then an
address to type into the phone's browser; choose the `.bundle` file
(`make app-bundle`) and send it. The unit checks the signature and version,
keeps the bundle on its userdata partition and restarts into it on trial.
If the new app does not confirm itself within two starts, the unit goes
back to what it ran before; the image's own copy is always there.

The same page has "Save this session's logs".

## The boot picture (optional)

U-Boot shows a 1280 × 480 baseline JPEG from the `bootlogo` partition, of at
most 188,530 bytes (`bootlogo_size` in its environment).
`tools/design_splash.py` draws one in the launcher's style,
`tools/splash_jpeg.py` makes the JPEG, and `make recovery-writer
WINDOW=0x7860000:0x80000` builds the one writer that may write that
partition, used with `tools/restore_manifest.py --bootlogo` and
`nand-restore` as below.

## Back to stock

From your capture, with the bounded writer:

    make recovery-writer WINDOW=0x160000:0x400000        # the kernel partition; 0x560000:0x6d00000 for rootfs
    tools/restore_manifest.py --source CAPTURE/A/mtd5.ecc --source-base 0x160000 \
        --window 0x160000:0x400000 --out DIR

Start the writer from the stick as the capture system is started
([stock-capture.md](stock-capture.md)), with `RESTORE.IMG` and
`RESTORE.MAN` on a stick in U DISK. In its shell:

    nand-restore plan /dev/mtdN /mnt/usb/RESTORE.IMG /mnt/usb/RESTORE.MAN     # N: the "autotop-write-window" in /proc/mtd
    echo 'arm 0x160000 0x400000' > /sys/bus/platform/devices/ec000000.nand/autotop_writer
    nand-restore execute /dev/mtdN /mnt/usb/RESTORE.IMG /mnt/usb/RESTORE.MAN

`plan` writes nothing and says what would change. A writer is built for one
window and refuses everything outside it. Kernel and rootfs were restored
this way on the author's unit. **userdata is not covered**: this firmware
leaves stock's files there in place, but if the unit ever had to rebuild
that partition (it does so by itself when it cannot be read), stock's files
in it are gone.
