/* SPDX-License-Identifier: MIT
 * Running the launcher's screens without the unit: a display in memory, a
 * clock the caller moves, and a finger. For the snapshot tool and the tests.
 */
#ifndef UI_HOST_H
#define UI_HOST_H
#include "lvgl.h"

int host_init(void);			/* LVGL, the display, the theme and the frame; 0 = ok */
void host_run(int ms);			/* let this much time pass, in 10 ms frames */
void host_tap(int x, int y);		/* finger down at (x, y), then up */
void host_draw(void);			/* draw what is pending now */
/* The panel as a picture: the layer over black, or over a stand-in for
 * CarPlay's picture (a blue gradient); 0 = ok. */
int host_save_ppm(const char *path, int over_carplay);
int host_drawn_pixels(void);		/* pixels of the layer that are not fully transparent */
#endif
