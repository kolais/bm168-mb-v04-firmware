/* SPDX-License-Identifier: MIT
 * The launcher's navigation: see nav.h, design/screens.md and
 * design/feedback.md.
 */
#include "nav.h"
#include "display.h"
#include <stdio.h>
#include <string.h>

/* What a push (or a tap) on a row does. */
enum act {
	ACT_INFO,	/* nothing to open: bumps */
	ACT_OPEN,	/* arg = the page it opens */
	ACT_CHOICE,	/* arg = the setting: step to its next value */
	ACT_AMOUNT,	/* arg = the setting: push to adjust, turn, push to keep */
	ACT_BACK,	/* "Cancel" */
	ACT_CONNECT, ACT_DISCONNECT, ACT_PROFILE /* arg = profile */, ACT_FORGET, ACT_REBOOT,
	ACT_NAME,	/* opens text entry for the unit's name */
};

static const char *const off_on[] = { "Off", "On", NULL };
static const char *const left_right[] = { "Left", "Right", NULL };
static const char *const sound_levels[] = { "Off", "Actions", "All", NULL };

static const struct {
	const char *label;
	const char *const *words;	/* a choice */
	int min, max, step;		/* an amount */
	const char *unit;
} settings[SET_COUNT] = {
	[SET_CONNECT_AT_IGNITION] = { "Connect at ignition", off_on, 0, 0, 0, NULL },
	[SET_OFFER_OTHER_PHONE] = { "Offer other phone in CarPlay", off_on, 0, 0, 0, NULL },
	[SET_BRIGHTNESS] = { "Brightness", NULL, 10, 100, 5, "%" },
	[SET_STEERING_RIGHT] = { "Steering side", left_right, 0, 0, 0, NULL },
	[SET_EFFECTS] = { "Visual effects", off_on, 0, 0, 0, NULL },
	[SET_VOLUME_MEDIA] = { "Media volume", NULL, 0, 100, 5, "%" },
	[SET_VOLUME_CALL] = { "Call volume", NULL, 0, 100, 5, "%" },
	[SET_VOLUME_NAVIGATION] = { "Navigation volume", NULL, 0, 100, 5, "%" },
	[SET_SOUNDS] = { "Launcher sounds", sound_levels, 0, 0, 0, NULL },
	[SET_BASS] = { "Bass", NULL, -6, 6, 1, " dB" },
	[SET_MID] = { "Mid", NULL, -6, 6, 1, " dB" },
	[SET_TREBLE] = { "Treble", NULL, -6, 6, 1, " dB" },
	[SET_DEVELOPER] = { "Developer access", off_on, 0, 0, 0, NULL },
};

static struct ui_model *m;
static ui_event_cb notify;

/* The way here: Home (or the waiting screen) at the bottom, then the lists
 * opened one from another. Each remembers where the knob was. */
static struct level {
	enum ui_page page;
	int param;		/* the phone, on a phone's pages */
	int focus, top;
} stack[10];
static int depth;		/* index of the level that is shown */
static int adjusting;
static enum ui_wait waiting;
static char waiting_for[128];	/* the name on the waiting screen */
static unsigned shows;		/* counts ui_nav_home/ui_nav_wait calls: did the callback change the screen? */

/* The list that is shown. */
static struct ui_row rows[UI_LIST_MAX];
static struct { enum act act; int arg; } acts[UI_LIST_MAX];
static char texts[UI_LIST_MAX][32], title[80];
static struct ui_list list;

/* An "open" waits for the ring's answer to show (design/feedback.md); any
 * input in between acts on the arriving screen at once. */
static lv_timer_t *opening;
static enum ui_page opening_page;
static int opening_param;
static int opening_by_touch;	/* text entry: the keyboard first */
static char entered[128];	/* the text of UI_EV_NAME */

/* A phone that has just been forgotten leaves the list before the driver's
 * eyes: the model no longer has it, but its row is shown once more, dimmed,
 * and closes (ui_list_collapse). Any input settles the list at once. */
static int leaving = -1;	/* that row's place in the list of phones, -1: none */
static int leaving_n;		/* the phones the model has without it */
static int leaving_shown;	/* its row is on the screen and closing */
static char leaving_label[64];
static const char *leaving_value;
static char message[120];	/* the last ui_nav_message, until the next input */
#define UNIT_NAME_CHARS 24		/* characters */

static void show(void);
static void settle(void);

/* Deeper and back, in Full: the whole screen is pushed out by the next
 * (owner, 2026-11-23). Lite, and a unit that cannot, cut. */
static void arrive(enum ui_change how)
{
	if (theme_mode() == UI_FULL)
		ui_display_change(how);
}

/* ... and once it is built it is drawn at once, not at the display's next
 * turn: the wait before the move is what the driver feels. */
static void arrived(void)
{
	if (theme_mode() == UI_FULL)
		ui_display_draw_now();
}

#define cur (&stack[depth])

/* A list is shown (not Home, the waiting screen, text entry or a page). */
static int on_list(void)
{
	return cur->page != PAGE_HOME && cur->page != PAGE_WAIT && cur->page != PAGE_TEXT && cur->page != PAGE_UPDATE &&
	       cur->page != PAGE_RECEIVE &&
	       cur->page != PAGE_HIDDEN;
}

/* ---- telling the application ---- */

static void sound(enum ui_sound s)
{
	int level = m->set[SET_SOUNDS];

	if (level == 0 || (level == 1 && s == SND_TICK))
		return;
	notify(UI_EV_SOUND, s, 0);
}

/* The callback may change the model or show another screen. If it did not
 * show one, redraw this one from the model. */
static void tell(enum ui_event ev, int arg, int value)
{
	unsigned before = shows;

	notify(ev, arg, value);
	if (shows == before)
		show();
}

/* ---- building a list from the model ---- */

static const char *state_word(const struct ui_phone *p)
{
	return p->state == PHONE_CONNECTED ? "Connected" : p->state == PHONE_UNREACHABLE ? "Unreachable" :
	       p->state == PHONE_LAST_USED ? "Last used" : "Paired";
}

static struct ui_row *add(const char *label, enum act act, int arg)
{
	struct ui_row *r = &rows[list.n];

	memset(r, 0, sizeof(*r));
	r->label = label;
	acts[list.n].act = act;
	acts[list.n].arg = arg;
	list.n++;
	return r;
}

static void add_open(const char *label, enum ui_page page)
{
	add(label, ACT_OPEN, page)->chevron = 1;
}

static void add_setting(enum ui_setting s)
{
	struct ui_row *r;

	if (settings[s].words) {
		r = add(settings[s].label, ACT_CHOICE, s);
		r->value = settings[s].words[m->set[s]];
	} else {
		r = add(settings[s].label, ACT_AMOUNT, s);
		r->amount = 1;
		r->now = m->set[s];
		r->min = settings[s].min;
		r->max = settings[s].max;
		r->unit = settings[s].unit;
	}
	r->disabled = m->set_off[s];
}

static void build(void)
{
	const struct ui_phone *p = cur->param < m->n ? &m->phones[cur->param] : NULL;

	list.n = 0;
	list.rows = rows;
	list.title = title;
	title[0] = 0;
	switch (cur->page) {
	case PAGE_SETTINGS:
		list.title = "Settings";
		add_open("General", PAGE_GENERAL);
		add_open("Display", PAGE_DISPLAY);
		add_open("Sound", PAGE_SOUND);
		/* a group with nothing in it that works is off itself */
		rows[list.n - 1].disabled = m->set_off[SET_VOLUME_MEDIA] && m->set_off[SET_VOLUME_CALL] &&
					    m->set_off[SET_VOLUME_NAVIGATION] && m->set_off[SET_SOUNDS] &&
					    m->set_off[SET_BASS] && m->set_off[SET_MID] && m->set_off[SET_TREBLE];
		add_open("System", PAGE_SYSTEM);
		break;
	case PAGE_GENERAL: {
		list.title = "General";
		/* One name, for Bluetooth and Wi-Fi. */
		struct ui_row *name = add("Name", ACT_NAME, 0);

		name->value = m->name;
		name->disabled = m->name_off;
		add_setting(SET_CONNECT_AT_IGNITION);
		add_setting(SET_OFFER_OTHER_PHONE);
		add_open("Paired phones", PAGE_PHONES);
		break;
	}
	case PAGE_PHONES:
		list.title = "Paired phones";
		for (int i = 0; i < m->n && list.n < UI_LIST_MAX; i++) {
			struct ui_row *r = add(m->phones[i].name, ACT_OPEN, PAGE_PHONE);

			r->value = state_word(&m->phones[i]);
			r->chevron = 1;
		}
		if (!m->n)
			add("No phones paired", ACT_INFO, 0);
		break;
	case PAGE_PHONE:
		snprintf(title, sizeof(title), "%s", p->name);
		add("Status", ACT_INFO, 0)->value = state_word(p);
		if (p->state == PHONE_CONNECTED)
			add("Disconnect", ACT_DISCONNECT, 0);
		else
			add("Connect", ACT_CONNECT, 0);
		add_open("Driver profile", PAGE_PROFILE);
		if (p->profile)
			snprintf(texts[list.n - 1], sizeof(texts[0]), "Profile %d", p->profile);
		rows[list.n - 1].value = p->profile ? texts[list.n - 1] : "None";
		rows[list.n - 1].disabled = m->profile_off;
		add_open("Forget this phone", PAGE_FORGET);
		rows[list.n - 1].disabled = m->forget_off;
		break;
	case PAGE_PROFILE:
		list.title = "Driver profile";
		add("None", ACT_PROFILE, 0)->checked = p->profile == 0;
		for (int i = 1; i <= 3; i++) {
			snprintf(texts[list.n], sizeof(texts[0]), "Profile %d", i);
			add(texts[list.n], ACT_PROFILE, i)->checked = p->profile == i;
		}
		break;
	case PAGE_FORGET:
		/* The title names the action and what it affects; "Cancel" comes first. */
		snprintf(title, sizeof(title), "Forget %s", p->name);
		add("Cancel", ACT_BACK, 0);
		add("Forget now", ACT_FORGET, 0);
		break;
	case PAGE_DISPLAY:
		list.title = "Display";
		add_setting(SET_BRIGHTNESS);
		add_setting(SET_STEERING_RIGHT);
		add_setting(SET_EFFECTS);
		break;
	case PAGE_SOUND:
		list.title = "Sound";
		add_setting(SET_VOLUME_MEDIA);
		add_setting(SET_VOLUME_CALL);
		add_setting(SET_VOLUME_NAVIGATION);
		add_setting(SET_SOUNDS);
		add_open("Equaliser", PAGE_EQUALISER);
		rows[list.n - 1].value = m->set[SET_BASS] || m->set[SET_MID] || m->set[SET_TREBLE] ? "Custom" : "Flat";
		rows[list.n - 1].disabled = m->set_off[SET_BASS] && m->set_off[SET_MID] && m->set_off[SET_TREBLE];
		break;
	case PAGE_EQUALISER:
		list.title = "Equaliser";
		add_setting(SET_BASS);
		add_setting(SET_MID);
		add_setting(SET_TREBLE);
		break;
	case PAGE_SYSTEM:
		list.title = "System";
		add_open("System info", PAGE_INFO);
		if (m->can_develop)	/* otherwise there is nothing to switch on: no row */
			add_setting(SET_DEVELOPER);
		add_open("Reboot", PAGE_REBOOT);
		break;
	case PAGE_INFO:
		list.title = "System info";
		add("Kernel", ACT_INFO, 0)->value = m->kernel;
		add("Processor", ACT_INFO, 0)->value = m->processor;
		add("Panel", ACT_INFO, 0)->value = m->panel;
		/* Done while parked: the application says when it may be started. */
		add_open("Software update", PAGE_UPDATE);
		rows[list.n - 1].disabled = !m->can_update;
		break;
	case PAGE_REBOOT:
		list.title = "Reboot";
		add("Cancel", ACT_BACK, 0);
		add("Reboot now", ACT_REBOOT, 0);
		break;
	case PAGE_HOME:
	case PAGE_WAIT:
	case PAGE_TEXT:
	case PAGE_UPDATE:
	case PAGE_RECEIVE:
	case PAGE_HIDDEN:
		break;
	}
	/* The way up by touch is the list's last row (owner, 2026-11-22: no
	 * Back button on the screen). A confirmation has "Cancel" for it. */
	if (cur->page != PAGE_FORGET && cur->page != PAGE_REBOOT && list.n < UI_LIST_MAX)
		add("Back", ACT_BACK, 0)->back = 1;
}

/* The software update's instruction page. The words are the design's; the
 * steps follow the update by USB stick and are settled with it. */
static const char *const update_steps[] = {
	"Have the update file on your phone.",
	"Press Start: the unit shows how to join its Wi-Fi and the address to send the file to.",
	"Keep the ignition on: the unit restarts when the update is in.",
};

/* The receiving screen's words (ui_nav_receive). */
static char rx_ssid[40], rx_psk[72], rx_addr[32], rx_status[96];
static int rx_step;	/* 1: join the Wi-Fi; 2: open the address */
static void tap_receive(int button);

/* ---- showing ---- */

static void bar(void)
{
	struct ui_rail r = {
		.clock = m->clock, .bt = m->bt, .rhd = m->set[SET_STEERING_RIGHT],
		.cog_shown = depth == 0 && cur->page == PAGE_HOME,
		.cog_focus = depth == 0 && cur->page == PAGE_HOME && cur->focus == UI_FOCUS_COG,
	};

	ui_frame_rail(&r);
}

static void tap_tile(int tile);
static void tap_cancel(void);
static void tap_row(int row, int part);
static void tap_button(int button);
static void text_done(const char *value);

static void keep_in_view(void)
{
	if (cur->focus < cur->top)
		cur->top = cur->focus;
	if (cur->focus > cur->top + UI_LIST_ROWS - 1)
		cur->top = cur->focus - (UI_LIST_ROWS - 1);
	if (cur->top > list.n - UI_LIST_ROWS)
		cur->top = list.n > UI_LIST_ROWS ? list.n - UI_LIST_ROWS : 0;
	list.focus = cur->focus;
	list.top = cur->top;
	list.adjusting = adjusting;
}

static void show(void)
{
	if (cur->page == PAGE_HIDDEN)	/* no launcher screen is up: nothing to draw */
		return;
	/* A phone's pages end when the phone is gone. */
	while (depth > 0 && (cur->page == PAGE_PHONE || cur->page == PAGE_PROFILE || cur->page == PAGE_FORGET) &&
	       cur->param >= m->n) {
		depth--;
		adjusting = 0;
	}
	/* The update's page ends when it may no longer be started (the car moves). */
	if (depth > 0 && cur->page == PAGE_UPDATE && !m->can_update)
		depth--;
	bar();
	if (cur->page == PAGE_UPDATE) {
		struct ui_steps s = { "Software update", update_steps, 3, "Cancel", "Start", cur->focus };

		ui_steps_show(&s, tap_button);
	} else if (cur->page == PAGE_RECEIVE) {
		/* Drawn once a step: a redraw for other news (the clock) leaves the code as it is. */
		if (ui_receive_shown() != rx_step) {
			struct ui_receive r = { rx_step, rx_ssid, rx_psk, rx_addr, rx_status, cur->focus };

			ui_receive_show(&r, tap_receive);
		}
	} else if (cur->page == PAGE_HOME) {
		struct ui_home h = { m->phones, m->n, m->chip, m->chip_text, cur->focus };

		if (cur->focus > m->n)
			h.focus = cur->focus = m->n;
		ui_home_show(&h, tap_tile);
	} else if (cur->page == PAGE_WAIT) {
		ui_wait_show(waiting, waiting_for, tap_cancel);
	} else if (cur->page == PAGE_TEXT) {
		/* What is being typed is the screen's own: a redraw for other
		 * news (the clock) must not touch it. */
		if (!ui_text_shown())
			ui_text_open("Name", m->name, UNIT_NAME_CHARS, opening_by_touch, text_done, sound);
	} else {
		int closes = -1;

		build();
		if (cur->focus >= list.n)
			cur->focus = list.n - 1;
		/* The knob never rests on a disabled row. */
		for (int i = cur->focus; i < list.n && rows[cur->focus].disabled; i++)
			if (!rows[i].disabled)
				cur->focus = i;
		for (int i = cur->focus; i >= 0 && rows[cur->focus].disabled; i--)
			if (!rows[i].disabled)
				cur->focus = i;
		/* The forgotten phone's row, once more: where it stood, dimmed. Any
		 * other redraw (the clock, a phone's state) ends it. */
		if (leaving >= 0 && (leaving_shown || cur->page != PAGE_PHONES || m->n != leaving_n || list.n >= UI_LIST_MAX))
			leaving = -1;
		if (leaving >= 0) {
			memmove(&rows[leaving + 1], &rows[leaving], (list.n - leaving) * sizeof(rows[0]));
			memmove(&acts[leaving + 1], &acts[leaving], (list.n - leaving) * sizeof(acts[0]));
			memset(&rows[leaving], 0, sizeof(rows[0]));
			rows[leaving].label = leaving_label;
			rows[leaving].value = leaving_value;
			rows[leaving].chevron = 1;
			rows[leaving].disabled = 1;
			acts[leaving].act = ACT_INFO;
			list.n++;
			closes = leaving;
			leaving_shown = 1;
		}
		keep_in_view();
		if (closes >= 0)
			list.focus = closes;	/* the ring stays where the phone was */
		ui_list_show(&list, tap_row);
		if (closes >= 0)
			ui_list_collapse(&list, closes, settle);
	}
}

static void cancel_opening(void)
{
	if (opening) {
		lv_timer_delete(opening);
		opening = NULL;
	}
}

static void open_now(void)
{
	cancel_opening();
	if (depth + 1 >= (int)(sizeof(stack) / sizeof(stack[0])))
		return;
	int param = opening_page == PAGE_PHONE ? opening_param : cur->param;

	depth++;
	cur->page = opening_page;
	cur->param = param;
	cur->top = 0;
	/* The ring starts where one push does the likely or the safe thing:
	 * the current option; "Connect" or "Disconnect" on a phone's list,
	 * not its status; "Cancel"; otherwise the first row. */
	cur->focus = opening_page == PAGE_PROFILE && param < m->n ? m->phones[param].profile :
		     opening_page == PAGE_PHONE ? 1 : 0;
	arrive(UI_CHANGE_FORWARD);
	show();
	arrived();
}

static void open_due(lv_timer_t *t)
{
	(void)t;
	opening = NULL;
	open_now();
}

/* If an "open" is on its way, be on the arriving screen already. */
static void settle(void)
{
	message[0] = 0;
	if (opening)
		open_now();
	if (leaving >= 0) {	/* the row that was closing is gone now */
		leaving = -1;
		show();
	}
}

/* Text entry ends: saved (value) or left (NULL). Back to the list it came from. */
static void text_done(const char *value)
{
	depth--;
	if (!value) {
		show();
		return;
	}
	snprintf(entered, sizeof(entered), "%s", value);
	tell(UI_EV_NAME, 0, 0);
}

static void open_page(enum ui_page page, int param)
{
	opening_page = page;
	opening_param = param;
	opening = lv_timer_create(open_due, ui_ack_ms(), NULL);
	lv_timer_set_repeat_count(opening, 1);
}

/* By touch there is no answer of the ring to wait for: the finger has said
 * which row, and the next screen comes at once, as "Back" does (owner,
 * 2026-11-23: the ring travelling down and answering before the push "feels
 * a little weird for touch"). The screen that leaves is drawn first, with the
 * ring on the touched row. */
static void open_at_once(enum ui_page page, int param)
{
	opening_page = page;
	opening_param = param;
	ui_display_draw_now();
	open_now();
}

static void close_page(void)
{
	adjusting = 0;
	depth--;
	if (depth == 0)
		cur->focus = UI_FOCUS_COG;	/* out of Settings: the knob is back on the cog */
	arrive(UI_CHANGE_BACK);
	show();
	arrived();
}

/* ---- Home ---- */

static void home_push(void)
{
	if (cur->focus == UI_FOCUS_COG) {
		sound(SND_SELECT);
		opening_page = PAGE_SETTINGS;
		open_now();
		return;
	}
	ui_home_ack();
	sound(SND_SELECT);
	if (cur->focus == m->n)
		tell(UI_EV_PAIR, 0, 0);
	else
		tell(UI_EV_PHONE, cur->focus, 0);
}

/* The knob runs along the tiles and ends on the cog, which is next to the
 * end of the band it is at: before the first tile with the cog on the left,
 * after "Pair a phone" with it on the right. */
static void home_turn(int dir)
{
	int rhd = m->set[SET_STEERING_RIGHT], last = m->n, to;

	if (cur->focus == UI_FOCUS_COG) {
		if (dir == (rhd ? 1 : -1)) {
			ui_frame_cog_bump();
			sound(SND_BUMP);
			return;
		}
		to = rhd ? last : 0;
	} else if (cur->focus + dir < 0 || cur->focus + dir > last) {
		if (dir != (rhd ? 1 : -1)) {
			ui_home_bump(dir > 0);
			sound(SND_BUMP);
			return;
		}
		to = UI_FOCUS_COG;
	} else {
		to = cur->focus + dir;
	}
	cur->focus = to;
	bar();
	ui_home_focus(to);
	sound(SND_TICK);
}

static void tap_tile(int tile)
{
	settle();
	if (cur->page != PAGE_HOME)
		return;
	/* After a touch the knob continues from the touched element. */
	cur->focus = tile;
	bar();
	ui_home_focus(tile);
	home_push();
}

/* ---- lists ---- */

static void set_value(enum ui_setting s, int value)
{
	m->set[s] = value;
	if (s == SET_EFFECTS)
		theme_set_mode(value ? UI_FULL : UI_LITE);
	if (s == SET_STEERING_RIGHT || s == SET_EFFECTS) {
		/* Back and the cog change ends, or every fill changes: draw the screen
		 * again, then tell (the callback may redraw once more). */
		show();
		notify(UI_EV_SETTING, s, value);
		return;
	}
	build();
	keep_in_view();
	ui_list_row(&list, cur->focus);
	notify(UI_EV_SETTING, s, value);
}

static void amount_step(int dir)
{
	enum ui_setting s = acts[cur->focus].arg;
	int to = m->set[s] + dir * settings[s].step;

	if (to < settings[s].min || to > settings[s].max) {
		ui_list_bump(dir > 0);
		sound(SND_BUMP);
		return;
	}
	sound(SND_TICK);
	set_value(s, to);
}

static void list_turn(int dir)
{
	int to = cur->focus + dir;

	if (adjusting) {
		amount_step(dir);
		return;
	}
	while (to >= 0 && to < list.n && rows[to].disabled)	/* the knob skips a disabled row */
		to += dir;
	if (to < 0 || to >= list.n) {	/* no wrap-around at the ends */
		ui_list_bump(dir > 0);
		sound(SND_BUMP);
		return;
	}
	cur->focus = to;
	keep_in_view();
	ui_list_place(&list);
	sound(SND_TICK);
}

static void list_push(int by_touch)
{
	int row = cur->focus, arg = acts[row].arg, phone = cur->param;

	if (adjusting) {	/* keep the amount */
		adjusting = 0;
		keep_in_view();
		ui_list_place(&list);
		sound(SND_SELECT);
		return;
	}
	switch (acts[row].act) {
	case ACT_INFO:
		ui_list_bump(1);
		sound(SND_BUMP);
		break;
	case ACT_OPEN:
		sound(SND_SELECT);
		if (by_touch) {
			open_at_once(arg, row);
		} else {
			ui_list_ack(&list);
			open_page(arg, row);
		}
		break;
	case ACT_CHOICE: {
		int to = settings[arg].words[m->set[arg] + 1] ? m->set[arg] + 1 : 0;

		sound(to ? SND_ON : SND_OFF);
		set_value(arg, to);
		ui_list_ack(&list);	/* after the change: the screen may have been drawn again */
		break;
	}
	case ACT_AMOUNT:
		adjusting = 1;
		keep_in_view();
		ui_list_place(&list);
		sound(SND_SELECT);
		break;
	case ACT_BACK:
		sound(SND_BACK);
		close_page();
		break;
	case ACT_CONNECT:
		ui_list_ack(&list);
		sound(SND_SELECT);
		tell(UI_EV_PHONE, phone, 0);
		break;
	case ACT_DISCONNECT:
		ui_list_ack(&list);
		sound(SND_OFF);
		tell(UI_EV_DISCONNECT, phone, 0);
		break;
	case ACT_PROFILE:
		sound(SND_SELECT);
		depth--;	/* back to the phone's list, which shows the new value */
		arrive(UI_CHANGE_BACK);
		tell(UI_EV_PROFILE, phone, arg);
		arrived();
		break;
	case ACT_FORGET:
		sound(SND_SELECT);
		depth -= 2;	/* back to the list of phones, where its row closes */
		snprintf(leaving_label, sizeof(leaving_label), "%s", m->phones[phone].name);
		leaving_value = state_word(&m->phones[phone]);
		leaving = phone;
		leaving_n = m->n - 1;
		leaving_shown = 0;
		cur->focus = phone;
		tell(UI_EV_FORGET, phone, 0);
		break;
	case ACT_REBOOT:
		sound(SND_SELECT);
		tell(UI_EV_REBOOT, 0, 0);
		break;
	case ACT_NAME:
		sound(SND_SELECT);
		opening_by_touch = by_touch;
		if (by_touch) {
			open_at_once(PAGE_TEXT, 0);
		} else {
			ui_list_ack(&list);
			open_page(PAGE_TEXT, 0);
		}
		break;
	}
}

static void tap_row(int row, int part)
{
	settle();
	if (!on_list() || row >= list.n)
		return;
	if (rows[row].disabled) {
		ui_list_deny(row);
		sound(SND_DENY);
		return;
	}
	/* The ring appears on the touched row without travelling. A touch on
	 * the row that has it moves nothing. */
	if (row != cur->focus) {
		adjusting = 0;
		cur->focus = row;
		keep_in_view();
		ui_list_put(&list);
	}
	if (part)	/* minus or plus: one step, without "adjusting" */
		amount_step(part);
	else if (acts[row].act != ACT_AMOUNT)
		list_push(1);
}

/* ---- the instruction page ---- */

static void page_push(void)
{
	if (cur->focus == 0) {	/* "Cancel" */
		sound(SND_BACK);
		close_page();
		return;
	}
	sound(SND_SELECT);
	tell(UI_EV_UPDATE, 0, 0);
}

/* The ring starts on "Cancel"; the knob moves between the two buttons and
 * bumps at either end. */
static void page_turn(int dir)
{
	if (cur->focus + dir < 0 || cur->focus + dir > 1) {
		ui_steps_bump(cur->focus, dir > 0);
		sound(SND_BUMP);
		return;
	}
	cur->focus += dir;
	ui_steps_focus(cur->focus);
	sound(SND_TICK);
}

static void tap_button(int button)
{
	settle();
	if (cur->page != PAGE_UPDATE)
		return;
	cur->focus = button;
	ui_steps_focus(button);
	page_push();
}

/* ---- the top band's buttons (touch) ---- */

static void tap_cog(void)
{
	settle();
	if (depth > 0 || cur->page != PAGE_HOME)	/* the cog is Home's */
		return;
	cur->focus = UI_FOCUS_COG;
	home_push();
}

/* "Cancel" on the waiting screen: the knob's Back by touch. */
static void tap_cancel(void)
{
	ui_nav_key(KEY_BACK);
}

/* ---- the interface ---- */

/* No launcher screen: the layer is clear (CarPlay or the BMW picture shows
 * through) or plain ground (a hand-off between pictures). */
static void no_screen(enum ui_layer l)
{
	cancel_opening();
	shows++;
	depth = 0;
	adjusting = 0;
	cur->page = PAGE_HIDDEN;
	ui_banner_hide();
	ui_frame_content();	/* nothing of the last screen lives on behind: no spinner turns unseen */
	ui_frame_layer(l);
}

/* A launcher screen is about to be drawn. */
static void screen_up(void)
{
	cancel_opening();
	shows++;
	ui_banner_hide();
	ui_frame_layer(LAYER_SCREEN);
}

void ui_nav_init(struct ui_model *model, ui_event_cb cb)
{
	m = model;
	notify = cb;
	theme_set_mode(m->set[SET_EFFECTS] ? UI_FULL : UI_LITE);
	ui_frame_on_tap(tap_cog);
	no_screen(LAYER_CLEAR);	/* the launcher starts unseen: the BMW picture is the driver's */
}

void ui_nav_hide(void)
{
	no_screen(LAYER_CLEAR);
}

void ui_nav_cover(void)
{
	no_screen(LAYER_GROUND);
}

static int banner_other;

static void banner_tapped(void)
{
	notify(UI_EV_SWITCH, banner_other, 0);
}

/* The switch banner, when CarPlay has just come up (design/screens.md, Over
 * CarPlay): only over CarPlay, only if the driver wants it, never for a
 * guest, and only if there is another phone to offer: the one used before
 * this one, else the first other one. */
int ui_nav_banner(void)
{
	int current = -1, other = -1;

	if (cur->page != PAGE_HIDDEN || ui_frame_layer_now() != LAYER_CLEAR)
		return 0;
	if (!m->set[SET_OFFER_OTHER_PHONE] || m->guest)
		return 0;
	for (int i = 0; i < m->n; i++) {
		if (m->phones[i].state == PHONE_CONNECTED)
			current = i;
		if (m->phones[i].state == PHONE_LAST_USED)
			other = i;
	}
	for (int i = 0; other < 0 && i < m->n; i++)
		if (i != current)
			other = i;
	if (current < 0 || other < 0)
		return 0;
	banner_other = other;
	ui_banner_show(m->phones[current].name, m->phones[other].name, banner_tapped);
	return 1;
}

int ui_nav_owns(int x, int y)
{
	return ui_frame_layer_now() == LAYER_CLEAR ? ui_banner_owns(x, y) : 1;
}

void ui_nav_home(int focus)
{
	screen_up();
	depth = 0;
	adjusting = 0;
	cur->page = PAGE_HOME;
	if (focus == UI_FOCUS_DEFAULT) {
		int last = -1;

		focus = -1;
		for (int i = 0; i < m->n; i++) {
			if (m->phones[i].state == PHONE_CONNECTED)
				focus = i;
			if (m->phones[i].state == PHONE_LAST_USED)
				last = i;
		}
		if (focus < 0)
			focus = m->profile_phone >= 0 && m->profile_phone < m->n ? m->profile_phone : last >= 0 ? last : 0;
	}
	cur->focus = focus;
	show();
}

void ui_nav_wait(enum ui_wait kind, const char *name)
{
	screen_up();
	waiting = kind;
	snprintf(waiting_for, sizeof(waiting_for), "%s", name);
	if (depth == 0 && cur->page == PAGE_WAIT) {
		ui_wait_set(kind, name);	/* the spinner keeps turning */
		return;
	}
	depth = 0;
	adjusting = 0;
	cur->page = PAGE_WAIT;
	bar();
	ui_wait_show(kind, name, tap_cancel);
}

void ui_nav_receive(const char *ssid, const char *psk, const char *addr, const char *status)
{
	snprintf(rx_status, sizeof(rx_status), "%s", status);
	if (cur->page == PAGE_RECEIVE) {
		ui_receive_status(rx_status);
		return;
	}
	if (cur->page == PAGE_HIDDEN || depth + 1 >= (int)(sizeof(stack) / sizeof(stack[0])))
		return;
	snprintf(rx_ssid, sizeof(rx_ssid), "%s", ssid);
	snprintf(rx_psk, sizeof(rx_psk), "%s", psk);
	snprintf(rx_addr, sizeof(rx_addr), "%s", addr);
	adjusting = 0;
	depth++;
	cur->page = PAGE_RECEIVE;
	cur->param = 0;
	cur->top = 0;
	cur->focus = 1;	/* "Next": the way on, and nothing is lost by it */
	rx_step = 1;
	arrive(UI_CHANGE_FORWARD);
	show();
	arrived();
}

void ui_nav_receive_next(void)
{
	if (cur->page != PAGE_RECEIVE || rx_step != 1)
		return;
	rx_step = 2;
	cur->focus = 0;
	arrive(UI_CHANGE_FORWARD);
	show();
	arrived();
}

int ui_nav_receive_step(void)
{
	return cur->page == PAGE_RECEIVE ? rx_step : 0;
}

static void tap_receive(int button)
{
	settle();
	if (cur->page != PAGE_RECEIVE)
		return;
	cur->focus = button;
	ui_nav_key(KEY_PUSH);
}

void ui_nav_refresh(void)
{
	if (cur->page == PAGE_HIDDEN)
		return;
	if (cur->page == PAGE_WAIT) {
		bar();		/* the line is the application's: only the clock and links change */
		return;
	}
	show();
}

void ui_nav_key(enum ui_key k)
{
	if (cur->page == PAGE_HIDDEN)	/* the knob is CarPlay's or the car's: the banner never takes it */
		return;
	settle();
	if (cur->page == PAGE_HOME) {
		if (k == KEY_PUSH) {
			home_push();
		} else if (k == KEY_BACK) {
			sound(SND_BACK);
			notify(UI_EV_LEAVE, 0, 0);
		} else {
			home_turn(k == KEY_RIGHT ? 1 : -1);
		}
	} else if (cur->page == PAGE_WAIT) {
		/* Nothing to operate. Back cancels, except during an update. */
		if (k == KEY_BACK && waiting != WAIT_UPDATING) {
			sound(SND_BACK);
			notify(UI_EV_CANCEL, 0, 0);
		} else {
			sound(SND_BUMP);
		}
	} else if (cur->page == PAGE_TEXT) {
		ui_text_key(k);
	} else if (cur->page == PAGE_RECEIVE) {
		/* Step 1: "Cancel" and "Next", the ring on "Next"; step 2: "Cancel"
		 * alone. Back is "Cancel": to the list the instruction page was
		 * opened from. */
		const int last = rx_step == 1 ? 1 : 0;

		if (k == KEY_PUSH && rx_step == 1 && cur->focus == 1) {
			sound(SND_SELECT);
			ui_nav_receive_next();
		} else if (k == KEY_PUSH || k == KEY_BACK) {
			sound(SND_BACK);
			depth -= depth > 1 && stack[depth - 1].page == PAGE_UPDATE ? 2 : 1;
			arrive(UI_CHANGE_BACK);
			tell(UI_EV_CANCEL, 0, 0);
			arrived();
		} else if (cur->focus + (k == KEY_RIGHT ? 1 : -1) < 0 || cur->focus + (k == KEY_RIGHT ? 1 : -1) > last) {
			ui_receive_bump(cur->focus, k == KEY_RIGHT);
			sound(SND_BUMP);
		} else {
			cur->focus += k == KEY_RIGHT ? 1 : -1;
			ui_receive_focus(cur->focus);
			sound(SND_TICK);
		}
	} else if (cur->page == PAGE_UPDATE) {
		if (k == KEY_PUSH) {
			page_push();
		} else if (k == KEY_BACK) {	/* the same as "Cancel" */
			sound(SND_BACK);
			close_page();
		} else {
			page_turn(k == KEY_RIGHT ? 1 : -1);
		}
	} else if (k == KEY_PUSH) {
		list_push(0);
	} else if (k == KEY_BACK) {
		if (adjusting) {	/* Back keeps the amount too */
			adjusting = 0;
			keep_in_view();
			ui_list_place(&list);
		} else {
			close_page();
		}
		sound(SND_BACK);
	} else {
		list_turn(k == KEY_RIGHT ? 1 : -1);
	}
}

void ui_nav_message(const char *words, const char *name)
{
	if (cur->page == PAGE_HIDDEN)
		return;
	ui_fit_name(message, sizeof(message), words, name, "", TY_LABEL_20, 520);
	ui_frame_message(message);
	if (on_list())
		ui_list_bump(1);
}

void ui_nav_state(struct ui_nav_state *s)
{
	s->page = cur->page;
	s->focus = cur->focus;
	s->top = cur->top;
	s->adjusting = adjusting;
	s->wait = waiting;
	s->list = on_list() ? &list : NULL;
	s->message = message;
}

const char *ui_nav_entered(void)
{
	return entered;
}

const struct ui_model *ui_nav_model(void)
{
	return m;
}
