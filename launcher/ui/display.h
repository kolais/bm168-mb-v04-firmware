/* SPDX-License-Identifier: MIT
 * The launcher's display: LVGL draws each changed area into a buffer of its
 * own, and the finished area is copied into the layer in one pass. The layer
 * that is scanned out never holds a half-drawn widget (the old UI let LVGL
 * draw straight into it).
 *
 * The same code serves the unit (the layer is the mapped framebuffer) and
 * the host tools (the layer is memory), so pictures and tests come from the
 * code that ships.
 */
#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H
#include <stdint.h>

/* layer: 1280 x 480 ARGB8888, 5120 bytes a row. now_ms: the clock LVGL runs
 * by. vsync: called before a full-screen copy, to start it at the top of a
 * frame (NULL: none). Sets up LVGL, the touch input, the fonts and the
 * frame; 0 = ok. */
int ui_display_init(uint8_t *layer, uint32_t (*now_ms)(void), void (*vsync)(void));

/* A screen that arrives by moving (Full; design/modes.md). The unit's display
 * controller has two GUI layers over one memory, and can show one screen
 * leaving as another arrives by changing registers alone, 60 times a second
 * (docs/plan-a-gpu-display-spike.md, "Display-layer spike"); the launcher
 * cannot redraw a whole screen that fast. So `layer` then holds two screens,
 * one after the other, and `move` shows screen `to` in place of `from` and
 * returns when it rests there. Without it (the host tools, a unit that
 * cannot) every change is a cut. */
enum ui_change { UI_CHANGE_CUT, UI_CHANGE_FORWARD, UI_CHANGE_BACK };
void ui_display_moves(void (*move)(enum ui_change how, int from, int to));
/* The screen that is built next arrives so: it is drawn whole, out of sight,
 * and moved in. */
void ui_display_change(enum ui_change how);

/* The launcher's whole picture fades away over what lies under its layer
 * (CarPlay's video), by the layer's own alpha: ui_display_fade(1) returns
 * when it is clear to the eye; the caller then draws the layer clear and
 * ends with ui_display_fade(0), after which the layer shows by its pixels
 * again. `fade` is the unit's way to do it; without one, nothing happens. */
void ui_display_fades(void (*fade)(int out));
void ui_display_fade(int out);
/* Run LVGL's timers (drawing, touch, animations); returns ms until it wants
 * to run again. */
int ui_display_tick(void);
void ui_display_draw_now(void);		/* draw what is pending before the caller goes on */
void ui_display_touch(int x, int y, int down);	/* the finger, as last seen */

/* Measurements, for the log. */
struct ui_display_stats {
	unsigned copies;	/* areas copied since the start */
	unsigned full;		/* of them, full-screen */
	unsigned long pixels;	/* pixels copied */
	unsigned forward, back;	/* screens asked to arrive by a push, each way */
	unsigned moved;		/* of them, moved by the display's layers */
	unsigned fades;		/* times the picture was asked to fade away */
	/* The last screen that was moved in: from the request to the picture
	 * drawn, its copy into the layer memory, and the move itself. */
	unsigned draw_ms, copy_ms, move_ms;
};
void ui_display_stats(struct ui_display_stats *s);
#endif
