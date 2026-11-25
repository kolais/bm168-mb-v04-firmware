/* SPDX-License-Identifier: MIT
 * The waiting screen: a spinner and one line (design/screens.md, Connecting;
 * design/components/ConnectingScreen). It has no time limit of its own: the
 * caller shows it for as long as the unit is trying and changes the line as
 * its attempts go. Nothing on it takes the knob's ring.
 */
#include "screens.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>

#define SPIN_BOX 136
#define SPIN_STROKE 5
#define SPIN_OUTER 130		/* the arc's centre line has r = 62, as drawn in the design */
#define TEXT_MAX (T_SCREEN_W - 2 * T_SPACE_5)

#define CANCEL_H 64		/* a button for a finger, as on the instruction page */
#define CANCEL_W 200

static lv_obj_t *screen, *arc, *line, *cancel;
static void (*on_cancel)(void);

static void cancel_tapped(lv_event_t *e)
{
	(void)e;
	if (on_cancel)
		on_cancel();
}

static void gone(lv_event_t *e)
{
	(void)e;
	screen = arc = line = cancel = NULL;
}

/* The words belong to the design; the caller supplies only the name. */
static const struct {
	const char *before, *after;
	int phone, turning;
	int cancel;	/* there is something to cancel: the unit is trying */
} kinds[] = {
	[WAIT_CONNECTING] = { "Connecting to ", "", 1, 1, 1 },
	[WAIT_RETRYING] = { "Still trying ", "", 1, 1, 1 },
	[WAIT_FAILED] = { "Couldn\xE2\x80\x99t connect to ", "", 1, 0, 0 },
	[WAIT_PAIRING] = { "Choose \xE2\x80\x9C", "\xE2\x80\x9D in your iPhone\xE2\x80\x99s Bluetooth settings", 1, 1, 1 },
	[WAIT_UPDATING] = { "Updating. Keep the ignition on", "", 0, 1, 0 },
};

/* One line. When it does not fit, the name is cut, never the verb. */
static void set_line(enum ui_wait kind, const char *name)
{
	char text[256];

	ui_fit_name(text, sizeof(text), kinds[kind].before, kind == WAIT_UPDATING ? "" : name, kinds[kind].after,
		    TY_TITLE_36, TEXT_MAX);
	lv_label_set_text(line, text);
}

static void set_kind(enum ui_wait kind, const char *name)
{
	set_line(kind, name);
	/* A turning arc says "working": when the unit has given up it goes,
	 * and the track stays. */
	if (kinds[kind].turning)
		lv_obj_remove_flag(arc, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
	if (kinds[kind].cancel)
		lv_obj_remove_flag(cancel, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(cancel, LV_OBJ_FLAG_HIDDEN);
}

void ui_wait_show(enum ui_wait kind, const char *name, void (*tap_cancel)(void))
{
	lv_obj_t *content = ui_frame_content();
	/* Spinner, line and "Cancel" as one block, centred on the screen. The
	 * block keeps its place when the button is not there. */
	const int text_y = SPIN_BOX + T_SPACE_5, cancel_y = text_y + ui_line(TY_TITLE_36) + T_SPACE_6;
	const int top = (T_SCREEN_H - (cancel_y + CANCEL_H)) / 2;

	on_cancel = tap_cancel;

	screen = ui_box(content, T_SCREEN_W, T_SCREEN_H);
	lv_obj_add_event_cb(screen, gone, LV_EVENT_DELETE, NULL);

	lv_obj_t *spin = ui_box(screen, SPIN_BOX, SPIN_BOX);

	lv_obj_align(spin, LV_ALIGN_TOP_MID, 0, top);
	/* The track: a faint full circle. */
	lv_obj_t *track = lv_arc_create(spin);

	lv_obj_remove_style_all(track);
	lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(track, SPIN_OUTER, SPIN_OUTER);
	lv_obj_center(track);
	lv_arc_set_bg_angles(track, 0, 360);
	lv_obj_set_style_arc_width(track, SPIN_STROKE, LV_PART_MAIN);
	lv_obj_set_style_arc_color(track, lv_color_hex(T_OUTLINE), LV_PART_MAIN);
	lv_obj_set_style_arc_opa(track, LV_OPA_50, LV_PART_MAIN);
	lv_obj_set_style_arc_opa(track, LV_OPA_TRANSP, LV_PART_INDICATOR);	/* the widget's own value arc: unused */
	/* The arc: a quarter in blue (blue is the phone), starting at the top. */
	arc = lv_arc_create(spin);
	lv_obj_remove_style_all(arc);
	lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(arc, SPIN_OUTER, SPIN_OUTER);
	lv_obj_center(arc);
	lv_arc_set_bg_angles(arc, 0, 90);
	lv_obj_set_style_arc_width(arc, SPIN_STROKE, LV_PART_MAIN);
	lv_obj_set_style_arc_color(arc, lv_color_hex(T_BLUE), LV_PART_MAIN);
	lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
	lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
	ui_turn(arc, 270, 1000);	/* once per second */

	if (kinds[kind].phone)
		lv_obj_center(ui_icon(spin, &icon_phone_56, T_INK));

	line = ui_text(screen, TY_TITLE_36, T_INK, "");
	lv_obj_align(line, LV_ALIGN_TOP_MID, 0, top + text_y);

	/* "Cancel": the way back by touch (the knob has its Back key). The one
	 * button on the screen, so it is drawn as the system's solid button, ink
	 * with dark words (owner, 2026-11-22). It never takes the ring. */
	cancel = ui_box(screen, LV_SIZE_CONTENT, CANCEL_H);
	lv_obj_set_style_min_width(cancel, CANCEL_W, 0);
	lv_obj_set_style_pad_hor(cancel, T_SPACE_5, 0);
	lv_obj_set_style_radius(cancel, T_RADIUS_PILL, 0);
	ui_bg(cancel, T_INK, 0);
	ui_bg(cancel, T_OUTLINE, LV_STATE_PRESSED);
	lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(cancel, cancel_tapped, LV_EVENT_CLICKED, NULL);
	lv_obj_set_flex_flow(cancel, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(cancel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	ui_text(cancel, TY_LABEL_20, T_GROUND, "Cancel");
	lv_obj_align(cancel, LV_ALIGN_TOP_MID, 0, top + cancel_y);
	set_kind(kind, name);
}

void ui_wait_set(enum ui_wait kind, const char *name)
{
	if (screen)	/* the spinner keeps its phase */
		set_kind(kind, name);
}
