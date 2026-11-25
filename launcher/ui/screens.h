/* SPDX-License-Identifier: MIT
 * The launcher's screens (design/screens.md), 1280 x 480.
 *
 * The caller owns the state (phones, link, focus) and hands a screen a
 * description of what to show; a screen keeps no state of its own beyond
 * what is on the panel.
 */
#ifndef UI_SCREENS_H
#define UI_SCREENS_H
#include "theme.h"

/* ---- input and its sounds (design/feedback.md) ---- */

enum ui_key { KEY_LEFT, KEY_RIGHT, KEY_PUSH, KEY_BACK };	/* the knob: one detent, push; the Back key */

enum ui_sound {
	SND_TICK, SND_KEY, SND_BUMP, SND_SELECT, SND_BACK, SND_ON, SND_OFF, SND_DENY, SND_CONNECTED, SND_LOST
};

/* ---- frame: the top band's status, the cog on Home, and the content area ---- */

/* Bluetooth, as the top band shows it: no mark while it is off; the rune
 * while it is on; the rune with a line at either side while a phone is
 * connected to it (owner, 2026-11-23). */
enum ui_bt { UI_BT_OFF, UI_BT_ON, UI_BT_LINKED };

struct ui_rail {
	const char *clock;	/* "14:32" */
	enum ui_bt bt;		/* the Bluetooth mark */
	int rhd;		/* the driver sits on the right: the cog's side, and the tiles' */
	int cog_shown;		/* Home: the Settings cog at the bottom of the driver's side */
	int cog_focus;		/* the knob is on the cog */
};

/* Create the frame on the active LVGL screen (once, after theme_init()). */
void ui_frame_init(void);
/* What the launcher's layer shows. It lies over CarPlay's video and the
 * car's picture, and is transparent wherever it draws nothing. */
enum ui_layer {
	LAYER_CLEAR,	/* nothing: CarPlay or the BMW picture has the screen (the banner may lie on it) */
	LAYER_SCREEN,	/* a launcher screen, on `ground` */
	LAYER_GROUND,	/* plain `ground`: every hand-off between pictures is a cut on black */
};
void ui_frame_layer(enum ui_layer l);
enum ui_layer ui_frame_layer_now(void);
void ui_frame_rail(const struct ui_rail *r);
/* The content area, emptied: the whole screen. The frame draws on top of
 * it: Wi-Fi, Bluetooth and the clock at the right of the top band on every
 * screen, and on Home the Settings cog under the tiles on the driver's
 * side. The status, the last tile and the cog end on one line, 32px from
 * the edge (owner, 2026-11-22: no empty strip). There is no Back on the
 * screen: a list ends in a "Back" row, a waiting screen has "Cancel", and
 * Home has none (owner, 2026-11-22). A screen's own things in the band
 * (title, chip, field) stay between ui_frame_head_x() and
 * ui_frame_head_end(). */
lv_obj_t *ui_frame_content(void);
int ui_frame_rhd(void);
#define UI_COG 60
#define UI_COG_Y 410		/* under the tiles, which end at 384: its ring stays 19px clear of them */
#define UI_EDGE T_SPACE_5	/* the status, the tiles and the cog end this far from the screen's edge */
#define UI_BAR_STATUS 164	/* the clock, at the right of a fixed width (two marks stood before it once) */
int ui_frame_head_x(void);	/* where a title starts */
int ui_frame_head_end(void);	/* where a screen's own things in the band end */
void ui_frame_cog_bump(void);	/* the knob is on the cog and can go no further */
/* A tap on the cog (touch only). */
void ui_frame_on_tap(void (*cog)(void));
void ui_frame_message(const char *text);	/* a line in the top band for three seconds */
int ui_frame_message_shown(void);

/* A header band: the title; returns the band for a chip. */
lv_obj_t *ui_header(lv_obj_t *content, const char *title);
enum ui_chip { CHIP_UP, CHIP_IDLE };	/* the CarPlay sign, before the phone's name; a hollow ring, before words */
void ui_header_chip(lv_obj_t *header, enum ui_chip kind, const char *text);

/* ---- Home ---- */

/* One state per phone; when several apply the caller picks the first of
 * connected, unreachable, last used, paired (design/screens.md, Home). */
enum ui_phone_state { PHONE_PAIRED, PHONE_LAST_USED, PHONE_UNREACHABLE, PHONE_CONNECTED };

struct ui_phone {
	const char *name;
	enum ui_phone_state state;
	int profile;		/* the driver profile this phone belongs to: 1-3, 0 none */
};

#define UI_TILES_PER_PAGE 4
#define UI_FOCUS_COG -1

struct ui_home {
	const struct ui_phone *phones;
	int n;
	enum ui_chip chip;
	const char *chip_text;	/* "Anna's iPhone connected", "No phone for profile 2", "Guest" */
	int focus;		/* tile with the knob: 0..n-1 a phone, n "Pair a phone", UI_FOCUS_COG */
};

/* tap: a finger lifted on tile `tile` (h->n is "Pair a phone"). */
void ui_home_show(const struct ui_home *h, void (*tap)(int tile));
/* Move the knob focus on the Home that is shown (ring travel, page change). */
void ui_home_focus(int focus);
void ui_home_bump(int right);	/* nothing further that way */
void ui_home_ack(void);		/* the ring answers a push */

/* ---- Waiting: a spinner and one line ---- */

enum ui_wait {
	WAIT_CONNECTING,	/* "Connecting to <phone>" */
	WAIT_RETRYING,		/* "Still trying <phone>": an attempt failed, the unit tries again */
	WAIT_FAILED,		/* "Couldn't connect to <phone>": the unit has given up; the arc stops */
	WAIT_PAIRING,		/* "Choose "<unit's name>" in your iPhone's Bluetooth settings" */
	WAIT_UPDATING,		/* "Updating. Keep the ignition on" (no name) */
};

/* cancel: "Cancel" under the line was tapped. The button is there while
 * the unit is trying or pairing, not once it has given up or is updating. */
void ui_wait_show(enum ui_wait kind, const char *name, void (*cancel)(void));
/* Change the line of the waiting screen that is shown; the spinner keeps turning. */
void ui_wait_set(enum ui_wait kind, const char *name);

/* ---- A list: the car-side screen (design/components/ListRow, SettingsScreen) ---- */

#define UI_LIST_ROWS 7		/* rows on the screen under the header band */
#define UI_LIST_MAX 24

struct ui_row {
	const char *label;
	const char *value;	/* text at the right edge, or NULL */
	int chevron;		/* the row opens another list */
	int checked;		/* the current one of several options: a check mark */
	int disabled;		/* ink-faint; the knob skips it */
	int back;		/* the list's last row, "Back": a chevron before the word */
	int amount;		/* an amount row: number, minus, bar, plus */
	int now, min, max;	/* the amount; a range from below zero fills from the middle */
	const char *unit;	/* "%", " dB" */
};

struct ui_list {
	const char *title;
	const struct ui_row *rows;	/* must stay valid while the list is shown */
	int n;
	int focus;		/* the row with the knob */
	int top;		/* the first row on the screen */
	int adjusting;		/* the focused amount is being adjusted */
};

/* tap: a finger lifted on row `row`: part 0 the row, -1 its minus, +1 its plus. */
void ui_list_show(const struct ui_list *l, void (*tap)(int row, int part));
/* The knob moved or adjusting began or ended: ring travel, scroll by whole rows. */
void ui_list_place(const struct ui_list *l);
/* A row's value changed. */
void ui_list_row(const struct ui_list *l, int row);
void ui_list_ack(const struct ui_list *l);	/* the ring answers a push */
void ui_list_put(const struct ui_list *l);	/* as ui_list_place, the ring there at once: a finger chose the row */
void ui_list_bump(int down);			/* the end of the list */
void ui_list_collapse(const struct ui_list *l, int row, void (*done)(void));	/* the row closes, then done() */
void ui_list_deny(int row);			/* a tap on a disabled row */

/* ---- The instruction page: numbered steps and two buttons ---- */

struct ui_steps {
	const char *title;
	const char *const *steps;	/* up to four, each one sentence */
	int n;
	const char *cancel, *start;	/* the two buttons' words */
	int focus;			/* the button with the knob: 0 cancel, 1 start */
};

/* tap: a finger lifted on button 0 or 1. */
void ui_steps_show(const struct ui_steps *s, void (*tap)(int button));
void ui_steps_focus(int button);
void ui_steps_bump(int button, int right);	/* the knob can go no further */

/* ---- The update's receiving screen (design/screens.md, Software update) ---- */

struct ui_receive {
	int step;		/* 1: join the unit's Wi-Fi; 2: open the address and send the file */
	const char *ssid, *psk;	/* the unit's Wi-Fi, as the phone must join it */
	const char *addr;	/* what to open in the phone's browser */
	const char *status;	/* step 2: the line that says what the unit is doing */
	int focus;		/* step 1: the button with the knob, 0 "Cancel", 1 "Next" */
};

/* tap: a finger lifted on button 0 ("Cancel") or 1 ("Next", step 1 only). */
void ui_receive_show(const struct ui_receive *r, void (*tap)(int button));
void ui_receive_focus(int button);
void ui_receive_status(const char *status);	/* the line alone changes */
int ui_receive_shown(void);			/* the step that is drawn, 0 if none */
void ui_receive_bump(int button, int right);	/* the knob has nowhere to go */

/* ---- The switch banner: the one thing drawn over CarPlay (design/screens.md, Over CarPlay) ---- */

/* current: the phone that is in CarPlay; other: the phone offered.
 * tap: the banner was tapped, anywhere on it: switch. It goes by itself
 * after 8 s, or at a tap just beside it. Touch only: it never takes the
 * knob's ring. */
void ui_banner_show(const char *current, const char *other, void (*tap)(void));
void ui_banner_hide(void);
int ui_banner_shown(void);
/* A finger coming down at (x, y) is the banner's, not CarPlay's: on it or
 * within 24px of it. */
int ui_banner_owns(int x, int y);

/* ---- Text entry: a dial for the knob, a keyboard for touch (design/components/TextEntry) ---- */

/* by_touch: opened with a tap, so the keyboard comes first; otherwise the dial.
 * done: "OK"/"Done" with the new text, or NULL when the driver left with
 * Back or saved nothing. sound: what to play. */
void ui_text_open(const char *title, const char *value, int max_chars, int by_touch,
		  void (*done)(const char *value), void (*sound)(enum ui_sound s));
void ui_text_key(enum ui_key k);
int ui_text_shown(void);

struct ui_text_state {		/* for tests and logs */
	const char *value;
	int dial;		/* the dial is shown; otherwise the keyboard */
	int item, items;	/* the dial: where the ring is, and how many stops it has */
	const char *caption;	/* the dial: what the ring is on ("A", "Delete", "Done") */
	int digits, shift;	/* the keyboard: its layer */
};
void ui_text_state(struct ui_text_state *s);
#endif
