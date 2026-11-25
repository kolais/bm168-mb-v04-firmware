# Capturing your own unit

The build needs your unit's stock software, and the way back to stock needs
all of its flash. Both come from one capture, made by a small Linux system
that runs from RAM and cannot write the flash (its NAND driver refuses
erase and program).

You need: the console ([hardware.md](hardware.md)), one FAT32 stick that
U-Boot reads, and this repository's first build step.

## 1. Build the capture system

    make recovery-capture

gives `dist/recovery-capture/autotop-recovery.bin` (a kernel with its own
small root in RAM, about 4 MB). This build needs no stock software.

## 2. Prepare the stick

Copy the file to the stick as `CAPTURE.BIN`. Create a text file
`AUTOTOP.CAP` on it whose first line is exactly

    AUTOTOP BM168 CAPTURE MEDIUM v1

Only a stick carrying that marker is mounted read-write by the capture
system, and it only ever creates new files under `CAPTURE/`. Have 300 MB
free.

## 3. Start it

Stick in the **CARPLAY** socket. Power on, stop U-Boot at its prompt, then:

    usb start
    fatload usb 0:1 0x1000000 CAPTURE.BIN
    bootz 0x1000000

Check the byte count `fatload` prints against the file's size before
`bootz`. **Before `bootz`, move the stick to the U DISK socket** (no power
cycle): U-Boot reads only CARPLAY, the capture system only U DISK.

Nothing is written to the unit by this: no `saveenv`, no flash command. It
ends at a shell after `AUTOTOP_CAPTURE_RECOVERY_READY`, having said `Capture
medium verified and mounted read-write at /mnt/usb`.

## 4. Capture

For each partition N from 0 to 13:

    nand-capture /dev/mtdNro /mnt/usb/CAPTURE/A/mtdN

Each writes `mtdN.raw` (pages with their spare bytes, as read), `mtdN.ecc`
(the data after error correction) and `mtdN.log`. Exit status 0 is a clean
read; 2 means a page could not be corrected (stop and look); 1 is a failure.
Then the same again into `CAPTURE/B`. Finish with

    sync
    umount /mnt/usb

and cycle power: the unit starts its stock software as before.

## 5. On the computer

Copy `CAPTURE/` somewhere safe, twice. Check that every `.ecc` file of pass
A equals its pass B twin. Then

    mkdir -p local/stock
    cp CAPTURE/A/mtd6.ecc local/stock/rootfs.ubi
    cp CAPTURE/A/mtd7.ecc local/stock/userdata.ubi

`make CAPTURE=<dir with the .raw files renamed mtdN-name.raw-oob.bin> ecc-check`
runs the ECC model against your raw pages; `tools/nand_ecc.py --help` says
what it checks.

The build judges `rootfs.ubi` file by file against the stock version it was
made for (`board/stock-files.sha256`, `tools/stock_manifest.py`). Files a
running unit writes to may differ; a program or library that differs means
another stock version, and the build stops.

## Keep the capture

It is your only way back to stock, and it holds things that are your
unit's alone (pairings, keys of the CarPlay stack). Do not publish it.
