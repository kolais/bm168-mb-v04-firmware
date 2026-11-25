/* SPDX-License-Identifier: MIT
 * Home: the paired phones as a row of tiles (design/screens.md, Home;
 * design/components/Tile).
 */
#include "screens.h"
#include "icons.h"
#include <math.h>

#define TILE_W 264
#define TILE_H 320
#define TILE_PAD T_SPACE_4			/* from a tile's edge to what is in it */
#define TILE_PITCH (TILE_W + T_SPACE_3)
#define PAGE_W (UI_TILES_PER_PAGE * TILE_PITCH)	/* 1120: four tiles and their gaps */
#define ROW_W (PAGE_W - T_SPACE_3)		/* four tiles and the three gaps between them: 1104 */
#define VIEW_H 348
#define VIEW_PAD_X T_SPACE_2			/* room for the ring around the edge tiles */
#define VIEW_Y (T_BAND - T_SPACE_1)		/* the tiles stand from 64 to 384: the cog is under them */
#define VIEW_PAD_Y 8
#define RING_OUT (T_SPACE_1 + T_RING)		/* the ring sits 4px outside a tile, 3px wide */
#define DOT 3					/* "Pair a phone": a dotted outline */
#define DOT_PITCH 9

static lv_obj_t *row, *ring, *dots;
static int tiles, shown_page, rhd;

/* Where tile i stands in the row. Four to a page, in reading order. A page
 * that is not full stands at the driver's side: with the driver on the
 * right its tiles take the last places, next to the cog (owner at the
 * bench, 2026-11-22: "I'd expect tiles to be on the right-hand side"). */
static int tile_x(int i)
{
	int page = i / UI_TILES_PER_PAGE, on_page = tiles - page * UI_TILES_PER_PAGE;

	if (on_page > UI_TILES_PER_PAGE)
		on_page = UI_TILES_PER_PAGE;
	return (i + (rhd ? UI_TILES_PER_PAGE - on_page : 0)) * TILE_PITCH;
}
static void (*on_tap)(int tile);

static void tapped(lv_event_t *e)
{
	if (on_tap)
		on_tap((int)(intptr_t)lv_event_get_user_data(e));
}

/* A dotted, rounded outline: round dots at one pitch all the way round,
 * corners included. (It was dashed: LVGL dashes only straight lines, so the
 * corners had dashes of another length, which the owner saw at the bench.) */
static void draw_dotted(lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_draw_rect_dsc_t d;
	const double pi = 3.14159265358979, inset = DOT / 2.0, r = T_RADIUS_LG - inset;

	lv_obj_get_coords(o, &a);
	const double x1 = a.x1 + inset, y1 = a.y1 + inset, x2 = a.x2 + 1 - inset, y2 = a.y2 + 1 - inset;
	const double side_w = x2 - x1 - 2 * r, side_h = y2 - y1 - 2 * r, arc = pi * r / 2;
	const double round = 2 * (side_w + side_h) + 4 * arc;
	const int n = (int)(round / DOT_PITCH + 0.5);

	lv_draw_rect_dsc_init(&d);
	d.bg_color = lv_color_hex(T_OUTLINE);
	d.radius = LV_RADIUS_CIRCLE;
	for (int i = 0; i < n; i++) {
		double s = round * i / n, x, y, t;	/* clockwise from the end of the top left corner */

		if (s < side_w) {
			x = x1 + r + s, y = y1;
		} else if ((s -= side_w) < arc) {
			t = s / r, x = x2 - r + r * sin(t), y = y1 + r - r * cos(t);
		} else if ((s -= arc) < side_h) {
			x = x2, y = y1 + r + s;
		} else if ((s -= side_h) < arc) {
			t = s / r, x = x2 - r + r * cos(t), y = y2 - r + r * sin(t);
		} else if ((s -= arc) < side_w) {
			x = x2 - r - s, y = y2;
		} else if ((s -= side_w) < arc) {
			t = s / r, x = x1 + r - r * sin(t), y = y2 - r + r * cos(t);
		} else if ((s -= arc) < side_h) {
			x = x1, y = y2 - r - s;
		} else {
			t = (s - side_h) / r, x = x1 + r - r * cos(t), y = y1 + r - r * sin(t);
		}
		lv_area_t dot = { (int)floor(x - inset + 0.5), (int)floor(y - inset + 0.5), 0, 0 };

		dot.x2 = dot.x1 + DOT - 1;
		dot.y2 = dot.y1 + DOT - 1;
		lv_draw_rect(layer, &d, &dot);
	}
}

static void gone(lv_event_t *e)
{
	(void)e;
	row = ring = dots = NULL;
}

static lv_obj_t *tile(int i, const lv_image_dsc_t *icon, const char *name, const char *word,
		      uint32_t name_color, uint32_t word_color, int red_square)
{
	lv_obj_t *t = ui_box(row, TILE_W, TILE_H);

	lv_obj_set_pos(t, RING_OUT + tile_x(i), RING_OUT);
	lv_obj_set_style_radius(t, T_RADIUS_LG, 0);
	lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(t, tapped, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	lv_obj_t *ic = ui_icon(t, icon, name_color);
	lv_obj_align(ic, LV_ALIGN_TOP_LEFT, TILE_PAD, T_SPACE_4);

	/* The state word, with a red square before "Unreachable": colour never
	 * carries a state alone. */
	lv_obj_t *sub = ui_box(t, LV_SIZE_CONTENT, ui_line(TY_LABEL_20));
	lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(sub, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(sub, T_SPACE_2, 0);
	lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, TILE_PAD, -T_SPACE_4);
	if (red_square)
		ui_bg(ui_box(sub, 12, 12), T_RED, 0);
	ui_text(sub, TY_LABEL_20, word_color, word);

	/* The name: two lines at most, then an ellipsis; it grows upwards. */
	lv_obj_t *n = ui_text(t, TY_NAME_28, name_color, name);
	const int width = TILE_W - 2 * TILE_PAD, two_lines = 2 * ui_line(TY_NAME_28);
	lv_point_t need;

	lv_text_get_size(&need, name, ui_font(TY_NAME_28), lv_obj_get_style_text_letter_space(n, 0),
			 lv_obj_get_style_text_line_space(n, 0), width, LV_TEXT_FLAG_NONE);
	lv_obj_set_width(n, width);
	if (need.y > two_lines) {
		lv_obj_set_height(n, two_lines);
		lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
	}
	lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, TILE_PAD, -(T_SPACE_4 + ui_line(TY_LABEL_20) + T_SPACE_2));
	return t;
}

static void place(int focus, int travel)
{
	int page = focus < 0 ? shown_page : focus / UI_TILES_PER_PAGE;
	int pages = (tiles + UI_TILES_PER_PAGE - 1) / UI_TILES_PER_PAGE;

	/* Crossing a page edge slides the row one whole page; the ring is the
	 * row's child and travels with it. */
	ui_move(row, VIEW_PAD_X - RING_OUT - page * PAGE_W, VIEW_PAD_Y - RING_OUT, travel ? T_DUR_SCREEN : 0);
	if (focus >= 0) {
		lv_obj_remove_flag(ring, LV_OBJ_FLAG_HIDDEN);
		ui_move(ring, tile_x(focus), 0, travel ? T_DUR_STEP : 0);
	} else {
		lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);	/* the knob is on the cog */
	}
	shown_page = page;
	/* Page dots: position only, not controls; none for a single page. */
	lv_obj_clean(dots);
	for (int i = 0; pages > 1 && i < pages; i++) {
		lv_obj_t *d = ui_box(dots, 8, 8);

		lv_obj_set_style_radius(d, 4, 0);
		ui_bg(d, i == page ? T_INK : T_OUTLINE, 0);
	}
}

void ui_home_show(const struct ui_home *h, void (*tap)(int tile))
{
	on_tap = tap;
	lv_obj_t *content = ui_frame_content();
	lv_obj_t *head = ui_header(content, "Phones");

	if (h->chip_text && *h->chip_text)	/* nothing to say: no chip (the tiles tell which phone is connected) */
		ui_header_chip(head, h->chip, h->chip_text);

	/* The row of four stands at the driver's side, its outer edge on the
	 * line of the status and the cog; what is left over is on the other side. */
	rhd = ui_frame_rhd();
	const int row_x = rhd ? T_SCREEN_W - UI_EDGE - ROW_W : UI_EDGE;
	lv_obj_t *view = ui_box(content, PAGE_W, VIEW_H);	/* clips the pages beside this one */

	lv_obj_set_pos(view, row_x - VIEW_PAD_X, VIEW_Y);
	tiles = h->n + 1;
	/* The row is larger than its tiles by the ring's reach on every side,
	 * so the ring around an edge tile is inside it (LVGL clips children). */
	row = ui_box(view, (tiles + UI_TILES_PER_PAGE) * TILE_PITCH + 2 * RING_OUT, TILE_H + 2 * RING_OUT);
	lv_obj_add_event_cb(row, gone, LV_EVENT_DELETE, NULL);

	for (int i = 0; i < h->n; i++) {
		const struct ui_phone *p = &h->phones[i];
		int on = p->state == PHONE_CONNECTED;
		lv_obj_t *t = tile(i, &icon_phone_56, p->name,
				   on ? "Connected" : p->state == PHONE_UNREACHABLE ? "Unreachable" :
				   p->state == PHONE_LAST_USED ? "Last used" : "Paired",
				   on ? T_ON_BLUE : T_INK,
				   on ? T_ON_BLUE : p->state == PHONE_LAST_USED ? T_INK : T_INK_MUTED,
				   p->state == PHONE_UNREACHABLE);

		if (on) {
			ui_bg(t, T_BLUE, 0);	/* blue is the phone that is in CarPlay now */
		} else {
			ui_fill(t, FILL_SURFACE, 0);
			ui_fill(t, FILL_SURFACE_RAISED, LV_STATE_PRESSED);
			ui_edge_light(t);
		}
	}
	/* "Pair a phone" is always the last tile, so Home is never empty. */
	lv_obj_t *pair = tile(h->n, &icon_plus_56, "Pair a phone", "Bluetooth", T_INK, T_INK_MUTED, 0);

	ui_fill(pair, FILL_SURFACE_RAISED, LV_STATE_PRESSED);
	lv_obj_add_event_cb(pair, draw_dotted, LV_EVENT_DRAW_MAIN, NULL);

	/* The knob's ring: one object that travels from tile to tile. */
	ring = ui_box(row, TILE_W + 2 * RING_OUT, TILE_H + 2 * RING_OUT);
	lv_obj_set_style_radius(ring, T_RADIUS_LG + T_SPACE_1 + T_RING, 0);
	lv_obj_set_style_border_width(ring, T_RING, 0);
	lv_obj_set_style_border_color(ring, lv_color_hex(T_AMBER), 0);

	dots = ui_box(content, LV_SIZE_CONTENT, 8);
	lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(dots, 12, 0);
	lv_obj_align(dots, LV_ALIGN_TOP_MID, row_x + ROW_W / 2 - T_SCREEN_W / 2, UI_COG_Y + UI_COG / 2 - 4);	/* under the row's middle, level with the cog */

	shown_page = 0;
	place(h->focus, 0);
}

void ui_home_focus(int focus)
{
	if (row)
		place(focus, 1);
}

void ui_home_bump(int right)
{
	if (ring && !lv_obj_has_flag(ring, LV_OBJ_FLAG_HIDDEN))
		ui_ring_bump(ring, right ? 1 : -1, 0);
}

void ui_home_ack(void)
{
	if (ring && !lv_obj_has_flag(ring, LV_OBJ_FLAG_HIDDEN))
		ui_ring_veil(ring);
}
