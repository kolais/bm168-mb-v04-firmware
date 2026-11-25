/* SPDX-License-Identifier: MIT
 * Draw one launcher screen into a picture, for design review on the host
 * (make ui-snap) and under qemu from the ARM build. Sample names only.
 *
 *   launcher-snap SCENE OUT.ppm [full] [rhd] [do=STEPS] [at=MS] [log]
 *
 * A scene is a starting screen and a few inputs. do=STEPS adds inputs:
 * l, r one knob detent left or right; p push; b Back; c tap the cog;
 * tX,Y; a tap at (X, Y); w wait 100 ms. at=MS then lets MS pass, and the
 * picture is taken. log prints the events the application would be told.
 */
#include "host.h"
#include "sample.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APOS "\xE2\x80\x99"
#define LONG_NAME "Alexandra-Marie" APOS "s second iPhone 17 Pro Max from the office"
#define SETTINGS "cw"		/* from Home: tap the cog */

static const struct scene {
	const char *name;
	enum sample_set phones;
	int home;		/* Home with the ring on this tile; or */
	int waits;		/* the waiting screen: */
	enum ui_wait wait;
	const char *wait_name;
	int then;		/* 1 + the kind its line changes to after the time has passed */
	const char *chip;	/* Home's chip, if not the sample's */
	int moving;		/* the car moves: no software update */
	int banner;		/* CarPlay has the screen, with the switch banner over it */
	const char *steps;
} scenes[] = {
	{ .name = "home", .home = UI_FOCUS_DEFAULT },
	{ .name = "home-page2", .home = 5 },
	{ .name = "home-cog", .home = UI_FOCUS_COG },
	{ .name = "home-idle", .phones = SAMPLE_TWO, .home = UI_FOCUS_DEFAULT },
	{ .name = "home-profile", .phones = SAMPLE_TWO, .home = UI_FOCUS_DEFAULT, .chip = "No phone for profile 2" },
	{ .name = "home-guest", .phones = SAMPLE_TWO, .home = UI_FOCUS_DEFAULT, .chip = "Guest" },
	{ .name = "home-empty", .phones = SAMPLE_NONE, .home = UI_FOCUS_DEFAULT },
	{ .name = "home-names", .phones = SAMPLE_NAMES, .home = UI_FOCUS_DEFAULT },
	{ .name = "wait-connecting", .waits = 1, .wait = WAIT_CONNECTING, .wait_name = "Work iPhone" },
	{ .name = "wait-retrying", .waits = 1, .wait = WAIT_RETRYING, .wait_name = "Work iPhone" },
	{ .name = "wait-failed", .waits = 1, .wait = WAIT_FAILED, .wait_name = "Work iPhone" },
	{ .name = "wait-pairing", .waits = 1, .wait = WAIT_PAIRING, .wait_name = "BMW 28114" },
	{ .name = "wait-updating", .waits = 1, .wait = WAIT_UPDATING, .wait_name = "" },
	{ .name = "wait-long", .waits = 1, .wait = WAIT_CONNECTING, .wait_name = LONG_NAME },
	{ .name = "wait-long-pairing", .waits = 1, .wait = WAIT_PAIRING, .wait_name = LONG_NAME },
	/* the line changed in place, as launcherd will do it */
	{ .name = "wait-then-retrying", .waits = 1, .wait = WAIT_CONNECTING, .wait_name = "Work iPhone",
	  .then = 1 + WAIT_RETRYING },
	{ .name = "wait-then-failed", .waits = 1, .wait = WAIT_CONNECTING, .wait_name = "Work iPhone",
	  .then = 1 + WAIT_FAILED },
	/* the lists, reached as the driver would */
	{ .name = "settings", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS },
	{ .name = "general", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" },
	{ .name = "phones", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" },
	{ .name = "phones-leaving", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" "rpw" "rrpw" "rp" "w" },
	{ .name = "phones-message", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" "r" "m" },
	{ .name = "phones-none", .phones = SAMPLE_NONE, .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" },
	{ .name = "phones-nine", .phones = SAMPLE_NINE, .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" "rrrrrrrr" },
	{ .name = "phones-names", .phones = SAMPLE_NAMES, .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" },
	{ .name = "phone", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" "rpw" },
	{ .name = "phone-connected", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" "pw" },
	{ .name = "profile", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" "rrpw" "rpw" },
	{ .name = "forget", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "rrrpw" "rpw" "rrpw" },
	{ .name = "display", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rpw" },
	{ .name = "display-adjusting", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rpw" "prr" },
	{ .name = "sound", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrpw" },
	{ .name = "equaliser", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrpw" "rrrrpw" "prrp" "r" "plllp" "r" },
	{ .name = "system", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrrpw" },
	{ .name = "info", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrrpw" "pw" },
	{ .name = "reboot", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrrpw" "rpw" },
	/* over CarPlay (a stand-in picture behind the layer) */
	{ .name = "banner", .banner = 1 },
	{ .name = "banner-long", .phones = SAMPLE_LONG, .banner = 1 },
	/* the instruction page */
	{ .name = "info-moving", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrrpw" "pw", .moving = 1 },
	{ .name = "update", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrrpw" "pw" "rrrpw" },
	{ .name = "update-start", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrrpw" "pw" "rrrpw" "r" },
	{ .name = "update-receive", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrrpw" "pw" "rrrpw" "r" "u" },
	{ .name = "update-receive-2", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "rrrpw" "pw" "rrrpw" "r" "u" "p" },
	/* text entry: the dial (opened with the knob) and the keyboard (opened with a tap) */
	{ .name = "name-dial", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "pw" },
	{ .name = "name-dial-delete", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "pw" "rrrrrrrp" "lllllllll" "l" },
	{ .name = "name-dial-small", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "pw" "llllp" "rrrr" },
	{ .name = "name-dial-digits", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "pw" "lllllp" "rrrrrrrrrrrr" },
	{ .name = "name-keys", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "pw" "t696,270;" },
	{ .name = "name-keys-shift", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "pw" "t696,270;" "t224,286;" },
	{ .name = "name-keys-digits", .home = UI_FOCUS_DEFAULT, .steps = SETTINGS "pw" "pw" "t696,270;" "t252,354;" },
};
#define SCENES (sizeof(scenes) / sizeof(scenes[0]))

static void steps(const char *s)
{
	for (; s && *s; s++) {
		if (*s == 'l' || *s == 'r' || *s == 'p' || *s == 'b') {
			ui_nav_key(*s == 'l' ? KEY_LEFT : *s == 'r' ? KEY_RIGHT : *s == 'p' ? KEY_PUSH : KEY_BACK);
			host_run(0);
		} else if (*s == 'w') {
			host_run(100);
		} else if (*s == 'u') {	/* "Start" on the update's page: the receiving screen */
			ui_nav_receive("BMW 28114", "acorn4river7tulip2mango", "192.168.43.1", "Waiting for the update file");
			host_run(0);
		} else if (*s == 'm') {	/* a phone that could not be forgotten */
			ui_nav_message("Couldn\xE2\x80\x99t forget ", "Work iPhone");
			host_run(0);
		} else if (*s == 'c') {
			host_tap(sample.set[SET_STEERING_RIGHT] ? T_SCREEN_W - UI_EDGE - UI_COG / 2 : UI_EDGE + UI_COG / 2,
				 UI_COG_Y + UI_COG / 2);
		} else if (*s == 't') {
			int x = 0, y = 0;

			sscanf(s + 1, "%d,%d", &x, &y);
			while (s[1] && *s != ';')
				s++;
			host_tap(x, y);
		}
	}
}

int main(int argc, char **argv)
{
	int full = 0, rhd = 0, at = 0, log = 0;
	const char *more = NULL;
	const struct scene *s = NULL;

	for (int i = 3; i < argc; i++) {
		full |= !strcmp(argv[i], "full");
		rhd |= !strcmp(argv[i], "rhd");
		log |= !strcmp(argv[i], "log");
		if (!strncmp(argv[i], "at=", 3))
			at = atoi(argv[i] + 3);
		if (!strncmp(argv[i], "do=", 3))
			more = argv[i] + 3;
	}
	for (size_t i = 0; argc >= 3 && i < SCENES; i++)
		if (!strcmp(scenes[i].name, argv[1]))
			s = &scenes[i];
	if (!s) {
		fprintf(stderr, "usage: launcher-snap SCENE OUT.ppm [full] [rhd] [do=STEPS] [at=MS] [log]\nscenes:");
		for (size_t i = 0; i < SCENES; i++)
			fprintf(stderr, " %s", scenes[i].name);
		fprintf(stderr, "\n");
		return 2;
	}
	if (host_init()) {
		fprintf(stderr, "launcher-snap: could not start (fonts?)\n");
		return 1;
	}
	sample_reset(s->phones);
	sample.set[SET_EFFECTS] = full;
	sample.set[SET_STEERING_RIGHT] = rhd;
	sample.can_update = !s->moving;
	if (s->chip) {
		sample.chip = CHIP_IDLE;
		sample.chip_text = s->chip;
	}
	ui_nav_init(&sample, sample_event);
	if (s->banner)
		ui_nav_banner();
	else if (s->waits)
		ui_nav_wait(s->wait, s->wait_name);
	else
		ui_nav_home(s->home);
	host_run(0);
	steps(s->steps);
	steps(more);
	host_run(at);
	if (s->then)
		ui_nav_wait(s->then - 1, s->wait_name);
	host_draw();
	for (int i = 0; log && i < sample_logged; i++)
		printf("%s %d %d\n", sample_event_name(sample_log[i].ev), sample_log[i].arg, sample_log[i].value);
	return host_save_ppm(argv[2], s->banner) ? 1 : 0;
}
