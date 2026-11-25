/* SPDX-License-Identifier: MIT
 * A list: the car-side screen, modelled on NBT's menus (design/screens.md,
 * Settings; design/components/ListRow, SettingsScreen). A title in the
 * header band, then up to seven 60px rows across the screen, as the car's
 * own lists; one amber ring; no help text.
 */
#include "screens.h"
#include "icons.h"
#include <stdio.h>

#define ROW_W (T_SCREEN_W - 2 * T_SPACE_5)
/* An amount row's columns, fixed from the right edge so every bar on a
 * screen starts and ends at the same x: plus, bar, minus, then the number. */
#define AMT_BUTTON T_BAND
#define AMT_BAR 200
#define AMT_NUMBER 120

static lv_obj_t *list, *ring, *rows[UI_LIST_MAX], *labels[UI_LIST_MAX];
static int shown_focus, shown_n;
static void (*on_tap)(int row, int part);

/* A row that is closing (ui_list_collapse). */
#define LEAVE_BEAT 200		/* it stands that long first, dimmed */
static int leaving = -1;
static void (*left)(void);
static lv_timer_t *leave_timer;

static void gone(lv_event_t *e)
{
	(void)e;
	list = ring = NULL;
	leaving = -1;
	left = NULL;
	if (leave_timer) {
		lv_timer_delete(leave_timer);
		leave_timer = NULL;
	}
}

static void tapped(lv_event_t *e)
{
	intptr_t what = (intptr_t)lv_event_get_user_data(e);	/* row * 4 + part + 1 */

	if (on_tap)
		on_tap((int)(what / 4), (int)(what % 4) - 1);
}

static lv_obj_t *amount_button(lv_obj_t *row, int i, int part, const char *sign, int x)
{
	lv_obj_t *b = ui_box(row, AMT_BUTTON, T_BAND);

	lv_obj_set_pos(b, x, 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(b, tapped, LV_EVENT_CLICKED, (void *)(intptr_t)(i * 4 + part + 1));
	/* The signs stand on the type's maths axis, 2px under the middle of the
	 * line; the bar is in the middle of the band. Raised to meet it (owner,
	 * 2026-11-22: the plus sat below the bar). */
	/* A sign is 15px wide in a 60px button, so the odd pixel falls on the
	 * same side of both and the gaps to the bar came out 22 and 23px: the
	 * minus stands one pixel out, and both are 23 (owner, the same day). */
	lv_obj_align(ui_text(b, TY_ROW_32, T_INK_MUTED, sign), LV_ALIGN_CENTER, part < 0 ? -1 : 0, -2);
	return b;
}

/* (Re)draw what is inside row i. */
static void fill(const struct ui_list *l, int i)
{
	const struct ui_row *r = &l->rows[i];
	int focused = i == l->focus;
	uint32_t ink = r->disabled ? T_INK_FAINT : T_INK;
	uint32_t muted = r->disabled ? T_INK_FAINT : focused ? T_INK : T_INK_MUTED;	/* the value brightens under the ring */
	int right = ROW_W - T_SPACE_4;	/* where the next thing from the right edge ends */

	lv_obj_clean(rows[i]);
	/* The hairline under a row; the ring replaces it on the focused one. */
	lv_obj_set_style_border_opa(rows[i], focused ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
	/* A finger on the row under the ring changes nothing until it lifts:
	 * the pressed fill would cover the ring, which lies under the rows, so
	 * the row seemed to lose the ring and get it back (owner, 2026-11-22). */
	lv_obj_set_style_bg_opa(rows[i], focused ? LV_OPA_TRANSP : LV_OPA_COVER, LV_STATE_PRESSED);

	if (r->amount) {
		char number[24];
		int x = ROW_W - AMT_BUTTON;	/* the row has no padding on this side */
		lv_obj_t *bar, *fillbar;
		int span = r->max - r->min, w;

		amount_button(rows[i], i, 1, "+", x);
		x -= AMT_BAR;
		bar = ui_box(rows[i], AMT_BAR, 8);
		lv_obj_set_pos(bar, x, (T_BAND - 8) / 2);
		lv_obj_set_style_radius(bar, 4, 0);
		ui_bg(bar, focused ? T_OUTLINE : T_SURFACE_RAISED, 0);
		fillbar = ui_box(bar, 0, 8);
		lv_obj_set_style_radius(fillbar, 4, 0);
		ui_bg(fillbar, T_INK, 0);
		if (r->min < 0) {	/* either side of zero: from the middle, with a centre mark */
			int zero = AMT_BAR * (0 - r->min) / span, at = AMT_BAR * (r->now - r->min) / span;
			lv_obj_t *mark = ui_box(rows[i], 2, 20);

			lv_obj_set_pos(fillbar, at < zero ? at : zero, 0);
			lv_obj_set_width(fillbar, at < zero ? zero - at : at - zero);
			lv_obj_set_pos(mark, x + zero - 1, (T_BAND - 20) / 2);
			ui_bg(mark, T_INK_MUTED, 0);
		} else {
			w = AMT_BAR * (r->now - r->min) / span;
			lv_obj_set_width(fillbar, w);
		}
		x -= AMT_BUTTON;
		amount_button(rows[i], i, -1, "\xE2\x88\x92", x);	/* a minus sign, not a hyphen */
		if (r->min < 0 && r->now != 0)
			snprintf(number, sizeof(number), "%s%d%s", r->now > 0 ? "+" : "\xE2\x88\x92",
				 r->now > 0 ? r->now : -r->now, r->unit);
		else
			snprintf(number, sizeof(number), "%d%s", r->now, r->unit);
		lv_obj_t *n = ui_text(rows[i], TY_ROW_32, muted, number);

		lv_obj_set_width(n, AMT_NUMBER);
		lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_align(n, LV_ALIGN_LEFT_MID, x - T_SPACE_2 - AMT_NUMBER, 0);
		right = x - T_SPACE_2 - AMT_NUMBER;
	} else {
		if (r->chevron || r->checked) {
			lv_obj_t *c = ui_icon(rows[i], r->chevron ? &icon_chevron_right_28 : &icon_check_28,
					      r->chevron ? (r->disabled ? T_INK_FAINT : T_INK_MUTED) : ink);

			lv_obj_align(c, LV_ALIGN_LEFT_MID, right - 28, 0);
			right -= 28 + T_SPACE_3;
		}
		if (r->value && r->value[0]) {
			lv_obj_t *v = ui_text(rows[i], TY_ROW_32, muted, r->value);

			lv_obj_update_layout(v);
			lv_obj_align(v, LV_ALIGN_LEFT_MID, right - lv_obj_get_width(v), 0);
			right -= lv_obj_get_width(v) + T_SPACE_3;
		}
	}
	/* One line: the label gives way to the value, with an ellipsis. */
	int left = T_SPACE_4;

	if (r->back) {	/* the way up, as a row: a chevron before the word */
		lv_obj_align(ui_icon(rows[i], &icon_chevron_left_32, ink), LV_ALIGN_LEFT_MID, T_SPACE_4 - T_SPACE_2, 0);
		left = T_SPACE_4 - T_SPACE_2 + 32 + T_SPACE_1;
	}
	labels[i] = ui_text(rows[i], TY_ROW_32, ink, r->label);
	lv_obj_set_width(labels[i], right - left);
	lv_obj_set_height(labels[i], lv_font_get_line_height(ui_font(TY_ROW_32)));
	lv_label_set_long_mode(labels[i], LV_LABEL_LONG_DOT);
	lv_obj_align(labels[i], LV_ALIGN_LEFT_MID, left, 0);
}

static void ring_fill(const struct ui_list *l)
{
	ui_ring_fill(ring, l->adjusting ? T_AMBER_PRESS : T_AMBER_WASH);
}

void ui_list_show(const struct ui_list *l, void (*tap)(int row, int part))
{
	lv_obj_t *content = ui_frame_content();

	ui_header(content, l->title);
	on_tap = tap;

	lv_obj_t *view = ui_box(content, ROW_W, UI_LIST_ROWS * T_BAND);	/* clips the rows beyond seven */

	lv_obj_set_pos(view, T_SPACE_5, T_BAND);
	list = ui_box(view, ROW_W, l->n * T_BAND);
	lv_obj_add_event_cb(list, gone, LV_EVENT_DELETE, NULL);
	lv_obj_set_pos(list, 0, -l->top * T_BAND);

	/* The knob's ring: under the rows' text, inside the row. */
	ring = ui_box(list, ROW_W, T_BAND);
	lv_obj_set_style_radius(ring, T_RADIUS_SM, 0);
	lv_obj_set_style_border_width(ring, T_RING, 0);
	lv_obj_set_style_border_color(ring, lv_color_hex(T_AMBER), 0);
	lv_obj_set_pos(ring, 0, l->focus * T_BAND);
	ring_fill(l);

	for (int i = 0; i < l->n && i < UI_LIST_MAX; i++) {
		rows[i] = ui_box(list, ROW_W, T_BAND);
		lv_obj_set_pos(rows[i], 0, i * T_BAND);
		lv_obj_set_style_radius(rows[i], T_RADIUS_SM, 0);
		lv_obj_set_style_border_side(rows[i], LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(rows[i], 1, 0);
		lv_obj_set_style_border_color(rows[i], lv_color_hex(T_HAIRLINE), 0);
		/* Finger down: the fill steps up; never amber. */
		ui_bg(rows[i], T_SURFACE_RAISED, LV_STATE_PRESSED);
		lv_obj_add_flag(rows[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(rows[i], tapped, LV_EVENT_CLICKED, (void *)(intptr_t)(i * 4 + 1));
		fill(l, i);
	}
	shown_focus = l->focus;
	shown_n = l->n < UI_LIST_MAX ? l->n : UI_LIST_MAX;
}

/* ---- a row leaves the list ---- */

static void leave_done(lv_timer_t *t)
{
	void (*done)(void) = left;

	(void)t;
	leave_timer = NULL;	/* it ran its one time and deletes itself */
	leaving = -1;
	left = NULL;
	if (done)
		done();	/* draws the list as it is now: this one is deleted with it */
}

static void leave_after(int ms)
{
	leave_timer = lv_timer_create(leave_done, ms, NULL);
	lv_timer_set_repeat_count(leave_timer, 1);
}

static void closing(void *var, int32_t v)
{
	(void)var;
	if (!list || leaving < 0)
		return;
	lv_obj_set_height(rows[leaving], T_BAND - v);
	for (int i = leaving + 1; i < shown_n; i++)
		lv_obj_set_y(rows[i], i * T_BAND - v);
}

static void closed(lv_anim_t *a)
{
	(void)a;
	leave_after(1);	/* not from inside the animation: the list is deleted next */
}

/* Row `row` of the list that is shown closes and the rows below move up one
 * band, into the ring, which stays (design: a forgotten phone leaves before
 * the driver's eyes). Full: it stands a beat, then closes in dur-scroll.
 * Lite: it stands a little longer and is gone in one frame. Then `done`. */
void ui_list_collapse(const struct ui_list *l, int row, void (*done)(void))
{
	lv_anim_t a;

	if (!list || row < 0 || row >= shown_n || row >= l->n || leaving >= 0) {
		if (done)
			done();
		return;
	}
	leaving = row;
	left = done;
	if (theme_mode() != UI_FULL) {
		leave_after(LEAVE_BEAT + T_DUR_SCROLL);
		return;
	}
	lv_anim_init(&a);
	lv_anim_set_var(&a, list);
	lv_anim_set_exec_cb(&a, closing);
	lv_anim_set_values(&a, 0, T_BAND);
	lv_anim_set_delay(&a, LEAVE_BEAT);
	lv_anim_set_duration(&a, T_DUR_SCROLL);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, closed);
	lv_anim_start(&a);
}

/* The ring on the model's row: by travelling there (the knob), or at once
 * (a finger has said where). */
static void place(const struct ui_list *l, int travel)
{
	if (!list)
		return;
	/* Rows slide by whole bands, so none is ever cut. */
	ui_move(ring, 0, l->focus * T_BAND, travel ? T_DUR_STEP : 0);
	ui_move(list, 0, -l->top * T_BAND, travel ? T_DUR_SCROLL : 0);
	ring_fill(l);
	if (shown_focus != l->focus && shown_focus < l->n)
		fill(l, shown_focus);
	fill(l, l->focus);
	shown_focus = l->focus;
}

void ui_list_place(const struct ui_list *l)
{
	place(l, 1);
}

void ui_list_put(const struct ui_list *l)
{
	place(l, 0);
}

void ui_list_row(const struct ui_list *l, int row)
{
	if (list && row < l->n)
		fill(l, row);
}

void ui_list_ack(const struct ui_list *l)
{
	if (list)
		ui_ring_flash(ring, l->adjusting ? T_AMBER_PRESS : T_AMBER_WASH, T_AMBER_PRESS);
}

void ui_list_bump(int down)
{
	if (list)
		ui_ring_bump(ring, 0, down ? 1 : -1);
}

void ui_list_deny(int row)
{
	if (list && row < UI_LIST_MAX)
		ui_deny(rows[row], labels[row]);
}
