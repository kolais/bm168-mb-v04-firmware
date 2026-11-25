/* SPDX-License-Identifier: MIT
 * The launcher's navigation: which screen is shown, where the knob is, and
 * what a turn, a push, Back or a tap does (design/screens.md,
 * design/feedback.md).
 *
 * The application (launcherd) owns the facts in struct ui_model and is told
 * what the driver asked for through one callback. It never has to know which
 * list is open. The callback runs inside ui_nav_key() or a tap; it may change
 * the model and may call ui_nav_home() or ui_nav_wait(): the navigation
 * redraws afterwards.
 */
#ifndef UI_NAV_H
#define UI_NAV_H
#include "screens.h"

/* Settings the navigation changes by itself, in ui_model.set[]; the
 * application applies and keeps them (UI_EV_SETTING). */
enum ui_setting {
	SET_CONNECT_AT_IGNITION,	/* 0 off, 1 on */
	SET_OFFER_OTHER_PHONE,		/* the switch banner over CarPlay: 0 off, 1 on */
	SET_BRIGHTNESS,			/* 10-100 % */
	SET_STEERING_RIGHT,		/* 0 left, 1 right: the end of the top band Back and the cog are at */
	SET_EFFECTS,			/* 0 Lite, 1 Full */
	SET_VOLUME_MEDIA, SET_VOLUME_CALL, SET_VOLUME_NAVIGATION,	/* 0-100 % */
	SET_SOUNDS,			/* 0 off, 1 actions, 2 all */
	SET_BASS, SET_MID, SET_TREBLE,	/* -6..+6 dB */
	SET_DEVELOPER,			/* a shell over the unit's Wi-Fi: 0 off, 1 on */
	SET_COUNT
};

enum ui_event {
	UI_EV_LEAVE,		/* Back on Home: to the BMW picture */
	UI_EV_PHONE,		/* arg = phone: connect it, or go to CarPlay if it is connected */
	UI_EV_PAIR,		/* "Pair a phone" */
	UI_EV_CANCEL,		/* Back while connecting or pairing */
	UI_EV_DISCONNECT,	/* arg = phone */
	UI_EV_FORGET,		/* arg = phone: confirmed; remove it from the model */
	UI_EV_PROFILE,		/* arg = phone, value = profile 1-3 or 0: record it in the model */
	UI_EV_SETTING,		/* arg = enum ui_setting, value = its new value (already in set[]) */
	UI_EV_REBOOT,		/* confirmed */
	UI_EV_NAME,		/* a new name for the unit: ui_nav_entered(); put it in the model */
	UI_EV_SWITCH,		/* the banner's pill: arg = the phone to switch to */
	UI_EV_UPDATE,		/* "Start" on the software update's page: begin, and show ui_nav_receive */
	UI_EV_SOUND,		/* arg = enum ui_sound, already filtered by the "Launcher sounds" level */
};
typedef void (*ui_event_cb)(enum ui_event ev, int arg, int value);

struct ui_model {
	struct ui_phone *phones;
	int n;
	int profile_phone;	/* the active driver profile's phone, or -1 */
	int guest;		/* the Guest profile is active: no switch banner */
	enum ui_chip chip;	/* Home's header chip */
	const char *chip_text;
	const char *clock;	/* the top band */
	enum ui_bt bt;		/* its Bluetooth mark */
	const char *name;	/* the unit's name, as phones see it */
	int set[SET_COUNT];
	/* What the application cannot do yet is shown but switched off: the row
	 * is ink-faint, the knob skips it, a tap is denied. */
	unsigned char set_off[SET_COUNT];	/* a setting */
	int name_off, profile_off, forget_off;	/* the unit's name; a phone's profile; forgetting a phone */
	const char *kernel, *processor, *panel;	/* System info */
	int can_update;		/* "Software update" may be started: there is an updater, and the car stands still */
	int can_develop;	/* the image was built with a developer's key: System has "Developer access" */
};

/* After ui_frame_init(). The launcher starts unseen (ui_nav_hide()). */
void ui_nav_init(struct ui_model *m, ui_event_cb cb);

/* No launcher screen. hide: the layer is clear, CarPlay or the BMW picture
 * has the screen. cover: plain ground, for a hand-off between pictures. */
void ui_nav_hide(void);
void ui_nav_cover(void);
/* Over CarPlay (after ui_nav_hide()): offer the other phone for 8 s. Returns
 * 0 and shows nothing if the setting is off, for a guest, or with no other
 * phone to offer. A tap on its pill is UI_EV_SWITCH. */
int ui_nav_banner(void);
/* A finger coming down at (x, y) belongs to the launcher, not to CarPlay or
 * the car: always while a launcher screen or the cover is up; over CarPlay
 * only on the banner and 24px around it. */
int ui_nav_owns(int x, int y);

#define UI_FOCUS_DEFAULT -2	/* the connected phone, else the profile's, else the last used, else the first */
void ui_nav_home(int focus);	/* show Home with the ring on this tile */
/* Show the waiting screen, or change its line if it is the one shown. */
void ui_nav_wait(enum ui_wait kind, const char *name);
/* The update's receiving screen, after "Start" on its instruction page.
 * Step 1: how the phone joins the unit's Wi-Fi, with "Cancel" and "Next";
 * step 2: the address to open, and a line that says what the unit is doing.
 * Called again while it is shown, only the line changes.
 * ui_nav_receive_next() goes to step 2 (a phone has joined; "Next" does the
 * same by itself). "Cancel", by knob or finger, tells UI_EV_CANCEL and goes
 * back to the list the instruction page was opened from. */
void ui_nav_receive(const char *ssid, const char *psk, const char *addr, const char *status);
void ui_nav_receive_next(void);
int ui_nav_receive_step(void);	/* 1 or 2 while that screen is shown, else 0 */
void ui_nav_refresh(void);	/* the model changed: redraw what is shown; the knob stays where it is */

void ui_nav_key(enum ui_key k);
const char *ui_nav_entered(void);	/* the text of the last UI_EV_NAME */

/* What is shown, for tests and logs. */
enum ui_page {
	PAGE_HOME, PAGE_WAIT, PAGE_SETTINGS, PAGE_GENERAL, PAGE_PHONES, PAGE_PHONE, PAGE_PROFILE, PAGE_FORGET,
	PAGE_DISPLAY, PAGE_SOUND, PAGE_EQUALISER, PAGE_SYSTEM, PAGE_INFO, PAGE_REBOOT, PAGE_TEXT, PAGE_UPDATE,
	PAGE_RECEIVE,	/* the update is awaited over the unit's Wi-Fi (ui_nav_receive) */
	PAGE_HIDDEN	/* no launcher screen: clear, the cover, or the banner over CarPlay */
};
struct ui_nav_state {
	enum ui_page page;
	int top, adjusting;
	enum ui_wait wait;	/* the waiting screen: its line */
	int focus;		/* Home: the tile or UI_FOCUS_COG; a list: the row; the update's page: the button */
	const struct ui_list *list;	/* NULL unless a list is shown */
	const char *message;		/* the line of the last ui_nav_message, "" if none since the last input */
};
/* Something the screen cannot show by itself: `words` and a name, as one
 * line for three seconds ("Couldn't forget Work iPhone"), and the ring's
 * nudge with it. Nothing when no launcher screen is up. */
void ui_nav_message(const char *words, const char *name);
void ui_nav_state(struct ui_nav_state *s);
const struct ui_model *ui_nav_model(void);	/* the facts it draws from */
#endif
