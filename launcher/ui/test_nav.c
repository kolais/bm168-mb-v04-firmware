/* SPDX-License-Identifier: MIT
 * Tests of the launcher's navigation (nav.c) against design/screens.md and
 * design/feedback.md: where the knob goes, what a push, Back and a tap do,
 * and what the application is told. Runs on the host and under qemu; no unit
 * needed. Exit status 0 = all passed.
 */
#include "display.h"
#include "host.h"
#include "sample.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;

#define CHECK(cond) \
	do { \
		checks++; \
		if (!(cond)) { \
			fails++; \
			printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
		} \
	} while (0)

static struct ui_nav_state st(void)
{
	struct ui_nav_state s;

	ui_nav_state(&s);
	return s;
}

static const struct ui_row *row(int i)
{
	return &st().list->rows[i];
}

static int same(const char *a, const char *b)
{
	return a && b && !strcmp(a, b);
}

/* l, r: one detent; p: push; b: Back; w: 100 ms pass */
static void keys(const char *s)
{
	for (; *s; s++) {
		if (*s == 'w') {
			host_run(100);
			continue;
		}
		ui_nav_key(*s == 'l' ? KEY_LEFT : *s == 'r' ? KEY_RIGHT : *s == 'p' ? KEY_PUSH : KEY_BACK);
		host_run(0);
	}
}

static int told(enum ui_event ev)	/* how many times */
{
	int n = 0;

	for (int i = 0; i < sample_logged; i++)
		n += sample_log[i].ev == ev;
	return n;
}

static int sounded(enum ui_sound s)
{
	int n = 0;

	for (int i = 0; i < sample_logged; i++)
		n += sample_log[i].ev == UI_EV_SOUND && sample_log[i].arg == (int)s;
	return n;
}

static struct sample_record last(enum ui_event ev)
{
	struct sample_record r = { ev, -99, -99 };

	for (int i = 0; i < sample_logged; i++)
		if (sample_log[i].ev == ev)
			r = sample_log[i];
	return r;
}

static void start(enum sample_set which, int rhd)
{
	sample_reset(which);
	sample.set[SET_STEERING_RIGHT] = rhd;
	ui_nav_init(&sample, sample_event);
	ui_nav_home(UI_FOCUS_DEFAULT);
	host_run(0);
}

/* Home, cog on the left: cog, then the tiles, then nothing. */
static void home_left(void)
{
	start(SAMPLE_FIVE, 0);
	CHECK(st().page == PAGE_HOME && st().focus == 0);	/* the connected phone */
	keys("r");
	CHECK(st().focus == 1);
	keys("ll");
	CHECK(st().focus == UI_FOCUS_COG);
	keys("l");
	CHECK(st().focus == UI_FOCUS_COG && sounded(SND_BUMP) == 1);
	keys("r");
	CHECK(st().focus == 0);
	keys("rrrrr");
	CHECK(st().focus == 5);		/* "Pair a phone" */
	keys("r");
	CHECK(st().focus == 5 && sounded(SND_BUMP) == 2);
	keys("p");
	CHECK(told(UI_EV_PAIR) == 1);
	keys("lllll");
	keys("p");
	CHECK(last(UI_EV_PHONE).arg == 0);
	keys("b");
	CHECK(told(UI_EV_LEAVE) == 1 && st().page == PAGE_HOME);
}

/* Home, cog on the right: the tiles, then the cog. */
static void home_right(void)
{
	start(SAMPLE_FIVE, 1);
	keys("l");
	CHECK(st().focus == 0 && sounded(SND_BUMP) == 1);
	keys("rrrrr");
	CHECK(st().focus == 5);
	keys("r");
	CHECK(st().focus == UI_FOCUS_COG);
	keys("r");
	CHECK(st().focus == UI_FOCUS_COG && sounded(SND_BUMP) == 2);
	keys("l");
	CHECK(st().focus == 5);
	keys("rp");
	CHECK(st().page == PAGE_SETTINGS);
}

static void home_start(void)
{
	start(SAMPLE_TWO, 0);
	CHECK(st().focus == 0);		/* nobody connected: the last used phone */
	sample.profile_phone = 1;
	ui_nav_home(UI_FOCUS_DEFAULT);
	CHECK(st().focus == 1);		/* the active profile's phone comes before it */
	start(SAMPLE_NONE, 0);
	CHECK(st().focus == 0);		/* "Pair a phone" alone */
	keys("p");
	CHECK(told(UI_EV_PAIR) == 1);
	ui_nav_home(3);
	CHECK(st().focus == 0);		/* a tile that is not there: the last one */
}

static void lists(void)
{
	start(SAMPLE_FIVE, 0);
	keys("lp");
	CHECK(st().page == PAGE_SETTINGS && st().focus == 0 && st().list->n == 5);	/* four groups and "Back" */
	/* An "open" waits for the ring's answer; an input meanwhile acts on the arriving list. */
	keys("p");
	CHECK(st().page == PAGE_SETTINGS);
	keys("r");
	CHECK(st().page == PAGE_GENERAL && st().focus == 1);
	keys("b");
	CHECK(st().page == PAGE_SETTINGS && st().focus == 0);	/* the ring returns to the row that was opened */
	keys("pw");
	CHECK(st().page == PAGE_GENERAL && st().focus == 0);
	CHECK(same(row(0)->label, "Name") && same(row(0)->value, "BMW 28114"));
	keys("l");
	CHECK(st().focus == 0 && sounded(SND_BUMP) == 1);

	/* a choice steps to its next value at once */
	keys("r");
	CHECK(same(row(1)->value, "On"));
	keys("p");
	CHECK(sample.set[SET_CONNECT_AT_IGNITION] == 0 && same(row(1)->value, "Off"));
	CHECK(last(UI_EV_SETTING).arg == SET_CONNECT_AT_IGNITION && last(UI_EV_SETTING).value == 0);
	CHECK(sounded(SND_OFF) == 1);
	keys("p");
	CHECK(sample.set[SET_CONNECT_AT_IGNITION] == 1 && sounded(SND_ON) == 1);
	keys("rp");
	CHECK(sample.set[SET_OFFER_OTHER_PHONE] == 0);

	keys("b");
	CHECK(st().page == PAGE_SETTINGS);
	keys("rrr");
	CHECK(st().focus == 3);
	keys("r");
	CHECK(st().focus == 4 && row(4)->back && same(row(4)->label, "Back"));	/* the last row of every list */
	keys("r");
	CHECK(st().focus == 4);		/* no wrap-around */
	keys("p");	/* "Back" as a row does what the Back key does */
	CHECK(st().page == PAGE_HOME && st().focus == UI_FOCUS_COG);	/* out of Settings: back on the cog */
	keys("p" "b");
	CHECK(st().page == PAGE_HOME && st().focus == UI_FOCUS_COG);
}

static void amounts(void)
{
	start(SAMPLE_FIVE, 0);
	keys("lp" "rpw");
	CHECK(st().page == PAGE_DISPLAY && st().focus == 0 && row(0)->amount && row(0)->now == 70);
	keys("p");
	CHECK(st().adjusting);
	keys("r");
	CHECK(sample.set[SET_BRIGHTNESS] == 75 && row(0)->now == 75 && st().focus == 0);
	CHECK(last(UI_EV_SETTING).arg == SET_BRIGHTNESS && last(UI_EV_SETTING).value == 75);	/* applied as it is made */
	keys("ll");
	CHECK(sample.set[SET_BRIGHTNESS] == 65);
	keys("rrrrrrr");
	CHECK(sample.set[SET_BRIGHTNESS] == 100);
	int bumps = sounded(SND_BUMP);

	keys("r");
	CHECK(sample.set[SET_BRIGHTNESS] == 100 && sounded(SND_BUMP) == bumps + 1);
	keys("b");	/* Back keeps the amount and only ends adjusting */
	CHECK(!st().adjusting && st().page == PAGE_DISPLAY && sample.set[SET_BRIGHTNESS] == 100);
	keys("pllp");	/* so does a push */
	CHECK(!st().adjusting && sample.set[SET_BRIGHTNESS] == 90);
	keys("r");
	CHECK(st().focus == 1);

	/* "Steering side" moves Back and the cog at once; the list stays where it was */
	keys("p");
	CHECK(sample.set[SET_STEERING_RIGHT] == 1 && ui_frame_rhd() == 1);
	CHECK(st().page == PAGE_DISPLAY && st().focus == 1 && same(row(1)->value, "Right"));
	keys("p");
	CHECK(ui_frame_rhd() == 0);
	/* "Visual effects" switches the render mode at once */
	keys("rp");
	CHECK(sample.set[SET_EFFECTS] == 1 && theme_mode() == UI_FULL && st().focus == 2);
	keys("p");
	CHECK(theme_mode() == UI_LITE);

	/* an amount either side of zero; the equaliser's row says whether it is flat */
	keys("b" "rpw");
	CHECK(st().page == PAGE_SOUND && same(row(4)->value, "Flat") && row(4)->chevron);
	keys("rrrr" "pw");
	CHECK(st().page == PAGE_EQUALISER && row(0)->min == -6 && row(0)->max == 6);
	keys("plllllllp");
	CHECK(sample.set[SET_BASS] == -6);	/* stops at the end of its range */
	keys("b");
	CHECK(st().page == PAGE_SOUND && same(row(4)->value, "Custom"));
	/* three values: Off, Actions, All, and round again */
	keys("l");
	CHECK(st().focus == 3 && same(row(3)->value, "Actions"));
	keys("p");
	CHECK(sample.set[SET_SOUNDS] == 2);
	keys("p");
	CHECK(sample.set[SET_SOUNDS] == 0);
	keys("p");
	CHECK(sample.set[SET_SOUNDS] == 1);
}

static void phones(void)
{
	start(SAMPLE_FIVE, 0);
	keys("lp" "pw" "rrr" "pw");
	CHECK(st().page == PAGE_PHONES && st().list->n == 6 && same(row(1)->value, "Last used") && row(1)->chevron);
	keys("r" "pw");
	CHECK(st().page == PAGE_PHONE && same(st().list->title, "Work iPhone") && st().list->n == 5);
	CHECK(same(row(0)->value, "Last used") && same(row(1)->label, "Connect") && same(row(2)->value, "None"));
	CHECK(st().focus == 1);	/* one push does the likely thing: the ring starts on "Connect" */
	keys("l" "p");	/* "Status" is information: pushing it bumps */
	CHECK(st().page == PAGE_PHONE && sounded(SND_BUMP) == 1);

	/* the driver profile: the ring starts on the current one; a profile has one phone */
	keys("rr" "pw");
	CHECK(st().page == PAGE_PROFILE && st().focus == 0 && row(0)->checked && !row(2)->checked);
	keys("rr" "p");
	CHECK(last(UI_EV_PROFILE).arg == 1 && last(UI_EV_PROFILE).value == 2);
	CHECK(st().page == PAGE_PHONE && st().focus == 2 && same(row(2)->value, "Profile 2"));
	CHECK(sample.phones[1].profile == 2 && sample.phones[2].profile == 0);	/* moved from the phone that held it */
	keys("pw");
	CHECK(st().page == PAGE_PROFILE && st().focus == 2 && row(2)->checked);
	keys("b");

	keys("l" "p");
	CHECK(last(UI_EV_PHONE).arg == 1 && st().page == PAGE_PHONE);	/* "Connect" */

	/* forgetting takes two deliberate steps, and "Cancel" has the ring */
	keys("rr" "pw");
	CHECK(st().page == PAGE_FORGET && st().focus == 0 && same(st().list->title, "Forget Work iPhone"));
	CHECK(same(row(0)->label, "Cancel") && same(row(1)->label, "Forget now") && st().list->n == 2);	/* "Cancel" is its way back */
	keys("p");
	CHECK(st().page == PAGE_PHONE && told(UI_EV_FORGET) == 0);
	keys("pw" "r" "p");
	CHECK(last(UI_EV_FORGET).arg == 1 && sample.n == 4);
	/* its row once more where it stood, dimmed and closing; the ring stays there */
	CHECK(st().page == PAGE_PHONES && st().list->n == 6 && same(row(1)->label, "Work iPhone") && row(1)->disabled &&
	      st().list->focus == 1);
	CHECK(same(row(2)->label, sample.phones[1].name));
	keys("wwwwww");	/* a beat, and it has closed: the phone below is in the ring */
	CHECK(st().page == PAGE_PHONES && st().list->n == 5 && same(row(1)->label, sample.phones[1].name) && st().focus == 1);

	/* disconnecting acts at once and can be undone with one push */
	keys("l" "pw");
	CHECK(st().page == PAGE_PHONE && same(row(1)->label, "Disconnect") && same(row(0)->value, "Connected"));
	keys("p");
	CHECK(last(UI_EV_DISCONNECT).arg == 0 && sounded(SND_OFF) == 1);
	CHECK(st().page == PAGE_PHONE && same(row(1)->label, "Connect") && same(row(0)->value, "Last used"));

	/* the phone goes away while its list is open: back to the list of phones */
	sample_event(UI_EV_FORGET, 0, 0);
	ui_nav_refresh();
	CHECK(st().page == PAGE_PHONE);	/* another phone has taken its place in the list */
	while (sample.n)
		sample_event(UI_EV_FORGET, 0, 0);
	ui_nav_refresh();
	CHECK(st().page == PAGE_PHONES && st().list->n == 2 && same(row(0)->label, "No phones paired") && row(1)->back);
}

/* Deeper and back in Full: the screen is asked to arrive by a push; never in Lite. */
static void pushes(void)
{
	struct ui_display_stats d0, d;

	start(SAMPLE_FIVE, 0);
	ui_display_stats(&d0);
	keys("lp" "w" "pw" "b" "b");	/* Lite: Settings, a list in it, and out again */
	ui_display_stats(&d);
	CHECK(st().page == PAGE_HOME && d.forward == d0.forward && d.back == d0.back);

	sample.set[SET_EFFECTS] = 1;
	ui_nav_init(&sample, sample_event);
	ui_nav_home(UI_FOCUS_DEFAULT);
	host_run(0);
	keys("lp" "w");	/* the cog: Settings arrives */
	ui_display_stats(&d);
	CHECK(st().page == PAGE_SETTINGS && d.forward == d0.forward + 1 && d.back == d0.back);
	keys("pw");
	ui_display_stats(&d);
	CHECK(d.forward == d0.forward + 2);
	keys("b");
	ui_display_stats(&d);
	CHECK(d.back == d0.back + 1 && d.moved == 0);	/* the host's display cannot move screens: a cut */
	keys("b");
	ui_display_stats(&d);
	CHECK(st().page == PAGE_HOME && d.back == d0.back + 2 && d.forward == d0.forward + 2);
	sample.set[SET_EFFECTS] = 0;
	ui_nav_init(&sample, sample_event);
}

static void scrolling(void)
{
	start(SAMPLE_NINE, 0);
	keys("lp" "pw" "rrr" "pw");
	CHECK(st().page == PAGE_PHONES && st().list->n == 10 && st().top == 0);
	keys("rrrrrr");
	CHECK(st().focus == 6 && st().top == 0);	/* seven rows on the screen */
	keys("r");
	CHECK(st().focus == 7 && st().top == 1);	/* then by whole rows */
	keys("r");
	CHECK(st().focus == 8 && st().top == 2);
	keys("r");
	CHECK(st().focus == 9 && st().top == 3 && row(9)->back);	/* "Back" comes after the last phone */
	keys("r");
	CHECK(st().focus == 9 && st().top == 3);
	keys("lllllllll");
	CHECK(st().focus == 0 && st().top == 0);
}

static void system_pages(void)
{
	start(SAMPLE_FIVE, 0);
	keys("lp" "rrr" "pw");
	CHECK(st().page == PAGE_SYSTEM && st().list->n == 3);
	sample.can_update = 0;		/* the car moves, or there is no updater */
	keys("pw");
	CHECK(st().page == PAGE_INFO && same(row(0)->value, "Linux 4.19") && row(3)->disabled);
	keys("rr");
	CHECK(st().focus == 2);
	keys("r");
	CHECK(st().focus == 4);		/* the knob skips "Software update": "Back" is next */
	keys("l" "p");
	CHECK(st().page == PAGE_INFO && st().focus == 2);	/* information: a push bumps */
	keys("b" "r" "pw");
	CHECK(st().page == PAGE_REBOOT && st().focus == 0 && same(row(0)->label, "Cancel"));
	keys("p");
	CHECK(st().page == PAGE_SYSTEM && told(UI_EV_REBOOT) == 0);
	keys("pw" "r" "p");
	CHECK(told(UI_EV_REBOOT) == 1);

	/* an image with a developer's key: one more row, a choice like the others */
	start(SAMPLE_FIVE, 0);
	sample.can_develop = 1;
	keys("lp" "rrr" "pw");
	CHECK(st().page == PAGE_SYSTEM && st().list->n == 4 && same(row(1)->label, "Developer access") &&
	      same(row(1)->value, "Off") && same(row(2)->label, "Reboot"));
	keys("r" "p");
	CHECK(sample.set[SET_DEVELOPER] == 1 && same(row(1)->value, "On") && st().page == PAGE_SYSTEM);
	CHECK(last(UI_EV_SETTING).arg == SET_DEVELOPER && last(UI_EV_SETTING).value == 1);
	sample.can_develop = 0;
	sample.set[SET_DEVELOPER] = 0;
}

static void waiting(void)
{
	start(SAMPLE_FIVE, 0);
	keys("lp" "pw");
	ui_nav_wait(WAIT_CONNECTING, "Work iPhone");	/* from inside Settings: the lists are left */
	CHECK(st().page == PAGE_WAIT);
	keys("rlp");
	CHECK(st().page == PAGE_WAIT && told(UI_EV_CANCEL) == 0);
	ui_nav_wait(WAIT_RETRYING, "Work iPhone");
	keys("b");
	CHECK(told(UI_EV_CANCEL) == 1);
	ui_nav_wait(WAIT_UPDATING, "");
	keys("b");
	CHECK(told(UI_EV_CANCEL) == 1);	/* Back does nothing during an update */
	ui_nav_home(3);
	CHECK(st().page == PAGE_HOME && st().focus == 3);	/* "Home returns with the ring on that tile" */
}

static void sound_levels(void)
{
	start(SAMPLE_FIVE, 0);
	keys("r");
	CHECK(sounded(SND_TICK) == 0);	/* "Actions": everything except the per-detent tick */
	sample.set[SET_SOUNDS] = 2;
	keys("r");
	CHECK(sounded(SND_TICK) == 1);
	sample.set[SET_SOUNDS] = 0;
	keys("rrrrrr" "b");
	CHECK(told(UI_EV_SOUND) == 1 && told(UI_EV_LEAVE) == 1);	/* "Off": silence, but the events still come */
}

/* Touch: the same results as the knob. Coordinates with the driver on the left. */
static void taps(void)
{
	const int cog_x = UI_EDGE + UI_COG / 2, cog_y = UI_COG_Y + UI_COG / 2;
	const int rows_x = T_SPACE_5, rows_w = T_SCREEN_W - 2 * T_SPACE_5;	/* a list's rows: across the screen */
	const int tile_x = UI_EDGE, tile_pitch = 280;	/* 264 wide, 16 apart, from the driver's edge */
#define ROW_Y(i) (T_BAND + (i) * T_BAND + T_BAND / 2)

	start(SAMPLE_FIVE, 0);
	host_tap(tile_x + 132, 234);	/* the first tile, under the ring */
	CHECK(last(UI_EV_PHONE).arg == 0 && st().focus == 0);
	host_tap(tile_x + 2 * tile_pitch + 132, 234);	/* the third tile */
	CHECK(last(UI_EV_PHONE).arg == 2 && st().focus == 2);	/* the knob continues from the touched tile */
	host_tap(tile_x + 3 * tile_pitch + 270, 234);	/* beside the fourth tile: nothing */
	CHECK(told(UI_EV_PHONE) == 2);
	host_tap(cog_x, cog_y);
	CHECK(st().page == PAGE_SETTINGS);
	host_tap(rows_x + 300, ROW_Y(1));	/* a row that opens: at once when the finger lifts, no answer of the ring first */
	CHECK(st().page == PAGE_DISPLAY);
	host_tap(rows_x + rows_w - 30, ROW_Y(0));	/* plus */
	CHECK(sample.set[SET_BRIGHTNESS] == 75 && !st().adjusting);
	host_tap(rows_x + rows_w - 60 - 200 - 30, ROW_Y(0));	/* minus */
	host_tap(rows_x + rows_w - 60 - 200 - 30, ROW_Y(0));
	CHECK(sample.set[SET_BRIGHTNESS] == 65);
	host_tap(rows_x + 200, ROW_Y(0));	/* the row itself: the ring comes, nothing changes */
	CHECK(sample.set[SET_BRIGHTNESS] == 65 && st().focus == 0 && !st().adjusting);
	host_tap(rows_x + 200, ROW_Y(2));
	CHECK(sample.set[SET_EFFECTS] == 1 && st().focus == 2);
	host_tap(rows_x + 200, ROW_Y(2));	/* the row that has the ring: it acts, the ring stays */
	CHECK(sample.set[SET_EFFECTS] == 0 && st().focus == 2);
	/* the steering side changes; the list stays under the finger */
	host_tap(rows_x + 200, ROW_Y(1));
	CHECK(sample.set[SET_STEERING_RIGHT] == 1 && st().page == PAGE_DISPLAY);
	host_tap(rows_x + 200, ROW_Y(1));
	CHECK(sample.set[SET_STEERING_RIGHT] == 0);

	/* There is no Back button: a list's last row is "Back". */
	CHECK(st().list->n == 4 && st().list->rows[3].back && same(row(3)->label, "Back"));
	host_tap(rows_x + 200, ROW_Y(3));
	CHECK(st().page == PAGE_SETTINGS && st().focus == 1 && sounded(SND_BACK) == 1);
	host_tap(rows_x + 300, ROW_Y(3));
	host_run(100);
	host_tap(rows_x + 300, ROW_Y(0));
	host_run(100);
	CHECK(st().page == PAGE_INFO);
	sample.can_update = 0;
	ui_nav_refresh();
	host_tap(rows_x + 300, ROW_Y(3));	/* "Software update": disabled */
	CHECK(sounded(SND_DENY) == 1 && st().page == PAGE_INFO && st().focus == 0);
	host_tap(rows_x + 300, ROW_Y(4));	/* "Back", three times, to Home */
	CHECK(st().page == PAGE_SYSTEM);
	host_tap(rows_x + 300, ROW_Y(2));
	CHECK(st().page == PAGE_SETTINGS && st().focus == 3);
	host_tap(rows_x + 300, ROW_Y(4));
	CHECK(st().page == PAGE_HOME && st().focus == UI_FOCUS_COG);
	host_tap(60, T_BAND / 2);	/* on the title, where a Back might be: Home has none */
	CHECK(told(UI_EV_LEAVE) == 0 && st().page == PAGE_HOME);
	/* the cog is Home's: nothing of it on a list */
	host_tap(cog_x, cog_y);
	CHECK(st().page == PAGE_SETTINGS);
	host_tap(cog_x, cog_y);		/* row 6 would be here: there is none */
	CHECK(st().page == PAGE_SETTINGS);
#undef ROW_Y
}

/* What the application cannot do yet: shown, switched off. */
static void rows_off(void)
{
	start(SAMPLE_FIVE, 0);
	sample.name_off = 1;
	sample.set_off[SET_CONNECT_AT_IGNITION] = 1;
	sample.profile_off = sample.forget_off = 1;
	sample.set_off[SET_VOLUME_MEDIA] = sample.set_off[SET_VOLUME_CALL] = sample.set_off[SET_VOLUME_NAVIGATION] = 1;
	sample.set_off[SET_SOUNDS] = 1;
	sample.set_off[SET_BASS] = sample.set_off[SET_MID] = sample.set_off[SET_TREBLE] = 1;
	keys("lp" "pw");
	CHECK(st().page == PAGE_GENERAL && row(0)->disabled && row(1)->disabled && !row(2)->disabled);
	CHECK(st().focus == 2);	/* the knob starts on the first row that works */
	keys("l");
	CHECK(st().focus == 2);
	keys("p");
	CHECK(sample.set[SET_OFFER_OTHER_PHONE] == 0 && sample.set[SET_CONNECT_AT_IGNITION] == 1);
	host_tap(T_SPACE_5 + 300, T_BAND + T_BAND / 2);	/* "Name": denied */
	CHECK(st().page == PAGE_GENERAL && sounded(SND_DENY) == 1);
	keys("r" "pw" "pw");
	CHECK(st().page == PAGE_PHONE && row(2)->disabled && row(3)->disabled && st().focus == 1);
	keys("r");
	CHECK(st().focus == 4);	/* the two below "Connect" cannot be reached: "Back" is next */
	keys("bbb" "rr" "pw");
	CHECK(st().page == PAGE_SYSTEM);	/* every row of Sound is off: the knob passes it by */
	keys("b" "l");
	CHECK(st().page == PAGE_SETTINGS && st().focus == 1);
}

/* The instruction page: touch-led, but the knob works. */
static void update_page(void)
{
	const int cancel_x = 56 + 100, start_x = 56 + 200 + T_SPACE_4 + 100;
	const int buttons_y = T_SCREEN_H - T_SPACE_5 - 32;

	start(SAMPLE_FIVE, 0);
	keys("lp" "rrr" "pw" "pw" "rrr");
	CHECK(st().page == PAGE_INFO && st().focus == 3 && !row(3)->disabled && row(3)->chevron);
	keys("pw");
	CHECK(st().page == PAGE_UPDATE && st().focus == 0);	/* the ring starts on "Cancel" */
	int bumps = sounded(SND_BUMP);

	keys("l");
	CHECK(st().focus == 0 && sounded(SND_BUMP) == bumps + 1);
	keys("r");
	CHECK(st().focus == 1);
	keys("r");
	CHECK(st().focus == 1 && sounded(SND_BUMP) == bumps + 2);
	keys("l" "p");	/* "Cancel" */
	CHECK(st().page == PAGE_INFO && st().focus == 3 && told(UI_EV_UPDATE) == 0);
	keys("pw" "b");	/* Back is the same as "Cancel" */
	CHECK(st().page == PAGE_INFO && told(UI_EV_UPDATE) == 0);
	keys("pw" "r" "p");	/* "Start" */
	CHECK(told(UI_EV_UPDATE) == 1 && st().page == PAGE_UPDATE);
	ui_nav_wait(WAIT_UPDATING, "");	/* what the application does next */
	keys("b");
	CHECK(st().page == PAGE_WAIT && told(UI_EV_CANCEL) == 0);	/* Back does nothing there */

	/* by touch */
	start(SAMPLE_FIVE, 0);
	keys("lp" "rrr" "pw" "pw" "rrr" "pw");
	host_tap(cancel_x, buttons_y);
	CHECK(st().page == PAGE_INFO && told(UI_EV_UPDATE) == 0);
	keys("pw");
	host_tap(start_x, buttons_y);
	CHECK(told(UI_EV_UPDATE) == 1 && st().focus == 1);
	host_tap(start_x + 250, buttons_y);	/* beside the buttons: nothing */
	CHECK(told(UI_EV_UPDATE) == 1);

	/* "Start": the application shows the receiving screen. Step 1 has
	 * "Cancel" and "Next", the ring on "Next"; a phone that has joined, or
	 * "Next", brings step 2 with "Cancel" alone; its line changes alone */
	ui_nav_receive("Unit", "pass;word", "192.168.43.1", "Waiting for the update file");
	host_run(0);
	CHECK(st().page == PAGE_RECEIVE && ui_receive_shown() == 1 && ui_nav_receive_step() == 1 && st().focus == 1);
	keys("r");	/* nowhere further */
	CHECK(sounded(SND_BUMP) >= 1 && st().focus == 1);
	keys("l");
	CHECK(st().focus == 0 && ui_nav_receive_step() == 1);
	keys("r" "p");	/* "Next" */
	CHECK(ui_nav_receive_step() == 2 && ui_receive_shown() == 2 && told(UI_EV_CANCEL) == 0);
	ui_nav_receive("", "", "", "Receiving the file: 43%");
	ui_nav_refresh();	/* other news (the clock): the screen stays as it is */
	host_run(0);
	CHECK(st().page == PAGE_RECEIVE && ui_receive_shown() == 2);
	keys("r");
	CHECK(st().page == PAGE_RECEIVE && told(UI_EV_CANCEL) == 0);
	keys("b");
	CHECK(told(UI_EV_CANCEL) == 1 && st().page == PAGE_INFO && !ui_receive_shown());
	keys("pw" "r" "p");	/* again: the application sees a phone join; "Cancel" by a push */
	ui_nav_receive("Unit", "pass;word", "192.168.43.1", "Waiting for the update file");
	host_run(0);
	ui_nav_receive_next();
	host_run(0);
	CHECK(ui_nav_receive_step() == 2 && ui_receive_shown() == 2);
	keys("p");
	CHECK(told(UI_EV_CANCEL) == 2 && st().page == PAGE_INFO);
	keys("pw" "r" "p");	/* and "Cancel" on step 1 */
	ui_nav_receive("Unit", "pass;word", "192.168.43.1", "Waiting for the update file");
	host_run(0);
	keys("l" "p");
	CHECK(told(UI_EV_CANCEL) == 3 && st().page == PAGE_INFO && ui_nav_receive_step() == 0);
	keys("pw");	/* the instruction page again, for what follows */

	/* the car starts to move while the page is open: it closes, and the row is disabled */
	sample.can_update = 0;
	ui_nav_refresh();
	CHECK(st().page == PAGE_INFO && row(3)->disabled && st().focus == 4);	/* the ring leaves the dead row for the next live one */
	sample.can_update = 1;
	ui_nav_refresh();
	CHECK(!row(3)->disabled);
}

/* Over CarPlay: the layer is clear except for the switch banner. */
static void banner(void)
{
	const int all = T_SCREEN_W * T_SCREEN_H, card_y = 72;	/* the card: 1000 x 112 at 140, 16 */

	start(SAMPLE_FIVE, 0);
	CHECK(host_drawn_pixels() == all && ui_nav_owns(640, 300));	/* a launcher screen has every pixel and touch */
	CHECK(ui_nav_banner() == 0 && !ui_banner_shown());	/* the banner is for CarPlay only */
	ui_nav_hide();
	CHECK(st().page == PAGE_HIDDEN && host_drawn_pixels() == 0 && !ui_nav_owns(640, 60));
	keys("rpb");
	CHECK(sample_logged == 0);	/* the knob is not ours while nothing of ours is up */

	CHECK(ui_nav_banner() == 1 && ui_banner_shown());
	int drawn = host_drawn_pixels();

	CHECK(drawn <= 1000 * 112 - 2500 && drawn > 1000 * 112 - 3000);	/* the card and nothing else; its ends are half-circles */
	CHECK(ui_nav_owns(640, 60) && ui_nav_owns(140, 16) && ui_nav_owns(117, 150));	/* on it and 24px around it */
	CHECK(!ui_nav_owns(115, 60) && !ui_nav_owns(640, 153) && !ui_nav_owns(640, 300));
	keys("rp");
	CHECK(ui_banner_shown() && sample_logged == 0);	/* it never takes the knob */
	host_tap(640, 300);	/* CarPlay's touch */
	CHECK(ui_banner_shown() && told(UI_EV_SWITCH) == 0);
	host_tap(960, card_y);	/* on its button */
	CHECK(told(UI_EV_SWITCH) == 1 && last(UI_EV_SWITCH).arg == 1);	/* the phone used before this one */
	CHECK(!ui_banner_shown() && host_drawn_pixels() == 0);
	ui_nav_banner();
	host_tap(300, card_y);	/* anywhere on the banner: the whole of it is the switch */
	CHECK(told(UI_EV_SWITCH) == 2);
	ui_nav_banner();
	host_tap(640, 140);	/* a near miss, just under it: it goes, nothing else */
	CHECK(!ui_banner_shown() && told(UI_EV_SWITCH) == 2 && host_drawn_pixels() == 0);

	ui_nav_banner();	/* 8 seconds, then it goes without a trace */
	host_run(7800);
	CHECK(ui_banner_shown());
	host_run(400);
	CHECK(!ui_banner_shown() && host_drawn_pixels() == 0 && !ui_nav_owns(640, 60));

	sample.set[SET_OFFER_OTHER_PHONE] = 0;	/* "Offer other phone in CarPlay": Off */
	CHECK(ui_nav_banner() == 0);
	sample.set[SET_OFFER_OTHER_PHONE] = 1;
	sample.guest = 1;	/* never under the Guest profile */
	CHECK(ui_nav_banner() == 0);
	sample.guest = 0;
	sample.phones[0].state = PHONE_PAIRED;	/* nobody "last used": the first other phone */
	sample.phones[1].state = PHONE_PAIRED;
	sample.phones[2].state = PHONE_CONNECTED;
	CHECK(ui_nav_banner() == 1);
	host_tap(640, card_y);
	CHECK(last(UI_EV_SWITCH).arg == 0);
	sample.phones[2].state = PHONE_PAIRED;	/* nobody in CarPlay */
	CHECK(ui_nav_banner() == 0);
	sample.phones[0].state = PHONE_CONNECTED;	/* no other phone to offer */
	sample.n = 1;
	CHECK(ui_nav_banner() == 0);

	start(SAMPLE_FIVE, 0);	/* a launcher screen takes its place */
	ui_nav_hide();
	ui_nav_banner();
	ui_nav_home(UI_FOCUS_DEFAULT);
	CHECK(!ui_banner_shown() && st().page == PAGE_HOME && host_drawn_pixels() == all && ui_nav_owns(640, 300));
	ui_nav_wait(WAIT_CONNECTING, "Work iPhone");
	ui_nav_cover();		/* a hand-off between pictures: plain ground, nothing behind it */
	CHECK(st().page == PAGE_HIDDEN && host_drawn_pixels() == all && ui_nav_owns(1, 1) && ui_nav_banner() == 0);
	ui_nav_hide();
	CHECK(host_drawn_pixels() == 0);

	sample.set[SET_EFFECTS] = 1;	/* Full: it fades over 200 ms */
	ui_nav_init(&sample, sample_event);
	CHECK(ui_nav_banner() == 1);
	host_run(8100);
	CHECK(ui_banner_shown());
	host_run(300);
	CHECK(!ui_banner_shown() && host_drawn_pixels() == 0);
}

/* ---- text entry ---- */

static struct ui_text_state tx(void)
{
	struct ui_text_state s;

	ui_text_state(&s);
	return s;
}

static void name_by_knob(void)	/* Settings, General, "Name" with a push: the dial */
{
	start(SAMPLE_FIVE, 0);
	keys("lp" "pw" "pw");
}

static void text_dial(void)
{
	name_by_knob();
	CHECK(st().page == PAGE_TEXT && tx().dial && same(tx().value, "BMW 28114"));
	CHECK(tx().items == 31 && tx().item == 0 && same(tx().caption, "A"));	/* 26 letters and five functions */
	keys("p");
	CHECK(same(tx().value, "BMW 28114A") && sounded(SND_KEY) == 1);
	keys("l");	/* the dial is a circle: the one place where focus wraps */
	CHECK(tx().item == 30 && same(tx().caption, "Done"));
	keys("l");
	CHECK(same(tx().caption, "Space"));
	keys("p");
	CHECK(same(tx().value, "BMW 28114A "));
	keys("l");
	CHECK(same(tx().caption, "Delete"));
	keys("pp");
	CHECK(same(tx().value, "BMW 28114"));
	keys("l");
	CHECK(same(tx().caption, "Small letters"));
	keys("p");
	CHECK(tx().item == 0 && same(tx().caption, "a") && tx().items == 31);
	keys("rp");
	CHECK(same(tx().value, "BMW 28114b"));
	keys("llllll");
	CHECK(same(tx().caption, "Numbers"));
	keys("p");
	CHECK(tx().items == 23 && same(tx().caption, "0"));	/* 19 digits and marks, four functions */
	keys("rrp");
	CHECK(same(tx().value, "BMW 28114b2"));
	keys("llllll");
	CHECK(same(tx().caption, "Capitals"));
	keys("p");
	CHECK(tx().items == 31 && same(tx().caption, "A"));
	keys("l");
	CHECK(tx().item == 30);
	keys("r");
	CHECK(tx().item == 0);
	/* "OK" saves and returns to the list */
	keys("lp");
	CHECK(told(UI_EV_NAME) == 1 && same(ui_nav_entered(), "BMW 28114b2"));
	CHECK(st().page == PAGE_GENERAL && st().focus == 0 && same(row(0)->value, "BMW 28114b2"));
}

static void text_limits(void)
{
	name_by_knob();
	keys("lll");	/* "Delete" */
	keys("ppppppppp");
	CHECK(same(tx().value, ""));
	int bumps = sounded(SND_BUMP);

	keys("p");	/* deleting from an empty field bumps */
	CHECK(same(tx().value, "") && sounded(SND_BUMP) == bumps + 1);
	keys("rrp");	/* "OK" on an empty field saves nothing */
	CHECK(st().page == PAGE_GENERAL && told(UI_EV_NAME) == 0 && same(row(0)->value, "BMW 28114"));

	keys("pw");
	CHECK(st().page == PAGE_TEXT && same(tx().value, "BMW 28114"));	/* a fresh start each time */
	keys("ppppppppppppppp");	/* 9 + 15 = 24 characters: full */
	CHECK(same(tx().value, "BMW 28114AAAAAAAAAAAAAAA") && sounded(SND_DENY) == 0);
	keys("p");	/* typing into a full field denies */
	CHECK(same(tx().value, "BMW 28114AAAAAAAAAAAAAAA") && sounded(SND_DENY) == 1);
	keys("b");	/* Back leaves without saving */
	CHECK(st().page == PAGE_GENERAL && told(UI_EV_NAME) == 0 && same(row(0)->value, "BMW 28114"));
	CHECK(same(sample.name, "BMW 28114"));
}

/* The keyboard: its rows are centred on the screen. */
#define KEY_Y(r) (2 * T_BAND + (r) * (T_BAND + T_SPACE_2) + T_BAND / 2)
#define KEYS_X 89	/* the rows are centred on the screen */
#define CANCEL_X (T_SCREEN_W - T_SPACE_5 - UI_BAR_STATUS - T_SPACE_4 - 66)	/* "Cancel": 132 wide, before the status */
#define KEY_X(i) (KEYS_X + (i) * 111 + 51)	/* the top row of ten */
#define KEY_SHIFT_X (KEYS_X + 79)
#define KEY_ROW3_X(i) (KEYS_X + 166 + (i) * 111 + 51)
#define KEY_DELETE_X (KEYS_X + 166 + 7 * 111 + 79)
#define KEY_LAYER_X (KEYS_X + 107)
#define KEY_DONE_X (KEYS_X + 214 + 8 + 658 + 8 + 107)

static void text_swap(void)
{
	name_by_knob();
	keys("rp");
	CHECK(same(tx().value, "BMW 28114B") && tx().item == 1);
	/* a touch while the dial is shown: the keyboard, the same text, nothing typed */
	host_tap(T_SCREEN_W / 2, T_BAND + 210);
	CHECK(st().page == PAGE_TEXT && !tx().dial && same(tx().value, "BMW 28114B"));
	host_tap(KEY_X(0), KEY_Y(0));
	CHECK(same(tx().value, "BMW 28114Bq"));
	host_tap(KEY_SHIFT_X, KEY_Y(2));
	CHECK(tx().shift && same(tx().value, "BMW 28114Bq"));
	host_tap(KEY_X(1), KEY_Y(0));
	CHECK(same(tx().value, "BMW 28114BqW"));
	host_tap(KEY_ROW3_X(0), KEY_Y(2));
	CHECK(same(tx().value, "BMW 28114BqWZ"));
	host_tap(KEY_LAYER_X, KEY_Y(3));
	CHECK(tx().digits);
	host_tap(KEY_X(0), KEY_Y(0));
	CHECK(same(tx().value, "BMW 28114BqWZ1"));
	host_tap(KEY_SHIFT_X, KEY_Y(2));	/* no capitals among the digits: an empty key */
	CHECK(same(tx().value, "BMW 28114BqWZ1") && tx().digits);
	host_tap(KEY_DELETE_X, KEY_Y(2));
	host_tap(KEY_DELETE_X, KEY_Y(2));
	CHECK(same(tx().value, "BMW 28114BqW"));
	host_tap(T_SCREEN_W / 2, KEY_Y(3));	/* space */
	CHECK(same(tx().value, "BMW 28114BqW "));
	/* the knob while the keyboard is shown: the dial returns, nothing typed */
	int ticks = sounded(SND_TICK), typed = sounded(SND_KEY);

	keys("r");
	CHECK(tx().dial && tx().item == 1 && same(tx().value, "BMW 28114BqW ") && sounded(SND_TICK) == ticks);
	host_tap(T_SCREEN_W / 2, T_BAND + 210);
	keys("p");
	CHECK(tx().dial && same(tx().value, "BMW 28114BqW ") && sounded(SND_KEY) == typed);
	/* "Done" on the keyboard saves */
	host_tap(T_SCREEN_W / 2, T_BAND + 210);
	host_tap(KEY_DONE_X, KEY_Y(3));
	CHECK(st().page == PAGE_GENERAL && same(ui_nav_entered(), "BMW 28114BqW ") && same(sample.name, "BMW 28114BqW "));

	/* opened with a tap: the keyboard comes first; "Cancel" beside the field leaves */
	host_tap(T_SPACE_5 + 300, T_BAND + T_BAND / 2);
	host_run(100);
	CHECK(st().page == PAGE_TEXT && !tx().dial && same(tx().value, "BMW 28114BqW "));
	host_tap(KEY_X(2), KEY_Y(0));
	CHECK(same(tx().value, "BMW 28114BqW e"));
	host_tap(CANCEL_X, T_BAND / 2);
	CHECK(st().page == PAGE_GENERAL && told(UI_EV_NAME) == 1 && same(row(0)->value, "BMW 28114BqW "));
	/* news from the application while typing does not disturb the text */
	keys("pw" "p");
	sample.clock = "14:33";
	ui_nav_refresh();
	CHECK(st().page == PAGE_TEXT && same(tx().value, "BMW 28114BqW A"));
	/* "Cancel" with the dial too, saving nothing */
	host_tap(CANCEL_X, T_BAND / 2);
	CHECK(st().page == PAGE_GENERAL && told(UI_EV_NAME) == 1);
}

int main(void)
{
	if (host_init()) {
		printf("could not start (fonts?)\n");
		return 1;
	}
	home_left();
	home_right();
	home_start();
	lists();
	pushes();
	amounts();
	phones();
	scrolling();
	system_pages();
	waiting();
	sound_levels();
	taps();
	rows_off();
	update_page();
	banner();
	text_dial();
	text_limits();
	text_swap();
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
