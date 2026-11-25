/* SPDX-License-Identifier: MIT
 * The switch banner (design/screens.md, Over CarPlay): while CarPlay has the
 * screen the launcher draws this one thing over it. The rest of the layer is
 * transparent, and touches outside the banner are CarPlay's.
 */
#include "screens.h"
#include "icons.h"
#include <stdio.h>

#define BANNER_W 1000
#define BANNER_H 112
#define BANNER_Y 16
#define BANNER_MS 8000
#define MARGIN T_SPACE_4	/* a finger this close to the banner is not CarPlay's */
#define BUTTON_H 64		/* the size of the instruction page's buttons */
#define BUTTON_MAX 520

static lv_obj_t *zone;
static lv_timer_t *timeout;
static void (*on_tap)(void);

static void gone(lv_event_t *e)
{
	(void)e;
	zone = NULL;
	if (timeout) {
		lv_timer_delete(timeout);
		timeout = NULL;
	}
}

static void time_up(lv_timer_t *t)
{
	(void)t;
	timeout = NULL;		/* a timer that ran its one time deletes itself */
	if (zone)
		ui_dismiss(zone, T_DUR_SCREEN);
}

static void switch_tapped(lv_event_t *e)
{
	void (*tap)(void) = on_tap;

	(void)e;
	ui_banner_hide();
	if (tap)
		tap();
}

static void beside_tapped(lv_event_t *e)
{
	(void)e;
	ui_banner_hide();	/* a near miss sends it away; it never reaches CarPlay */
}

void ui_banner_show(const char *current, const char *other, void (*tap)(void))
{
	char label[160];

	ui_banner_hide();
	on_tap = tap;
	/* The banner with its margin: touches here are ours. */
	zone = ui_box(lv_screen_active(), BANNER_W + 2 * MARGIN, BANNER_H + 2 * MARGIN);
	lv_obj_add_event_cb(zone, gone, LV_EVENT_DELETE, NULL);
	lv_obj_align(zone, LV_ALIGN_TOP_MID, 0, BANNER_Y - MARGIN);
	lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(zone, beside_tapped, LV_EVENT_CLICKED, NULL);

	/* The card: opaque in both modes (CarPlay moves behind it, and nothing
	 * may move behind glass), and the whole of it is the switch: a driver
	 * taps the banner, not a word on it (owner at the bench, 2026-11-22). */
	lv_obj_t *card = ui_box(zone, BANNER_W, BANNER_H);

	lv_obj_center(card);
	/* A pill: the button's round end is 24px from the card's edge, and
	 * with the card's ends as half-circles of 32 + 24 it stays 24px all
	 * the way round (owner, 2026-11-22). */
	lv_obj_set_style_radius(card, BANNER_H / 2, 0);
	ui_bg(card, T_SURFACE, 0);
	ui_bg(card, T_SURFACE_RAISED, LV_STATE_PRESSED);
	ui_edge_light(card);
	lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(card, switch_tapped, LV_EVENT_CLICKED, NULL);

	/* What a tap does, drawn as the system's solid button: ink, as "Start"
	 * and "Done". It leads away from the phone that is in CarPlay now, so
	 * nothing here is blue. */
	lv_obj_t *button = ui_box(card, LV_SIZE_CONTENT, BUTTON_H);

	lv_obj_set_style_radius(button, T_RADIUS_PILL, 0);
	lv_obj_set_style_pad_hor(button, T_SPACE_5, 0);
	ui_bg(button, T_INK, 0);
	ui_fit_name(label, sizeof(label), "Switch to ", other, "", TY_NAME_28, BUTTON_MAX - 2 * T_SPACE_5);
	lv_obj_center(ui_text(button, TY_NAME_28, T_GROUND, label));
	lv_obj_align(button, LV_ALIGN_RIGHT_MID, -T_SPACE_4, 0);
	lv_obj_update_layout(button);

	/* The phone that is in CarPlay now: its mark and its name. */
	lv_obj_t *icon = ui_icon(card, &icon_phone_56, T_INK);

	lv_obj_align(icon, LV_ALIGN_LEFT_MID, T_SPACE_5, 0);	/* clear of the round end */
	lv_obj_t *name = ui_text(card, TY_NAME_28, T_INK, current);
	const int x = T_SPACE_5 + 56 + T_SPACE_3;

	lv_obj_set_width(name, BANNER_W - x - 2 * T_SPACE_4 - lv_obj_get_width(button));
	lv_obj_set_height(name, lv_font_get_line_height(ui_font(TY_NAME_28)));
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_align(name, LV_ALIGN_LEFT_MID, x, 0);

	timeout = lv_timer_create(time_up, BANNER_MS, NULL);
	lv_timer_set_repeat_count(timeout, 1);
}

void ui_banner_hide(void)
{
	if (zone)
		lv_obj_delete(zone);
}

int ui_banner_shown(void)
{
	return zone != NULL;
}

int ui_banner_owns(int x, int y)
{
	const int left = (T_SCREEN_W - BANNER_W) / 2 - MARGIN, top = BANNER_Y - MARGIN;

	return zone && x >= left && x < left + BANNER_W + 2 * MARGIN && y >= top && y < top + BANNER_H + 2 * MARGIN;
}
