/* SPDX-License-Identifier: MIT
 * The launcher's look (design/README.md): colours, type and fills, with the
 * values of design/tokens.json (tokens.h, generated).
 *
 * Two render modes (design/modes.md): Lite is the design, Full only adds to
 * it, and "Visual effects" switches between them at run time. So a screen is
 * written once and never asks which mode is on: everything that differs
 * (glass fills, ring travel, later the wallpaper, glow and fades) is decided
 * here, behind ui_fill(), ui_move() and their kin.
 */
#ifndef UI_THEME_H
#define UI_THEME_H
#include "lvgl.h"
#include "tokens.h"

enum ui_mode { UI_LITE, UI_FULL };

/* The eight type styles of the design; do not add sizes. */
enum ui_type {
	TY_CLOCK_96, TY_DISPLAY_48, TY_TITLE_36, TY_NAME_28,
	TY_ROW_32, TY_BODY_24, TY_LABEL_20, TY_EYEBROW_20,
	TY_COUNT
};

enum ui_fill { FILL_SURFACE, FILL_SURFACE_RAISED };

int theme_init(void);			/* after lv_init(); 0 = ok */
void theme_set_mode(enum ui_mode m);	/* screens must be rebuilt afterwards */
enum ui_mode theme_mode(void);

const lv_font_t *ui_font(enum ui_type t);
int ui_line(enum ui_type t);		/* line pitch in px */

/* A bare object: no theme styles, not scrollable, transparent to touch. */
lv_obj_t *ui_box(lv_obj_t *parent, int w, int h);
/* A label in one of the type styles. */
lv_obj_t *ui_text(lv_obj_t *parent, enum ui_type t, uint32_t color, const char *s);
/* One line of words around a name, no wider than max_w in type t: when it
 * does not fit, the name is cut with an ellipsis, never the words. */
void ui_fit_name(char *out, size_t size, const char *before, const char *name, const char *after,
		 enum ui_type t, int max_w);
/* An icon (8-bit alpha map) drawn in the given colour. */
lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *icon, uint32_t color);
void ui_icon_color(lv_obj_t *icon, uint32_t color);

void ui_bg(lv_obj_t *o, uint32_t color, lv_style_selector_t sel);	/* opaque fill */
/* Tile and card fills: opaque in Lite, glass in Full. */
void ui_fill(lv_obj_t *o, enum ui_fill f, lv_style_selector_t sel);
/* The 1px top highlight that makes a surface read as glass (both modes). */
void ui_edge_light(lv_obj_t *o);
/* Knob focus drawn on the object itself: the amber ring `pad` px outside it. */
void ui_ring_on(lv_obj_t *o, int pad, int on);

/* Move an object: a cut in Lite, `ms` of ease-out travel in Full. */
void ui_move(lv_obj_t *o, int x, int y, int ms);
/* Answers to an input (design/feedback.md); each ends by itself and dies
 * with its object.
 * Acknowledge: the ring's fill is `lit` for a moment, then `rest`. */
void ui_ring_flash(lv_obj_t *ring, uint32_t rest, uint32_t lit);
void ui_ring_fill(lv_obj_t *ring, uint32_t color);	/* set the fill, ending a flash */
/* The same for a ring with no fill of its own (around a tile): a light veil. */
void ui_ring_veil(lv_obj_t *ring);
int ui_ack_ms(void);	/* how long the acknowledge lasts before the change follows */
/* Bump: nothing further that way. Lite thickens the ring for 100 ms; Full
 * nudges it 6px towards (dx, dy) and back. */
void ui_ring_bump(lv_obj_t *ring, int dx, int dy);
/* The same for a ring drawn as an object's outline (a pill, a button). */
void ui_outline_bump(lv_obj_t *o, int dx);
/* Deny: not available. Lite outlines the row for 200 ms; Full shakes its label. */
void ui_deny(lv_obj_t *row, lv_obj_t *label);

/* Remove an object: at once in Lite, after fading over `ms` in Full. */
void ui_dismiss(lv_obj_t *o, int ms);

/* A caret: steady in Lite, blinking once a second in Full. */
void ui_blink(lv_obj_t *o);

/* Turn an arc for ever, `ms` per turn, starting at `deg`, smoothly in both
 * modes. Ends when the arc is deleted. */
void ui_turn(lv_obj_t *arc, int deg, int ms);
#endif
