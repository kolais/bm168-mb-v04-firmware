/* SPDX-License-Identifier: MIT
 * The frame every launcher screen shares: the status at the right of the top
 * band, the Settings cog on Home, and the content area under and behind
 * them (design/screens.md, design/components/StatusBar).
 */
#include "screens.h"
#include "icons.h"

#define CLOCK_W 84

static lv_obj_t *root, *cover, *content;
static enum ui_layer layer = LAYER_CLEAR;
static lv_obj_t *bar_status, *bar_clock, *bar_bt, *bar_cog, *bar_cog_icon;
static int rhd;
static void (*tap_cog)(void);

static void on_cog(lv_event_t *e)
{
	(void)e;
	if (tap_cog)
		tap_cog();
}

void ui_frame_init(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	/* The layer itself is transparent: what shows is what we draw. */
	lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
	root = ui_box(scr, T_SCREEN_W, T_SCREEN_H);
	ui_bg(root, T_GROUND, 0);
	lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);	/* clear until a screen is asked for */
	cover = ui_box(scr, T_SCREEN_W, T_SCREEN_H);
	ui_bg(cover, T_GROUND, 0);
	lv_obj_add_flag(cover, LV_OBJ_FLAG_HIDDEN);

	content = ui_box(root, T_SCREEN_W, T_SCREEN_H);

	/* The frame's own things, after the content so they lie on it. They
	 * carry no blue and no status colours. */
	bar_status = ui_box(root, UI_BAR_STATUS, T_BAND);
	lv_obj_set_pos(bar_status, T_SCREEN_W - T_SPACE_5 - UI_BAR_STATUS, 0);
	lv_obj_set_flex_flow(bar_status, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar_status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(bar_status, T_SPACE_2, 0);
	/* Bluetooth and the clock. Bluetooth has no mark while it is off, as
	 * on a phone; a Wi-Fi mark stood here until 2026-11-23 (a running
	 * CarPlay session is now Home's chip: the CarPlay sign and the phone). */
	bar_bt = ui_icon(bar_status, &icon_bluetooth_28, T_INK);
	bar_clock = ui_text(bar_status, TY_NAME_28, T_INK, "");
	lv_obj_set_width(bar_clock, CLOCK_W);	/* fixed: the marks do not move with the minute */
	lv_obj_set_style_text_align(bar_clock, LV_TEXT_ALIGN_RIGHT, 0);

	bar_cog = ui_box(root, UI_COG, UI_COG);
	lv_obj_set_style_radius(bar_cog, UI_COG / 2, 0);
	lv_obj_add_flag(bar_cog, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(bar_cog, 12);	/* a finger gets more than the circle, up to the tiles */
	bar_cog_icon = ui_icon(bar_cog, &icon_settings_32, T_INK);
	lv_obj_center(bar_cog_icon);
	ui_bg(bar_cog, T_SURFACE_RAISED, 0);
	ui_edge_light(bar_cog);
	lv_obj_add_event_cb(bar_cog, on_cog, LV_EVENT_CLICKED, NULL);
}

void ui_frame_rail(const struct ui_rail *r)
{
	rhd = r->rhd;
	lv_label_set_text(bar_clock, r->clock);
	lv_image_set_src(bar_bt, r->bt == UI_BT_LINKED ? &icon_bluetooth_connected_28 : &icon_bluetooth_28);
	if (r->bt == UI_BT_OFF)
		lv_obj_add_flag(bar_bt, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_remove_flag(bar_bt, LV_OBJ_FLAG_HIDDEN);
	/* The cog: a quiet circle under the tiles on the driver's side, its
	 * outer edge on the tiles' and the status's line; on Home only. The
	 * last stop of the knob on its side. */
	lv_obj_set_pos(bar_cog, rhd ? T_SCREEN_W - UI_EDGE - UI_COG : UI_EDGE, UI_COG_Y);
	if (r->cog_shown)
		lv_obj_remove_flag(bar_cog, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(bar_cog, LV_OBJ_FLAG_HIDDEN);
	ui_ring_on(bar_cog, T_SPACE_1, r->cog_shown && r->cog_focus);
}

lv_obj_t *ui_frame_content(void)
{
	lv_obj_clean(content);
	return content;
}

int ui_frame_rhd(void)
{
	return rhd;
}

int ui_frame_head_x(void)
{
	return T_SPACE_5;
}

int ui_frame_head_end(void)
{
	return T_SCREEN_W - T_SPACE_5 - UI_BAR_STATUS - T_SPACE_4;
}

void ui_frame_on_tap(void (*cog)(void))
{
	tap_cog = cog;
}

/* ---- a short message ---- */

/* A line that says what the screen cannot show by itself (a phone that
 * could not be forgotten): a pill in the top band, before the status, for
 * three seconds. It lies over the content, so a redraw does not take it. */
#define MESSAGE_MS 3000

static lv_obj_t *message;
static lv_timer_t *message_timer;

static void message_gone(lv_event_t *e)
{
	(void)e;
	message = NULL;
	if (message_timer) {
		lv_timer_delete(message_timer);
		message_timer = NULL;
	}
}

static void message_over(lv_timer_t *t)
{
	(void)t;
	message_timer = NULL;	/* it ran its one time and deletes itself */
	if (message)
		ui_dismiss(message, T_DUR_SCREEN);
}

void ui_frame_message(const char *text)
{
	if (message)
		lv_obj_delete(message);
	if (layer != LAYER_SCREEN)	/* no launcher screen is up to say it on */
		return;
	message = ui_box(root, LV_SIZE_CONTENT, T_PILL_H);
	lv_obj_add_event_cb(message, message_gone, LV_EVENT_DELETE, NULL);
	lv_obj_set_style_radius(message, T_RADIUS_PILL, 0);
	lv_obj_set_style_pad_hor(message, T_SPACE_4, 0);
	ui_bg(message, T_SURFACE_RAISED, 0);
	ui_edge_light(message);
	lv_obj_center(ui_text(message, TY_LABEL_20, T_INK, text));
	lv_obj_align(message, LV_ALIGN_TOP_RIGHT, -(T_SCREEN_W - ui_frame_head_end()), (T_BAND - T_PILL_H) / 2);
	message_timer = lv_timer_create(message_over, MESSAGE_MS, NULL);
	lv_timer_set_repeat_count(message_timer, 1);
}

int ui_frame_message_shown(void)
{
	return message != NULL;
}

lv_obj_t *ui_header(lv_obj_t *parent, const char *title)
{
	lv_obj_t *h = ui_box(parent, T_SCREEN_W, T_BAND);
	lv_obj_t *t = ui_text(h, TY_TITLE_36, T_INK, title);

	lv_obj_align(t, LV_ALIGN_LEFT_MID, ui_frame_head_x(), 0);
	return h;
}

void ui_header_chip(lv_obj_t *header, enum ui_chip kind, const char *text)
{
	/* A status chip pairs a shape with a word, so it reads without colour. */
	lv_obj_t *c = ui_box(header, LV_SIZE_CONTENT, 40);

	ui_bg(c, T_SURFACE_RAISED, 0);
	lv_obj_set_style_radius(c, T_RADIUS_PILL, 0);
	lv_obj_set_style_pad_hor(c, T_SPACE_3, 0);
	lv_obj_set_style_pad_column(c, T_SPACE_2, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_align(c, LV_ALIGN_RIGHT_MID, ui_frame_head_end() - T_SCREEN_W, 0);

	if (kind == CHIP_UP) {	/* a CarPlay session runs: its sign, and whose it is */
		ui_icon(c, &icon_carplay_24, T_INK);	/* as the Bluetooth mark: ink, no status colour */
	} else {
		lv_obj_t *dot = ui_box(c, 12, 12);

		lv_obj_set_style_radius(dot, 6, 0);
		lv_obj_set_style_border_width(dot, 2, 0);
		lv_obj_set_style_border_color(dot, lv_color_hex(T_OUTLINE), 0);
	}
	ui_text(c, TY_LABEL_20, T_INK, text);
}

static void cog_thick(void *cog, int32_t v)
{
	(void)v;
	lv_obj_set_style_outline_width(cog, T_RING + 2, 0);
}

static void cog_thin(lv_anim_t *a)
{
	lv_obj_set_style_outline_width(a->var, T_RING, 0);
}

void ui_frame_cog_bump(void)
{
	lv_anim_t a;

	/* Both modes thicken the ring: a nudge would move the button itself. */
	lv_anim_delete(bar_cog, cog_thick);
	lv_anim_init(&a);
	lv_anim_set_var(&a, bar_cog);
	lv_anim_set_exec_cb(&a, cog_thick);
	lv_anim_set_duration(&a, 100);
	lv_anim_set_completed_cb(&a, cog_thin);
	lv_anim_start(&a);
}

void ui_frame_layer(enum ui_layer l)
{
	layer = l;
	if (l == LAYER_SCREEN)
		lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
	if (l == LAYER_GROUND)
		lv_obj_remove_flag(cover, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(cover, LV_OBJ_FLAG_HIDDEN);
}

enum ui_layer ui_frame_layer_now(void)
{
	return layer;
}
