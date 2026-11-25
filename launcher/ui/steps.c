/* SPDX-License-Identifier: MIT
 * The instruction page: a few numbered steps and two buttons, for an action
 * that needs explaining before it starts and is done while parked
 * (design/screens.md, Instruction page). The only launcher screen with
 * running text; touch-led, but the knob works so nobody is stranded.
 */
#include "screens.h"
#include <stdio.h>

#define TEXT_X 56		/* the column's left edge */
#define TEXT_W 860		/* numbers and text together */
#define NUMBER_W T_SPACE_5
#define BUTTON_H 64		/* sized for a finger: larger than the 48px pills elsewhere */
#define BUTTON_W 200		/* at least */

static lv_obj_t *buttons[2];
static void (*on_tap)(int button);

static void gone(lv_event_t *e)
{
	(void)e;
	buttons[0] = buttons[1] = NULL;
}

static void tapped(lv_event_t *e)
{
	if (on_tap)
		on_tap((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *button(lv_obj_t *parent, int i, const char *label, int solid)
{
	lv_obj_t *b = ui_box(parent, LV_SIZE_CONTENT, BUTTON_H);

	lv_obj_set_style_min_width(b, BUTTON_W, 0);
	lv_obj_set_style_pad_hor(b, T_SPACE_5, 0);
	lv_obj_set_style_radius(b, T_RADIUS_PILL, 0);
	/* Neither is blue: blue is for the phone. "Start" is filled ink. */
	if (solid) {
		ui_bg(b, T_INK, 0);
	} else {
		ui_bg(b, T_SURFACE_RAISED, 0);
		ui_edge_light(b);
	}
	ui_bg(b, T_OUTLINE, LV_STATE_PRESSED);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(b, tapped, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	ui_text(b, TY_LABEL_20, solid ? T_GROUND : T_INK, label);
	return b;
}

void ui_steps_show(const struct ui_steps *s, void (*tap)(int button))
{
	lv_obj_t *content = ui_frame_content();

	ui_header(content, s->title);
	on_tap = tap;

	/* Up to four steps, each one sentence, in a column no wider than 860px. */
	lv_obj_t *col = ui_box(content, TEXT_W, LV_SIZE_CONTENT);

	lv_obj_add_event_cb(col, gone, LV_EVENT_DELETE, NULL);
	lv_obj_set_pos(col, TEXT_X, T_BAND + T_SPACE_2);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, T_SPACE_3, 0);
	for (int i = 0; i < s->n && i < 4; i++) {
		char number[8];
		lv_obj_t *line = ui_box(col, TEXT_W, LV_SIZE_CONTENT);

		snprintf(number, sizeof(number), "%d.", i + 1);
		ui_text(line, TY_BODY_24, T_INK, number);
		lv_obj_t *text = ui_text(line, TY_BODY_24, T_INK, s->steps[i]);

		lv_obj_set_width(text, TEXT_W - NUMBER_W);
		lv_obj_set_x(text, NUMBER_W);
	}

	/* "Cancel", then the action, at the bottom on the driver's side. They
	 * stand in the content area itself: the ring lies outside a button, and
	 * a container of their own would cut it. */
	const int y = T_SCREEN_H - T_SPACE_5 - BUTTON_H;

	buttons[0] = button(content, 0, s->cancel, 0);
	buttons[1] = button(content, 1, s->start, 1);
	lv_obj_update_layout(buttons[0]);
	lv_obj_update_layout(buttons[1]);
	const int w0 = lv_obj_get_width(buttons[0]), both = w0 + T_SPACE_4 + lv_obj_get_width(buttons[1]);
	const int x = ui_frame_rhd() ? T_SCREEN_W - TEXT_X - both : TEXT_X;

	lv_obj_set_pos(buttons[0], x, y);
	lv_obj_set_pos(buttons[1], x + w0 + T_SPACE_4, y);
	ui_steps_focus(s->focus);
}

void ui_steps_focus(int button)
{
	for (int i = 0; i < 2 && buttons[0]; i++)
		ui_ring_on(buttons[i], T_SPACE_1, i == button);
}

void ui_steps_bump(int button, int right)
{
	if (buttons[0])
		ui_outline_bump(buttons[button], right ? 1 : -1);
}
