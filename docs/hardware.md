# The board

What is known about the BM168-MB-V04 unit this was done on. Check each point
against your own board before relying on it.

## Parts

| Part | What |
| --- | --- |
| SoC | Arkmicro ARK1668, on a plug-in compute module (`CM_BM168_V04`); 180 MB of RAM used by Linux |
| Flash | 128 MiB SLC NAND, 2,048-byte pages, 64-byte spare, 128 KiB blocks |
| MCU | GigaDevice GD32F305RBT6: power, CAN to the car, the picture switch, backlight, keys; talks to the SoC on its `ttyS2` at 115200 |
| Display processor | TW8836: switches between the car's picture and the SoC's, draws the "no signal" text |
| Wi-Fi / Bluetooth | RTL8821CS (SDIO) for the access point; Bluetooth is driven by the vendor's `gocsdk` |
| Audio | SoC DAC/ADC with a ROHM BD37033 sound processor |
| Panel | 1280 × 480, capacitive touch (Goodix) |

## Flash layout

| mtd | Name | Start | Size |
| --- | --- | --- | --- |
| 0 | S-Loader | `0x0` | first-stage loader, its own ECC format |
| 1 | U-boot | | |
| 2, 3 | U-boot-Env and its copy | `0xa0000` | |
| 4 | arkdata | `0x140000` | |
| 5 | kernel | `0x160000` | `0x400000` |
| 6 | rootfs | `0x560000` | `0x6d00000` (UBI) |
| 7 | userdata | `0x7260000` | `0x600000` (UBI, UBIFS; about 3 MB usable) |
| 8 | bootlogo | `0x7860000` | `0x80000`: a 1280 × 480 baseline JPEG that U-Boot shows |
| 9… | bootanimation and others | | |

The ECC of the normal partitions is described in
[`tools/nand_ecc.py`](../tools/nand_ecc.py) (BCH over GF(2^14), t = 7, two
1,024-byte steps per page). This firmware writes flash only through its own
bounded writer in the kernel, in the stock format, and only inside a window
fixed at build time (`recovery/bm168_nand_format.h`).

## Console

A four-hole header on the main board, above the compute module, has two
holes marked `RX` and `TX`. 3.3 V logic, 115200 8N1, no flow control; take
ground from a marked `GND`. Connect the adapter's ground, its RX to the
board's TX and its TX to the board's RX; do not connect the adapter's power
pin. Stock prints its boot and gives a root shell there; U-Boot (2012.10)
stops at its prompt if it receives a key during the first second or two
after its banner.

`tools/bench/console.py` is the helper used here (it refuses U-Boot commands
outside a short read-only list and checks a loaded image's size and CRC
before starting it). Set `AUTOTOP_UART` to your adapter's device.

## USB sockets

| Socket | Use |
| --- | --- |
| `CARPLAY` | USB0. **The only socket U-Boot reads.** Stock U-Boot also looks there at every start for vendor update files (`uboot-env.bin`, `update`): never leave files of those names on a stick. Under this firmware's kernel the socket is the phone's (device role); Linux does not see a stick there. |
| `U DISK` | USB1. Linux reads sticks there. U-Boot does not. |

Sticks: FAT32 with an MBR partition table, 8.3 file names in capitals. Not
every stick works in U-Boot; a small plain one did, and reads high up on one
4 GB stick hung U-Boot until power was cycled.

## Power

12 V on the display's harness. On a bench, 0.7 to 0.8 A while running. The
MCU stays powered while the car is off and switches the SoC.

## The picture switch

After a cold start the MCU shows the car's picture. The SoC asks for its own
with the MCU frame `AA 55 00 04 4C 00 00 B0`; the launcher sends it when it
has something to show. With no car connected the panel shows the MCU's own
"BMW / Check Signal" text until then.

## MCU

SWD on a four-hole header marked `GND`, `CLK`, `DIO`, `3V3` (the boards have
more than one such header: read the part's ID before anything else, and do
not erase or unlock anything). The flash is
128 KiB: a boot loader in the first 8 KiB, the application after it, two
settings pages at the end. `mcu/` holds two optional patches (the car's
driver profile reported to the SoC; the "Check Signal" text the right way
round) and the tool that writes them: it has the MCU write a page from its
own RAM, so no debugger transfer happens while a page is incomplete.
They apply only to an MCU dump with the same hash as the one they were made
on; the tool checks.
