/* SPDX-License-Identifier: MIT
 * The update's receiving screen (design/screens.md, Software update): the
 * unit waits for the update file over its own Wi-Fi. Two steps: first the
 * Wi-Fi, as a code for the phone's camera (the name and password beside it
 * for joining by hand); then, once the phone has joined or "Next" is
 * pressed, the address to type, with one line that says what the unit is
 * doing. Read while parked, with the phone in the hand:
 * like the instruction page, running text is acceptable here.
 */
#include "screens.h"
#include <stdio.h>
#include <string.h>

#define CODE 216		/* the code's side */
#define CODE_PAD 12		/* the light margin a camera needs around it */
#define CODE_X 56
#define TEXT_X (CODE_X + CODE + 2 * CODE_PAD + T_SPACE_5)
#define TEXT_W (T_SCREEN_W - TEXT_X - 56)
#define BUTTON_H 64
#define BUTTON_W 200

static lv_obj_t *root, *line, *buttons[2];
static int shown_step;
static void (*on_tap)(int button);

static void gone(lv_event_t *e)
{
	(void)e;
	root = line = buttons[0] = buttons[1] = NULL;
	shown_step = 0;
}

static void tapped(lv_event_t *e)
{
	if (on_tap)
		on_tap((int)(intptr_t)lv_event_get_user_data(e));
}

/* "WIFI:T:WPA;S:<name>;P:<password>;;", the form phone cameras read; the
 * four characters that mean something in it are escaped. */
static void wifi_code(char *out, size_t size, const char *ssid, const char *psk)
{
	size_t n = (size_t)snprintf(out, size, "WIFI:T:WPA;S:");

	for (int part = 0; part < 2; part++) {
		for (const char *p = part ? psk : ssid; *p && n + 3 < size; p++) {
			if (strchr("\\;,:\"", *p))
				out[n++] = '\\';
			out[n++] = *p;
		}
		n += (size_t)snprintf(out + n, size - n, part ? ";;" : ";P:");
		if (n >= size)
			break;
	}
}

static lv_obj_t *words(lv_obj_t *col, const char *number, enum ui_type t, uint32_t ink, const char *text)
{
	lv_obj_t *row = ui_box(col, TEXT_W, LV_SIZE_CONTENT);
	lv_obj_t *label;

	ui_text(row, TY_BODY_24, T_INK, number);
	label = ui_text(row, t, ink, text);
	lv_obj_set_width(label, TEXT_W - T_SPACE_5);
	lv_obj_set_x(label, T_SPACE_5);
	return label;
}

static lv_obj_t *button(lv_obj_t *parent, int i, const char *label, int solid)
{
	lv_obj_t *b = ui_box(parent, LV_SIZE_CONTENT, BUTTON_H);

	lv_obj_set_style_min_width(b, BUTTON_W, 0);
	lv_obj_set_style_pad_hor(b, T_SPACE_5, 0);
	lv_obj_set_style_radius(b, T_RADIUS_PILL, 0);
	if (solid) {	/* the way on, filled ink as "Start" is */
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

void ui_receive_show(const struct ui_receive *r, void (*tap)(int button))
{
	lv_obj_t *content = ui_frame_content();
	char text[320];
	const int y = T_SCREEN_H - T_BAND - T_SPACE_5 - BUTTON_H;

	ui_header(content, "Software update");
	on_tap = tap;
	root = ui_box(content, T_SCREEN_W, T_SCREEN_H - T_BAND);
	lv_obj_set_pos(root, 0, T_BAND);
	lv_obj_add_event_cb(root, gone, LV_EVENT_DELETE, NULL);
	shown_step = r->step == 2 ? 2 : 1;

	/* Step 1 has a code for the phone's camera: the unit's Wi-Fi, dark on
	 * light whatever the theme, with its quiet margin. Step 2 has none:
	 * the address is plain HTTP, and the phone's browser opens that only
	 * when it is typed (from a code: "the request was for an HTTP URL with
	 * HTTPS-Only enabled"; owner's phone, 2026-11-24). So it stands large,
	 * to be typed once; the browser offers it by itself afterwards. */
	const int x = shown_step == 1 ? TEXT_X : CODE_X;

	if (shown_step == 1) {
		lv_obj_t *card = ui_box(root, CODE + 2 * CODE_PAD, CODE + 2 * CODE_PAD);

		lv_obj_set_pos(card, CODE_X, T_SPACE_2);
		lv_obj_set_style_radius(card, T_RADIUS_SM, 0);
		ui_bg(card, 0xffffff, 0);
#if LV_USE_QRCODE
		lv_obj_t *qr = lv_qrcode_create(card);

		wifi_code(text, sizeof(text), r->ssid, r->psk);
		lv_qrcode_set_size(qr, CODE);
		lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
		lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
		lv_qrcode_update(qr, text, (uint32_t)strlen(text));
		lv_obj_center(qr);
#endif
	}

	lv_obj_t *col = ui_box(root, TEXT_W, LV_SIZE_CONTENT);

	lv_obj_set_pos(col, x, T_SPACE_2);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, T_SPACE_2, 0);
	line = NULL;
	buttons[0] = button(root, 0, "Cancel", 0);
	lv_obj_set_pos(buttons[0], x, y);
	if (shown_step == 1) {
		words(col, "1.", TY_BODY_24, T_INK, "Point the phone\xE2\x80\x99s camera at the code and join the Wi-Fi.");
		snprintf(text, sizeof(text), "By hand: Wi-Fi %s, password %s", r->ssid, r->psk);
		words(col, "", TY_LABEL_20, T_INK_MUTED, text);
		words(col, "", TY_LABEL_20, T_INK_MUTED,
		      "The next step follows when the phone has joined. If it is on this Wi-Fi already, press Next.");
		buttons[1] = button(root, 1, "Next", 1);
		lv_obj_update_layout(buttons[0]);
		lv_obj_set_pos(buttons[1], TEXT_X + lv_obj_get_width(buttons[0]) + T_SPACE_4, y);
	} else {
		words(col, "2.", TY_BODY_24, T_INK, "In the phone\xE2\x80\x99s browser, type this address:");
		snprintf(text, sizeof(text), "http://%s", r->addr);
		words(col, "", TY_DISPLAY_48, T_INK, text);
		words(col, "", TY_BODY_24, T_INK, "Choose the update file there and send it.");
		line = words(col, "", TY_NAME_28, T_INK, r->status);
		buttons[1] = NULL;
	}
	ui_receive_focus(r->focus);
}

void ui_receive_focus(int b)
{
	for (int i = 0; i < 2; i++)
		if (buttons[i])
			ui_ring_on(buttons[i], T_SPACE_1, i == (buttons[1] ? b : 0));
}

void ui_receive_status(const char *status)
{
	if (line)
		lv_label_set_text(line, status);
}

int ui_receive_shown(void)
{
	return root ? shown_step : 0;
}

void ui_receive_bump(int b, int right)
{
	if (buttons[b] || buttons[0])
		ui_outline_bump(buttons[b] ? buttons[b] : buttons[0], right ? 1 : -1);
}
