/* SPDX-License-Identifier: MIT
 * See sample.h.
 */
#include "sample.h"
#include <stdio.h>
#include <string.h>

#define APOS "\xE2\x80\x99"

static const struct ui_phone five[] = {
	{ "Anna" APOS "s iPhone", PHONE_CONNECTED, 1 }, { "Work iPhone", PHONE_LAST_USED, 0 },
	{ "Mikael" APOS "s iPhone", PHONE_PAIRED, 2 }, { "Guest iPhone", PHONE_UNREACHABLE, 0 },
	{ "Sam" APOS "s iPhone", PHONE_PAIRED, 0 },
};
static const struct ui_phone two[] = {
	{ "Anna" APOS "s iPhone", PHONE_LAST_USED, 1 }, { "Work iPhone", PHONE_PAIRED, 0 },
};
static const struct ui_phone names[] = {	/* long, two-line and non-Latin names */
	{ "Alexandra-Marie" APOS "s iPhone 17 Pro Max", PHONE_LAST_USED, 0 },
	{ "\xD0\x90\xD0\xB9\xD1\x84\xD0\xBE\xD0\xBD \xD0\x9C\xD0\xB0\xD1\x80\xD0\xB8\xD0\xB8", PHONE_PAIRED, 0 },
	{ "J\xC3\xB6rg" APOS "s iPhone", PHONE_PAIRED, 0 },
	{ "Mia \xF0\x9F\x93\xB1 \xE9\x9B\xBB\xE8\xA9\xB1", PHONE_PAIRED, 0 },	/* an emoji and CJK: not in Inter */
	{ "Alexandra-Marie" APOS "s second iPhone 17 Pro Max from the office", PHONE_UNREACHABLE, 0 },
};
static const struct ui_phone nine[] = {
	{ "Anna" APOS "s iPhone", PHONE_CONNECTED, 1 }, { "Work iPhone", PHONE_LAST_USED, 0 },
	{ "Mikael" APOS "s iPhone", PHONE_PAIRED, 2 }, { "Guest iPhone", PHONE_UNREACHABLE, 0 },
	{ "Sam" APOS "s iPhone", PHONE_PAIRED, 0 }, { "Phone six", PHONE_PAIRED, 0 },
	{ "Phone seven", PHONE_PAIRED, 0 }, { "Phone eight", PHONE_PAIRED, 0 }, { "Phone nine", PHONE_PAIRED, 0 },
};

static const struct ui_phone longs[] = {
	{ "Alexandra-Marie" APOS "s iPhone 17 Pro Max", PHONE_CONNECTED, 1 },
	{ "Alexandra-Marie" APOS "s second iPhone 17 Pro Max from the office", PHONE_LAST_USED, 0 },
};

static struct ui_phone phones[16];
static char name[128];
struct ui_model sample;
struct sample_record sample_log[256];
int sample_logged;

void sample_reset(enum sample_set which)
{
	static const struct {
		const struct ui_phone *p;
		int n;
	} sets[] = {
		[SAMPLE_FIVE] = { five, 5 }, [SAMPLE_TWO] = { two, 2 }, [SAMPLE_NONE] = { NULL, 0 },
		[SAMPLE_NAMES] = { names, 5 }, [SAMPLE_NINE] = { nine, 9 },
		[SAMPLE_LONG] = { longs, 2 },
	};
	int connected = which == SAMPLE_FIVE || which == SAMPLE_NINE || which == SAMPLE_LONG;

	if (sets[which].n)
		memcpy(phones, sets[which].p, sets[which].n * sizeof(phones[0]));
	memset(&sample, 0, sizeof(sample));
	sample.phones = phones;
	sample.n = sets[which].n;
	sample.profile_phone = -1;
	sample.chip = connected ? CHIP_UP : CHIP_IDLE;
	sample.chip_text = connected ? "Anna" APOS "s iPhone" : "";
	sample.bt = connected ? UI_BT_OFF : UI_BT_ON;	/* off while CarPlay runs, as on the unit */
	sample.clock = "14:32";
	sample.name = "BMW 28114";
	sample.set[SET_CONNECT_AT_IGNITION] = 1;
	sample.set[SET_OFFER_OTHER_PHONE] = 1;
	sample.set[SET_BRIGHTNESS] = 70;
	sample.set[SET_VOLUME_MEDIA] = 60;
	sample.set[SET_VOLUME_CALL] = 70;
	sample.set[SET_VOLUME_NAVIGATION] = 55;
	sample.set[SET_SOUNDS] = 1;
	sample.kernel = "Linux 4.19";
	sample.processor = "ARK1668";
	sample.panel = "1280 \xC3\x97 480";
	sample.can_update = 1;
	sample_logged = 0;
}

void sample_event(enum ui_event ev, int arg, int value)
{
	if (sample_logged < (int)(sizeof(sample_log) / sizeof(sample_log[0])))
		sample_log[sample_logged++] = (struct sample_record){ ev, arg, value };
	switch (ev) {
	case UI_EV_DISCONNECT:
		phones[arg].state = PHONE_LAST_USED;
		for (int i = 0; i < sample.n; i++)	/* at most one phone is "last used" */
			if (i != arg && phones[i].state == PHONE_LAST_USED)
				phones[i].state = PHONE_PAIRED;
		sample.chip = CHIP_IDLE;
		sample.chip_text = "";
		break;
	case UI_EV_FORGET:
		if (phones[arg].state == PHONE_CONNECTED) {
			sample.chip = CHIP_IDLE;
			sample.chip_text = "";
		}
		memmove(&phones[arg], &phones[arg + 1], (sample.n - arg - 1) * sizeof(phones[0]));
		sample.n--;
		break;
	case UI_EV_PROFILE:
		for (int i = 0; value && i < sample.n; i++)	/* a profile has one phone */
			if (phones[i].profile == value)
				phones[i].profile = 0;
		phones[arg].profile = value;
		break;
	case UI_EV_NAME:
		snprintf(name, sizeof(name), "%s", ui_nav_entered());
		sample.name = name;
		break;
	default:
		break;
	}
}

const char *sample_event_name(enum ui_event ev)
{
	static const char *const n[] = {
		[UI_EV_LEAVE] = "leave", [UI_EV_PHONE] = "phone", [UI_EV_PAIR] = "pair", [UI_EV_CANCEL] = "cancel",
		[UI_EV_DISCONNECT] = "disconnect", [UI_EV_FORGET] = "forget", [UI_EV_PROFILE] = "profile",
		[UI_EV_SETTING] = "setting", [UI_EV_REBOOT] = "reboot", [UI_EV_SOUND] = "sound", [UI_EV_NAME] = "name",
		[UI_EV_UPDATE] = "update", [UI_EV_SWITCH] = "switch",
	};

	return n[ev];
}
