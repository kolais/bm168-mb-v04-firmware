/* SPDX-License-Identifier: MIT
 * The launcher's look: see theme.h and design/README.md.
 */
#include "theme.h"
#include <stdio.h>
#include <string.h>

/* Inter (SIL OFL 1.1), three weights, embedded by the build (cmake/embed.cmake).
 * Glyphs are rendered from the outlines on first use and cached, so a phone's
 * name may use any script Inter covers (Latin, Greek, Cyrillic) at no cost
 * in image size per type style. */
extern const unsigned char font_inter_light[], font_inter_regular[], font_inter_semibold[];
extern const unsigned int font_inter_light_len, font_inter_regular_len, font_inter_semibold_len;

static const struct {
	int size, line, weight, space;
} type[TY_COUNT] = {
	[TY_CLOCK_96] = { T_CLOCK_96_SIZE, T_CLOCK_96_LINE, T_CLOCK_96_WEIGHT, T_CLOCK_96_SPACE },
	[TY_DISPLAY_48] = { T_DISPLAY_48_SIZE, T_DISPLAY_48_LINE, T_DISPLAY_48_WEIGHT, T_DISPLAY_48_SPACE },
	[TY_TITLE_36] = { T_TITLE_36_SIZE, T_TITLE_36_LINE, T_TITLE_36_WEIGHT, T_TITLE_36_SPACE },
	[TY_NAME_28] = { T_NAME_28_SIZE, T_NAME_28_LINE, T_NAME_28_WEIGHT, T_NAME_28_SPACE },
	[TY_ROW_32] = { T_ROW_32_SIZE, T_ROW_32_LINE, T_ROW_32_WEIGHT, T_ROW_32_SPACE },
	[TY_BODY_24] = { T_BODY_24_SIZE, T_BODY_24_LINE, T_BODY_24_WEIGHT, T_BODY_24_SPACE },
	[TY_LABEL_20] = { T_LABEL_20_SIZE, T_LABEL_20_LINE, T_LABEL_20_WEIGHT, T_LABEL_20_SPACE },
	[TY_EYEBROW_20] = { T_EYEBROW_20_SIZE, T_EYEBROW_20_LINE, T_EYEBROW_20_WEIGHT, T_EYEBROW_20_SPACE },
};

static lv_font_t *font[TY_COUNT];
static enum ui_mode mode = UI_LITE;

/* Glyphs kept per font: the clock needs ten digits and a colon, text styles
 * a few alphabets' worth. */
#define GLYPHS_CLOCK 16
#define GLYPHS_TEXT 384

int theme_init(void)
{
	for (int t = 0; t < TY_COUNT; t++) {
		/* Styles of one size and weight share a font (label-20 and eyebrow-20). */
		for (int u = 0; u < t && !font[t]; u++)
			if (type[u].size == type[t].size && type[u].weight == type[t].weight)
				font[t] = font[u];
		if (font[t])
			continue;
		const unsigned char *data = type[t].weight <= 300 ? font_inter_light :
					    type[t].weight <= 400 ? font_inter_regular : font_inter_semibold;
		unsigned int len = type[t].weight <= 300 ? font_inter_light_len :
				   type[t].weight <= 400 ? font_inter_regular_len : font_inter_semibold_len;

		font[t] = lv_tiny_ttf_create_data_ex(data, len, type[t].size, LV_FONT_KERNING_NORMAL,
						     t == TY_CLOCK_96 ? GLYPHS_CLOCK : GLYPHS_TEXT);
		if (!font[t])
			return -1;
	}
	return 0;
}

void theme_set_mode(enum ui_mode m)
{
	mode = m;
}

enum ui_mode theme_mode(void)
{
	return mode;
}

const lv_font_t *ui_font(enum ui_type t)
{
	return font[t];
}

int ui_line(enum ui_type t)
{
	return type[t].line;
}

lv_obj_t *ui_box(lv_obj_t *parent, int w, int h)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, w, h);
	/* Not scrollable, and a finger goes through it to what is beneath:
	 * whatever takes a tap asks for it. */
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	return o;
}

lv_obj_t *ui_text(lv_obj_t *parent, enum ui_type t, uint32_t color, const char *s)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font[t], 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_obj_set_style_text_letter_space(l, type[t].space, 0);
	lv_obj_set_style_text_line_space(l, type[t].line - lv_font_get_line_height(font[t]), 0);
	lv_label_set_text(l, s);
	return l;
}

lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *icon, uint32_t color)
{
	lv_obj_t *i = lv_image_create(parent);

	lv_image_set_src(i, icon);
	ui_icon_color(i, color);
	return i;
}

void ui_icon_color(lv_obj_t *icon, uint32_t color)
{
	/* An alpha map has no colour of its own: LVGL draws it in the recolour. */
	lv_obj_set_style_image_recolor(icon, lv_color_hex(color), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
}

void ui_bg(lv_obj_t *o, uint32_t color, lv_style_selector_t sel)
{
	lv_obj_set_style_bg_color(o, lv_color_hex(color), sel);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, sel);
}

void ui_fill(lv_obj_t *o, enum ui_fill f, lv_style_selector_t sel)
{
	if (mode == UI_FULL) {
		lv_obj_set_style_bg_color(o, lv_color_hex(f == FILL_SURFACE ? T_GLASS : T_GLASS_RAISED), sel);
		lv_obj_set_style_bg_opa(o, f == FILL_SURFACE ? T_GLASS_OPA : T_GLASS_RAISED_OPA, sel);
	} else {
		ui_bg(o, f == FILL_SURFACE ? T_SURFACE : T_SURFACE_RAISED, sel);
	}
}

void ui_edge_light(lv_obj_t *o)
{
	lv_obj_set_style_border_side(o, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_width(o, 1, 0);
	lv_obj_set_style_border_color(o, lv_color_hex(T_EDGE_LIGHT), 0);
	lv_obj_set_style_border_opa(o, T_EDGE_LIGHT_OPA, 0);
}

void ui_ring_on(lv_obj_t *o, int pad, int on)
{
	lv_obj_set_style_outline_width(o, on ? T_RING : 0, 0);
	lv_obj_set_style_outline_pad(o, pad, 0);
	lv_obj_set_style_outline_color(o, lv_color_hex(T_AMBER), 0);
	lv_obj_set_style_outline_opa(o, LV_OPA_COVER, 0);
}

static void anim_x(void *o, int32_t v)
{
	lv_obj_set_x(o, v);
}

static void anim_y(void *o, int32_t v)
{
	lv_obj_set_y(o, v);
}

void ui_move(lv_obj_t *o, int x, int y, int ms)
{
	lv_anim_delete(o, anim_x);
	lv_anim_delete(o, anim_y);
	if (mode != UI_FULL || ms <= 0) {
		lv_obj_set_pos(o, x, y);
		return;
	}
	lv_anim_t a;

	/* From wherever the object is now: a new input retargets a move in
	 * flight (design/feedback.md: input is never queued behind an animation). */
	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, anim_x);
	lv_anim_set_values(&a, lv_obj_get_x(o), x);
	lv_anim_start(&a);
	lv_anim_set_exec_cb(&a, anim_y);
	lv_anim_set_values(&a, lv_obj_get_y(o), y);
	lv_anim_start(&a);
}

static void turn(void *arc, int32_t deg)
{
	deg %= 360;
	if (lv_arc_get_rotation(arc) != deg)	/* a step that has not come yet redraws nothing */
		lv_arc_set_rotation(arc, deg);
}

void ui_turn(lv_obj_t *arc, int deg, int ms)
{
	lv_anim_t a;

	lv_arc_set_rotation(arc, deg);
	lv_anim_init(&a);
	lv_anim_set_var(&a, arc);
	lv_anim_set_exec_cb(&a, turn);
	lv_anim_set_values(&a, deg, deg + 360);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	/* Smoothly in both modes. Lite stepped it twelve times per turn; the
	 * owner found that choppy at the bench (2026-11-22), and the arc is
	 * small: about 29 redraws of 136 x 136 a second, with the launcher's
	 * whole CPU time under 5 % over two connects. */
	lv_anim_set_path_cb(&a, lv_anim_path_linear);
	lv_anim_start(&a);
}

/* ---- answers to an input ---- */

static void nothing(void *o, int32_t v)
{
	(void)o;
	(void)v;
}

static void flash_done(lv_anim_t *a)
{
	ui_bg(a->var, (uint32_t)(uintptr_t)lv_anim_get_user_data(a), 0);
}

int ui_ack_ms(void)
{
	return mode == UI_FULL ? T_DUR_ACK : 100;
}

void ui_ring_flash(lv_obj_t *ring, uint32_t rest, uint32_t lit)
{
	lv_anim_t a;

	lv_anim_delete(ring, nothing);
	ui_bg(ring, lit, 0);
	lv_anim_init(&a);
	lv_anim_set_var(&a, ring);
	lv_anim_set_exec_cb(&a, nothing);
	lv_anim_set_duration(&a, ui_ack_ms());
	lv_anim_set_user_data(&a, (void *)(uintptr_t)rest);
	lv_anim_set_completed_cb(&a, flash_done);
	lv_anim_start(&a);
}

void ui_ring_fill(lv_obj_t *ring, uint32_t color)
{
	lv_anim_delete(ring, nothing);
	ui_bg(ring, color, 0);
}

static void thick(void *ring, int32_t v)
{
	(void)v;
	lv_obj_set_style_border_width(ring, T_RING + 2, 0);
}

static void thick_done(lv_anim_t *a)
{
	lv_obj_set_style_border_width(a->var, T_RING, 0);
}

static void nudge_x(void *o, int32_t v)
{
	lv_obj_set_style_translate_x(o, v, 0);
}

static void nudge_y(void *o, int32_t v)
{
	lv_obj_set_style_translate_y(o, v, 0);
}

void ui_ring_bump(lv_obj_t *ring, int dx, int dy)
{
	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, ring);
	if (mode != UI_FULL) {
		lv_anim_delete(ring, thick);
		lv_anim_set_exec_cb(&a, thick);
		lv_anim_set_duration(&a, 100);
		lv_anim_set_completed_cb(&a, thick_done);
		lv_anim_start(&a);
		return;
	}
	/* out in the first 40% of the time, back in the rest */
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_duration(&a, T_DUR_BUMP * 2 / 5);
	lv_anim_set_playback_duration(&a, T_DUR_BUMP * 3 / 5);
	if (dx) {
		lv_anim_set_exec_cb(&a, nudge_x);
		lv_anim_set_values(&a, 0, dx > 0 ? 6 : -6);
		lv_anim_start(&a);
	}
	if (dy) {
		lv_anim_set_exec_cb(&a, nudge_y);
		lv_anim_set_values(&a, 0, dy > 0 ? 6 : -6);
		lv_anim_start(&a);
	}
}

static void edge(void *row, int32_t v)
{
	(void)v;
	lv_obj_set_style_outline_width(row, 1, 0);
	lv_obj_set_style_outline_pad(row, -1, 0);
	lv_obj_set_style_outline_color(row, lv_color_hex(T_OUTLINE), 0);
}

static void edge_done(lv_anim_t *a)
{
	lv_obj_set_style_outline_width(a->var, 0, 0);
}

static void shake(void *label, int32_t ms)
{
	/* 4px left, right, left, home: two shakes in 160 ms */
	lv_obj_set_style_translate_x(label, ms < 40 ? -4 : ms < 80 ? 4 : ms < 120 ? -4 : 0, 0);
}

void ui_deny(lv_obj_t *row, lv_obj_t *label)
{
	lv_anim_t a;

	lv_anim_init(&a);
	if (mode != UI_FULL) {
		lv_anim_delete(row, edge);
		lv_anim_set_var(&a, row);
		lv_anim_set_exec_cb(&a, edge);
		lv_anim_set_duration(&a, 200);
		lv_anim_set_completed_cb(&a, edge_done);
	} else {
		lv_anim_set_var(&a, label);
		lv_anim_set_exec_cb(&a, shake);
		lv_anim_set_values(&a, 0, 160);
		lv_anim_set_duration(&a, 160);
	}
	lv_anim_start(&a);
}

static void veil_done(lv_anim_t *a)
{
	lv_obj_set_style_bg_opa(a->var, LV_OPA_TRANSP, 0);
}

void ui_ring_veil(lv_obj_t *ring)
{
	lv_anim_t a;

	lv_anim_delete(ring, nothing);
	lv_obj_set_style_bg_color(ring, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(ring, 31, 0);	/* white at 12% */
	lv_anim_init(&a);
	lv_anim_set_var(&a, ring);
	lv_anim_set_exec_cb(&a, nothing);
	lv_anim_set_duration(&a, ui_ack_ms());
	lv_anim_set_completed_cb(&a, veil_done);
	lv_anim_start(&a);
}

static void blink(void *o, int32_t on)
{
	lv_obj_set_style_opa(o, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static int32_t path_halves(const lv_anim_t *a)
{
	return a->act_time * 2 < a->duration ? a->start_value : a->end_value;
}

void ui_blink(lv_obj_t *o)
{
	lv_anim_t a;

	if (mode != UI_FULL)
		return;
	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	lv_anim_set_exec_cb(&a, blink);
	lv_anim_set_values(&a, 1, 0);
	lv_anim_set_duration(&a, 1000);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_path_cb(&a, path_halves);
	lv_anim_start(&a);
}

static void outline_thick(void *o, int32_t v)
{
	(void)v;
	lv_obj_set_style_outline_width(o, T_RING + 2, 0);
}

static void outline_thin(lv_anim_t *a)
{
	lv_obj_set_style_outline_width(a->var, T_RING, 0);
}

void ui_outline_bump(lv_obj_t *o, int dx)
{
	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	if (mode != UI_FULL) {
		lv_anim_delete(o, outline_thick);
		lv_anim_set_exec_cb(&a, outline_thick);
		lv_anim_set_duration(&a, 100);
		lv_anim_set_completed_cb(&a, outline_thin);
	} else {
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_set_duration(&a, T_DUR_BUMP * 2 / 5);
		lv_anim_set_playback_duration(&a, T_DUR_BUMP * 3 / 5);
		lv_anim_set_exec_cb(&a, nudge_x);
		lv_anim_set_values(&a, 0, dx > 0 ? 6 : -6);
	}
	lv_anim_start(&a);
}

/* ---- text that must fit ---- */

static int width_of(const char *s, enum ui_type ty)
{
	lv_point_t size;

	lv_text_get_size(&size, s, font[ty], type[ty].space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return size.x;
}

/* The start of the UTF-8 character that byte n of s belongs to. */
static size_t utf8_floor(const char *s, size_t n)
{
	while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
		n--;
	return n;
}

void ui_fit_name(char *out, size_t size, const char *before, const char *name, const char *after,
		 enum ui_type ty, int max_w)
{
	char cut[128];
	size_t n = strlen(name);

	if (n > sizeof(cut) - 1)
		n = utf8_floor(name, sizeof(cut) - 1);
	memcpy(cut, name, n);
	cut[n] = 0;
	snprintf(out, size, "%s%s%s", before, cut, after);
	while (n > 0 && width_of(out, ty) > max_w) {
		n = utf8_floor(cut, n - 1);	/* one character shorter */
		while (n > 0 && cut[n - 1] == ' ')
			n--;
		cut[n] = 0;
		snprintf(out, size, "%s%s\xE2\x80\xA6%s", before, cut, after);
	}
}

/* ---- going ---- */

static void fade(void *o, int32_t opa)
{
	lv_obj_set_style_opa(o, opa, 0);
}

static void faded(lv_anim_t *a)
{
	lv_obj_delete(a->var);
}

void ui_dismiss(lv_obj_t *o, int ms)
{
	lv_anim_t a;

	if (mode != UI_FULL) {
		lv_obj_delete(o);
		return;
	}
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);	/* it is going: it takes no more taps */
	lv_anim_delete(o, fade);
	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	lv_anim_set_exec_cb(&a, fade);
	lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_completed_cb(&a, faded);
	lv_anim_start(&a);
}
