# local/: yours

Everything the build needs from you and nothing of it is ever committed
(`.gitignore` keeps all of `local/` out but this file).

| Path | What | From |
| --- | --- | --- |
| `stock/rootfs.ubi` | your unit's stock rootfs partition, ECC-corrected (`mtd6.ecc` of a capture) | docs/stock-capture.md |
| `stock/userdata.ubi` | your unit's userdata partition (`mtd7.ecc`) | docs/stock-capture.md |
| `stock/gd32-flash.bin` | your unit's MCU flash (only for `mcu/`) | SWD |
| `keys/update-key.pem` | public half of your update signing key | `make keys` |
| `keys/dev-authorized_keys` | public half of your developer shell key (optional) | `make keys` |
| `state/PLANA.CFG`, `DATA.TAR`, `GOC.TAR` | your unit's session files | `make session` |
| `assets/carplay-icon@2x.png` | your own picture for the car's tile in CarPlay (120 × 120; optional) | you |
| `icons/carplay.svg` | your own mark for a running session (an SVG body on a 24-unit grid; optional; then `tools/design_icons.py`) | you |
| `assets/splash/` | your own boot picture (`tools/design_splash.py`, `tools/splash_jpeg.py`; optional) | you |
