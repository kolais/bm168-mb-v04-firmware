/* SPDX-License-Identifier: MIT
 * Text entry for names: one screen, two forms, chosen by the hand that
 * opened it and swapped by the hand that is used next (design/screens.md,
 * Text entry; design/components/TextEntry). The dial for the knob: the
 * characters stand on a circle and the amber ring travels round them. The
 * keyboard for touch. The field stays in the header band in both.
 */
#include "screens.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DIAL_R 180		/* the characters' circle */
#define DIAL_TRACK 296		/* a hairline circle inside it */
#define DIAL_RING 52
#define DIAL_CX (T_SCREEN_W / 2)
#define DIAL_CY ((T_SCREEN_H - T_BAND) / 2)
#define KEY_W 103
#define KEY_WIDE 158		/* shift, delete */
#define KEY_WIDER 214		/* "123", "Done" */
#define KEYS_W (10 * KEY_W + 9 * T_SPACE_2 + 2)	/* the top row of ten, as wide as the old content area allowed */

enum what { CHARACTER, TO_DIGITS, TO_CAPITALS, TO_SMALL, DELETE, SPACE, OK, SHIFT, LAYER, NOTHING };
enum set { CAPITALS, SMALL, DIGITS };

struct item {
	enum what what;
	char text[8];		/* what stands on the dial or the key */
};

static const char dial_digits[] = "0123456789-_.'&@!?+";
static const char *const key_rows[2][3] = {
	{ "qwertyuiop", "asdfghjkl", "zxcvbnm" },
	{ "1234567890", "-_.,'&+()", "!?@#/:;" },
};

static struct {
	char title[40], value[128];
	int max_chars;
	int dial;		/* the form that is shown */
	enum set set;		/* the dial's characters */
	int item;		/* the dial's ring */
	int digits, shift;	/* the keyboard's layer */
	void (*done)(const char *value);
	void (*sound)(enum ui_sound s);
} t;

static struct item items[48];	/* the dial's stops, or the keyboard's keys */
static int n_items;
static lv_obj_t *screen, *field, *ring, *big, *caption, *stops[48];

static void draw(void);

static void gone(lv_event_t *e)
{
	(void)e;
	screen = field = ring = big = caption = NULL;
}

/* ---- the text ---- */

static int chars(const char *s)
{
	int n = 0;

	for (; *s; s++)
		n += ((unsigned char)*s & 0xC0) != 0x80;
	return n;
}

/* What an item does. Each typed or deleted character plays `key`; deleting
 * from an empty field bumps; typing into a full one denies. */
static void act(const struct item *it)
{
	size_t len = strlen(t.value);

	switch (it->what) {
	case OK:
		t.sound(SND_SELECT);
		t.done(len ? t.value : NULL);	/* an empty field saves nothing */
		return;
	case DELETE:
		if (!len) {
			t.sound(SND_BUMP);
			return;
		}
		do
			len--;
		while (len > 0 && ((unsigned char)t.value[len] & 0xC0) == 0x80);	/* a whole character */
		t.value[len] = 0;
		t.sound(SND_KEY);
		break;
	case TO_DIGITS:
	case TO_CAPITALS:
	case TO_SMALL:
		t.set = it->what == TO_DIGITS ? DIGITS : it->what == TO_CAPITALS ? CAPITALS : SMALL;
		t.item = 0;
		t.sound(SND_KEY);
		draw();
		return;
	case SHIFT:
		t.shift = !t.shift;
		t.sound(SND_KEY);
		draw();
		return;
	case LAYER:
		t.digits = !t.digits;
		t.sound(SND_KEY);
		draw();
		return;
	case NOTHING:
		return;
	case SPACE:
	case CHARACTER:
		if (chars(t.value) >= t.max_chars || len + 2 > sizeof(t.value)) {
			t.sound(SND_DENY);
			return;
		}
		t.value[len] = it->what == SPACE ? ' ' : it->text[0];
		t.value[len + 1] = 0;
		t.sound(SND_KEY);
		break;
	}
	lv_label_set_text(field, t.value);
}

/* ---- the dial ---- */

static struct item *add(enum what what, const char *text)
{
	struct item *it = &items[n_items++];

	it->what = what;
	snprintf(it->text, sizeof(it->text), "%s", text);
	return it;
}

static void dial_items(void)
{
	n_items = 0;
	if (t.set == DIGITS) {
		for (const char *c = dial_digits; *c; c++)
			add(CHARACTER, (char[]){ *c, 0 });
		add(TO_CAPITALS, "ABC");
	} else {
		for (int i = 0; i < 26; i++)
			add(CHARACTER, (char[]){ (char)((t.set == CAPITALS ? 'A' : 'a') + i), 0 });
		add(TO_DIGITS, "123");
		add(t.set == CAPITALS ? TO_SMALL : TO_CAPITALS, t.set == CAPITALS ? "abc" : "ABC");
	}
	add(DELETE, "\xE2\x8C\xAB");
	add(SPACE, "\xE2\x90\xA3");
	add(OK, "OK");
}

/* Where stop i stands: degrees clockwise from the top. The characters run
 * from the lower left round to the lower right; the functions sit along the
 * bottom, each as wide as 1.7 characters, with their middle at the bottom. */
static double dial_angle(int i)
{
	double at = 0, total = 0, functions = 0, centre = 0;

	for (int k = 0; k < n_items; k++) {
		double w = items[k].what == CHARACTER ? 1 : 1.7;

		if (k == i)
			centre = total + w / 2;
		total += w;
		if (items[k].what != CHARACTER)
			functions += w;
	}
	at = 180 + (centre - (total - functions / 2)) * 360 / total;
	return at * 3.14159265358979 / 180;
}

static const char *dial_caption(const struct item *it)
{
	return it->what == OK ? "Done" : it->what == DELETE ? "Delete" : it->what == SPACE ? "Space" :
	       it->what == TO_DIGITS ? "Numbers" : it->what == TO_CAPITALS ? "Capitals" :
	       it->what == TO_SMALL ? "Small letters" : "";
}

/* The characters stay put; the ring travels, and the centre repeats what it is on. */
static void dial_place(int travel)
{
	double a = dial_angle(t.item);

	ui_move(ring, DIAL_CX + (int)lround(DIAL_R * sin(a)) - DIAL_RING / 2,
		DIAL_CY - (int)lround(DIAL_R * cos(a)) - DIAL_RING / 2, travel ? T_DUR_STEP : 0);
	for (int i = 0; i < n_items; i++)
		lv_obj_set_style_text_color(stops[i], lv_color_hex(i == t.item ? T_INK : T_INK_MUTED), 0);
	lv_label_set_text(big, items[t.item].text);
	lv_label_set_text(caption, dial_caption(&items[t.item]));
}

static void to_keyboard(lv_event_t *e)
{
	(void)e;
	/* A touch while the dial is shown: the keyboard takes its place. The
	 * swap types nothing and keeps the text. */
	t.dial = 0;
	draw();
}

static void draw_dial(void)
{
	lv_obj_t *dial = ui_box(screen, T_SCREEN_W, T_SCREEN_H - T_BAND);

	lv_obj_set_pos(dial, 0, T_BAND);
	lv_obj_add_flag(dial, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(dial, to_keyboard, LV_EVENT_PRESSED, NULL);

	lv_obj_t *track = ui_box(dial, DIAL_TRACK, DIAL_TRACK);

	lv_obj_center(track);
	lv_obj_set_style_radius(track, DIAL_TRACK / 2, 0);
	lv_obj_set_style_border_width(track, 1, 0);
	lv_obj_set_style_border_color(track, lv_color_hex(T_HAIRLINE), 0);

	ring = ui_box(dial, DIAL_RING, DIAL_RING);	/* under the characters */
	lv_obj_set_style_radius(ring, DIAL_RING / 2, 0);
	lv_obj_set_style_border_width(ring, T_RING, 0);
	lv_obj_set_style_border_color(ring, lv_color_hex(T_AMBER), 0);
	ui_bg(ring, T_AMBER_WASH, 0);

	dial_items();
	if (t.item >= n_items)
		t.item = 0;
	for (int i = 0; i < n_items; i++) {
		double a = dial_angle(i);

		stops[i] = ui_text(dial, items[i].what == CHARACTER ? TY_NAME_28 : TY_LABEL_20, T_INK_MUTED, items[i].text);
		lv_obj_align(stops[i], LV_ALIGN_CENTER, (int)lround(DIAL_R * sin(a)), -(int)lround(DIAL_R * cos(a)));
	}

	lv_obj_t *mid = ui_box(dial, 200, 120);

	lv_obj_center(mid);
	lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	big = ui_text(mid, TY_DISPLAY_48, T_INK, "");
	caption = ui_text(mid, TY_LABEL_20, T_INK_MUTED, "");
	dial_place(0);
}

/* ---- the keyboard ---- */

static void key_tapped(lv_event_t *e)
{
	act(&items[(intptr_t)lv_event_get_user_data(e)]);	/* a key types on release */
}

static lv_obj_t *key(lv_obj_t *row, int w, enum what what, const char *text, enum ui_type type, int quiet, int lit)
{
	struct item *it = add(what, text);
	lv_obj_t *k = ui_box(row, w, T_BAND);

	lv_obj_set_style_radius(k, T_RADIUS_SM, 0);
	if (lit) {		/* "Done", and shift while it is on */
		ui_bg(k, T_INK, 0);
	} else {
		ui_bg(k, quiet ? T_SURFACE : T_SURFACE_RAISED, 0);
		ui_edge_light(k);
	}
	if (what != NOTHING) {
		ui_bg(k, T_OUTLINE, LV_STATE_PRESSED);	/* a key darkens while the finger is down */
		lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(k, key_tapped, LV_EVENT_CLICKED, (void *)(intptr_t)(it - items));
	}
	lv_obj_center(ui_text(k, type, lit ? T_GROUND : T_INK, text));
	return k;
}

static void draw_keyboard(void)
{
	n_items = 0;
	for (int r = 0; r < 4; r++) {
		lv_obj_t *row = ui_box(screen, LV_SIZE_CONTENT, T_BAND);

		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_style_pad_column(row, T_SPACE_2, 0);
		lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 2 * T_BAND + r * (T_BAND + T_SPACE_2));
		if (r == 3) {
			key(row, KEY_WIDER, LAYER, t.digits ? "abc" : "123", TY_LABEL_20, 1, 0);
			key(row, KEYS_W - 2 * (KEY_WIDER + T_SPACE_2) - 2, SPACE, "space", TY_LABEL_20, 1, 0);
			key(row, KEY_WIDER, OK, "Done", TY_LABEL_20, 1, 1);
			break;
		}
		if (r == 2)	/* the digits have no capitals: the key is there, and empty */
			key(row, KEY_WIDE, t.digits ? NOTHING : SHIFT, t.digits ? "" : "\xE2\x87\xA7", TY_ROW_32, 1,
			    !t.digits && t.shift);
		for (const char *c = key_rows[t.digits][r]; *c; c++) {
			char ch = !t.digits && t.shift ? (char)(*c - 'a' + 'A') : *c;

			key(row, KEY_W, CHARACTER, (char[]){ ch, 0 }, TY_ROW_32, 0, 0);
		}
		if (r == 2)
			key(row, KEY_WIDE, DELETE, "\xE2\x8C\xAB", TY_ROW_32, 1, 0);
	}
}

/* ---- the screen ---- */

#define CANCEL_W 132

static void cancel_tapped(lv_event_t *e)
{
	(void)e;
	ui_text_key(KEY_BACK);
}

static void draw(void)
{
	lv_obj_t *content = ui_frame_content();

	screen = ui_box(content, T_SCREEN_W, T_SCREEN_H);
	lv_obj_add_event_cb(screen, gone, LV_EVENT_DELETE, NULL);

	/* The field: in the header band beside the title, in both forms, so it
	 * does not move when they swap. */
	lv_obj_t *head = ui_header(screen, t.title), *title = lv_obj_get_child(head, 0);

	lv_obj_update_layout(title);
	int x = ui_frame_head_x() + lv_obj_get_width(title) + T_SPACE_4;
	/* "Cancel" after the field: the way out without saving, by touch (the
	 * knob has its Back key; there is no Back button on the screen). */
	lv_obj_t *cancel = ui_box(head, CANCEL_W, T_PILL_H);

	lv_obj_align(cancel, LV_ALIGN_LEFT_MID, ui_frame_head_end() - CANCEL_W, 0);
	lv_obj_set_style_radius(cancel, T_RADIUS_PILL, 0);
	ui_bg(cancel, T_SURFACE_RAISED, 0);
	ui_edge_light(cancel);
	ui_bg(cancel, T_OUTLINE, LV_STATE_PRESSED);
	lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(cancel, (T_BAND - T_PILL_H) / 2);	/* the band's height for a finger */
	lv_obj_add_event_cb(cancel, cancel_tapped, LV_EVENT_CLICKED, NULL);
	lv_obj_center(ui_text(cancel, TY_LABEL_20, T_INK, "Cancel"));
	lv_obj_t *box = ui_box(head, ui_frame_head_end() - CANCEL_W - T_SPACE_3 - x, T_PILL_H);

	lv_obj_align(box, LV_ALIGN_LEFT_MID, x, 0);
	lv_obj_set_style_radius(box, T_RADIUS_SM, 0);
	ui_fill(box, FILL_SURFACE, 0);
	ui_edge_light(box);
	lv_obj_set_style_pad_left(box, T_SPACE_3, 0);
	lv_obj_set_style_pad_column(box, T_SPACE_1, 0);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	field = ui_text(box, TY_ROW_32, T_INK, t.value);
	lv_obj_t *caret = ui_box(box, 3, 32);

	ui_bg(caret, T_INK, 0);
	ui_blink(caret);

	if (t.dial)
		draw_dial();
	else
		draw_keyboard();
}

void ui_text_open(const char *title, const char *value, int max_chars, int by_touch,
		  void (*done)(const char *value), void (*sound)(enum ui_sound s))
{
	memset(&t, 0, sizeof(t));
	snprintf(t.title, sizeof(t.title), "%s", title);
	snprintf(t.value, sizeof(t.value), "%s", value ? value : "");
	t.max_chars = max_chars;
	t.dial = !by_touch;
	t.set = CAPITALS;
	t.done = done;
	t.sound = sound;
	draw();
}

void ui_text_key(enum ui_key k)
{
	if (!screen)
		return;
	if (k == KEY_BACK) {	/* leaves without saving */
		t.sound(SND_BACK);
		t.done(NULL);
		return;
	}
	if (!t.dial) {
		/* The knob while the keyboard is shown: the dial returns. The
		 * swap types nothing and keeps the text. */
		t.dial = 1;
		draw();
		return;
	}
	if (k == KEY_PUSH) {
		ui_ring_flash(ring, T_AMBER_WASH, T_AMBER_PRESS);
		act(&items[t.item]);
		return;
	}
	/* The dial is a circle: the one place where focus wraps. */
	t.item = (t.item + (k == KEY_RIGHT ? 1 : n_items - 1)) % n_items;
	dial_place(1);
	t.sound(SND_TICK);
}

int ui_text_shown(void)
{
	return screen != NULL;
}

void ui_text_state(struct ui_text_state *s)
{
	s->value = t.value;
	s->dial = t.dial;
	s->item = t.item;
	s->items = t.dial ? n_items : 0;
	s->caption = t.dial ? (items[t.item].what == CHARACTER ? items[t.item].text : dial_caption(&items[t.item])) : "";
	s->digits = t.digits;
	s->shift = t.shift;
}
