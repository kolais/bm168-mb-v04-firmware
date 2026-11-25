/* SPDX-License-Identifier: MIT
 * Scenario tests of the launcher without the unit: launcherd's logic
 * (launcherd.c) and the real screens (launcher/ui/), with the io side (io.h)
 * as a recording double, the clock the test's, the layer in memory and the
 * stock root a scratch directory. A scenario feeds what gocsdk, carplay, the
 * MCU and a finger would send, lets time pass, and checks what the launcher
 * sent and what the driver sees, in order.
 *
 *   launcher-coretest            all scenarios; exit status 0 = all passed
 *   launcher-coretest -v NAME    one scenario, printing everything it did
 *   CORETEST_OUT=DIR             also save the pictures the scenarios take
 *
 * Sample phones only. Each scenario runs in a process of its own, because
 * launcherd.c keeps its state in statics, as it does on the unit.
 */
#include "io.h"
#include "phones.h"
#include "display.h"
#include "nav.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- what launcherd did, in order ---- */

static char did[2048][120];
static int n_did, cursor, verbose, fails, checks;
static long clock_ms = 1000;

static void note_screen(void);
static int noting;

static void rec(const char *fmt, ...)
{
	va_list ap;

	if (!noting)
		note_screen();	/* what the driver saw by the time this was done */
	if (n_did >= (int)(sizeof(did) / sizeof(did[0])))
		return;
	va_start(ap, fmt);
	vsnprintf(did[n_did], sizeof(did[0]), fmt, ap);
	va_end(ap);
	if (verbose)
		printf("%8.1f    %s\n", clock_ms / 1000.0, did[n_did]);
	n_did++;
}

/* The next thing launcherd did that reads `what`, after the last one found. */
static int find(const char *what)
{
	for (int i = cursor; i < n_did; i++)
		if (!strcmp(did[i], what))
			return i;
	return -1;
}

#define THEN(what) \
	do { \
		int at_ = find(what); \
		checks++; \
		if (at_ < 0) { \
			fails++; \
			printf("FAIL %s:%d: expected next: %s\n", __func__, __LINE__, what); \
		} else { \
			cursor = at_ + 1; \
		} \
	} while (0)

/* Nothing reading `what` since the last THEN. */
#define NOT(what) \
	do { \
		checks++; \
		if (find(what) >= 0) { \
			fails++; \
			printf("FAIL %s:%d: not expected: %s\n", __func__, __LINE__, what); \
		} \
	} while (0)

#define CHECK(cond) \
	do { \
		checks++; \
		if (!(cond)) { \
			fails++; \
			printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
		} \
	} while (0)

/* ---- the io side (io.h) ---- */

static char root[96];
static int bt_link, carplay_alive;
static long frames;

long io_now_ms(void)
{
	return clock_ms;
}

void io_log(const char *line)
{
	if (verbose)
		printf("         %s\n", line);
}

const char *io_root(void)
{
	return root;
}

int io_mcu_open(void)
{
	return 1;
}

int io_mcu_write(const unsigned char *f, int n)
{
	char s[100] = "mcu";	/* command and data, without the frame around them */

	for (int i = 4; i < n - 1; i++)
		snprintf(s + strlen(s), sizeof(s) - strlen(s), " %02x", f[i]);
	rec("%s", s);
	return 0;
}

int io_bt_open(void)
{
	return bt_link;
}

int io_bt_connect(void)
{
	bt_link = 1;
	return 0;
}

int io_bt_write(const char *line, int n)
{
	rec("bt %.*s", n - 5, line + 3);	/* without "AT#" and the line end */
	return 0;
}

void io_call(const char *dest, const char *path, const char *iface, const char *member,
	     const struct dbm_arg *a, int n)
{
	char s[120];

	(void)path;
	(void)iface;
	if (!strcmp(member, "requestTouchStatus")) {	/* its third argument is XML */
		rec("cp touch %d", a[1].i);
		return;
	}
	snprintf(s, sizeof(s), "%s %s", strstr(dest, "audio") ? "au" : "cp", member);
	for (int i = 0; i < n; i++)
		if (a[i].type == 'i')
			snprintf(s + strlen(s), sizeof(s) - strlen(s), " %d", (int)a[i].i);
		else
			snprintf(s + strlen(s), sizeof(s) - strlen(s), " %s", a[i].s);
	rec("%s", s);
}

long io_vdec_frames(void)
{
	return frames;
}

int io_carplay_running(void)
{
	return carplay_alive;
}

void io_carplay_end(void)
{
	rec("carplay end");
	carplay_alive = 0;
}

void io_ap_name(const char *name)
{
	rec("ap name %s", name);
}

void io_rotate_ap(void)
{
	rec("ap rotate");
}

static void put(const char *rel, const char *text);

void io_put(const char *path, const char *text)
{
	char line[200];

	rec("put %s %s", path, text);
	snprintf(line, sizeof(line), "%s\n", text);
	put(path, line);
}

void io_sound(int which)
{
	static const char *const names[] = { "tick", "key", "bump", "select", "back", "on", "off", "deny", "connected", "lost" };

	rec("sound %s", names[which]);
}

static int state_kept;	/* a scenario's unit has the state partition */

int io_state_kept(void)
{
	return state_kept;	/* 0: as on a unit without the state partition: the name row needs BENCH */
}

void io_app_confirm(void)
{
	rec("app confirm");
}

static int dev_state;	/* a scenario's image: 1 built with a developer's key, +2 access is on */

int io_dev_state(void)
{
	return dev_state;
}

void io_dev_access(int on)
{
	rec("developer access %s", on ? "on" : "off");
}

static const char *update_status;	/* the receiver's line, as a scenario sets it */

int io_update_start(void)
{
	rec("update start");
	return 0;
}

void io_update_stop(void)
{
	rec("update stop");
}

int io_update_status(char *line, int n)
{
	if (!update_status)
		return 0;
	snprintf(line, n, "%s", update_status);
	return 1;
}

static int ap_now;	/* phones on the access point, as a scenario sets it */

int io_ap_clients(void)
{
	return ap_now;
}

void io_ap_info(char *ssid, int ssid_n, char *psk, int psk_n)
{
	snprintf(ssid, ssid_n, "Unit Wi-Fi");
	snprintf(psk, psk_n, "pass;word");
}

void io_state_save(void)
{
	rec("state save");
}

void io_fb_clear(void)
{
	rec("fb clear");
}

void io_reboot(void)
{
	rec("reboot");
}

/* ---- the screen: the real UI, drawn into memory ---- */

static uint8_t *pixels;

static uint32_t ticks(void)
{
	return (uint32_t)clock_ms;
}

/* What is on the layer, in a word or two. */
static const char *screen(void)
{
	static const char *const waits[] = {
		[WAIT_CONNECTING] = "wait connecting", [WAIT_RETRYING] = "wait retrying", [WAIT_FAILED] = "wait failed",
		[WAIT_PAIRING] = "wait pairing", [WAIT_UPDATING] = "wait updating",
	};
	static char name[64];
	struct ui_nav_state s;

	if (ui_frame_layer_now() == LAYER_GROUND)
		return "cover";
	if (ui_frame_layer_now() == LAYER_CLEAR)
		return ui_banner_shown() ? "banner" : "clear";
	ui_nav_state(&s);
	if (s.page == PAGE_HOME)
		return "home";
	if (s.page == PAGE_WAIT)
		return waits[s.wait];
	if (s.list) {
		snprintf(name, sizeof(name), "list %s", s.list->title);
		return name;
	}
	if (s.page == PAGE_RECEIVE)
		return "receive";
	return "other";
}

static void note_screen(void)
{
	static char last[64] = "";

	if (!pixels || !strcmp(last, screen()))
		return;
	snprintf(last, sizeof(last), "%s", screen());
	noting = 1;
	rec("screen %s", last);
	noting = 0;
}

static struct ui_nav_state st(void)
{
	struct ui_nav_state s;

	ui_nav_state(&s);
	return s;
}

/* The picture the driver would see, for the record (CORETEST_OUT). */
static void picture(const char *name)
{
	const char *dir = getenv("CORETEST_OUT");
	char path[200];
	FILE *f;

	if (!dir)
		return;
	ui_display_draw_now();
	snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
	f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", T_SCREEN_W, T_SCREEN_H);
	for (int i = 0; i < T_SCREEN_W * T_SCREEN_H; i++) {
		const uint8_t *p = pixels + 4 * i;	/* B G R A; a stand-in for CarPlay behind */
		int x = i % T_SCREEN_W, y = i / T_SCREEN_W, a = p[3];
		int under[3] = { 24 + x * 40 / T_SCREEN_W, 60 + y * 60 / T_SCREEN_H, 110 + x * 60 / T_SCREEN_W };
		unsigned char rgb[3] = {
			(unsigned char)((p[2] * a + under[0] * (255 - a)) / 255),
			(unsigned char)((p[1] * a + under[1] * (255 - a)) / 255),
			(unsigned char)((p[0] * a + under[2] * (255 - a)) / 255),
		};

		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* ---- the scratch root: what launcherd reads under /mnt/stock ---- */

static void put(const char *rel, const char *text)
{
	char path[200];
	FILE *f;

	snprintf(path, sizeof(path), "%s%s", root, rel);
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(2);
	}
	fputs(text, f);
	fclose(f);
}

#define PHONE1 "name0=Phone one\naddr0=aabbccddee01\n"
#define PHONE2 "name1=Phone two\naddr1=aabbccddee02\n"
#define PHONE3 "name2=Phone three\naddr2=aabbccddee03\n"

/* gocsdk's phones, as the real one keeps them: three slots that hold phones
 * one, two and three (a dropped slot keeps its lines), and `order`, the slots
 * that are paired, in gocsdk's own count: the most recently connected first. */
static void paired(const char *order)
{
	char text[512];
	int total = 0;

	for (const char *c = order; *c; c++)
		total += *c == ',';
	snprintf(text, sizeof(text), "pairlisttotal=%d\npairlistindex=%s\n" PHONE1 PHONE2 PHONE3 "lastaddr=aabbccddee01\n",
		 total, order);
	put("/etc/goc/bt_conf.ini", text);
}

static void make_root(void)
{
	static const char *const dirs[] = { "", "/etc", "/etc/goc", "/data", "/tmp" };
	char path[200];

	snprintf(root, sizeof(root), "/tmp/launcher-coretest-%d", (int)getpid());
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s%s", root, dirs[i]);
		mkdir(path, 0700);
	}
	paired("0,1,");	/* phones one and two; slot 2 holds a phone dropped earlier */
	put("/data/Blutooth.ini", "DeviceName=TESTKIT\nPinCode=1234\n");
	put("/data/bluetoothaddress", "00:11:22:33:44:55\n");
	put("/etc/driverDirection", "1\n");
}

static void remove_root(void)
{
	static const char *const files[] = {
		"/etc/goc/bt_conf.ini", "/data/Blutooth.ini", "/data/bluetoothaddress", "/etc/driverDirection",
		"/tmp/bluetoothaddress", "/etc/goc", "/etc", "/data", "/tmp", "",
	};
	char path[200];

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s%s", root, files[i]);
		remove(path);
	}
}

/* ---- what the outside does ---- */

/* Time passes, in the 10 ms passes of the unit's loop. */
static void run(int ms)
{
	for (long end = clock_ms + ms; clock_ms < end;) {
		clock_ms += 10;
		core_flush();
		core_tick();
		note_screen();
	}
}

static void bt(const char *line)	/* gocsdk says */
{
	core_bt_bytes(line, (int)strlen(line));
	core_bt_bytes("\r\n", 2);
	note_screen();
}

static void cp(int status)	/* carplay's link status */
{
	carplay_alive = 1;	/* whoever reports is running, and stays so until it is ended */
	core_link_status(7, status);
	note_screen();
}

/* Bluetooth has come on: IS1, then gocsdk lists its phones in its count. */
static void bt_up(const char *order)
{
	char line[40];
	int n = 0;

	bt("IS1");
	for (const char *c = order; *c; c++)
		if (*c != ',') {
			snprintf(line, sizeof(line), "MX%daabbccddee0%dPhone", ++n, *c - '0' + 1);
			bt(line);
		}
	snprintf(line, sizeof(line), "PO%d", n);
	bt(line);
}

static void mcu(int cmd, int n, ...)	/* an MCU report */
{
	unsigned char f[64] = { 0xAA, 0x55, 0, (unsigned char)(n + 2), (unsigned char)cmd };
	int sum = 0, len = 5;
	va_list ap;

	va_start(ap, n);
	for (int i = 0; i < n; i++)
		f[len++] = (unsigned char)va_arg(ap, int);
	va_end(ap);
	for (int k = 2; k < len; k++)
		sum += f[k];
	f[len++] = (unsigned char)(-sum);
	core_mcu_bytes(f, len);
	note_screen();
}

static void profile(int code)	/* the patched MCU: the car's driver profile, fresh */
{
	mcu(0xE6, 4, 0x02, 0x3A, code, 0x01);
}

/* The iDrive controller, as the MCU reports it (codes from the car). */
static void knob(char c)
{
	if (c == 'r' || c == 'l')
		mcu(0xD0, 5, 0x4B, 0x81, 0x01, c == 'r' ? 0x47 : 0x46, c == 'r' ? 0x21 : 0x11);
	else
		mcu(0xD0, 5, 0x4B, 0x81, 0x00, c == 'p' ? 0x61 : c == 'b' ? 0x62 : 0x63, 0x00);
	if (c != 'r' && c != 'l')
		mcu(0xD0, 5, 0x4B, 0x81, 0x00, 0x00, 0x30);	/* the release */
	run(10);
}

static void knobs(const char *s)
{
	for (; *s; s++)
		if (*s == 'w')
			run(120);
		else
			knob(*s);
}

static void tap(int x, int y)
{
	ui_display_draw_now();	/* a finger touches what is on the screen */
	core_touch(1, x, y);
	run(60);
	core_touch(3, x, y);
	run(60);
}

/* Where things are, with the driver on the right (the sample root says so):
 * two phones and "Pair a phone" take the last three of the four places. */
#define TILE_X(i) (T_SCREEN_W - UI_EDGE - 1104 + ((i) + 1) * 280 + 132)
#define TILE_Y 234
#define CANCEL_X 640	/* "Cancel" on the waiting screen */
#define CANCEL_Y 370
#define PILL_X 640	/* the banner: the whole of it is "Switch to" */
#define PILL_Y 72

static const struct ui_model *model(void)
{
	return ui_nav_model();
}

static int same(const char *a, const char *b)
{
	return a && b && !strcmp(a, b);
}

/* ---- pieces of a drive, reused by the scenarios ---- */

/* Power-on. Nothing connects by itself yet; Home is ready behind the car's picture. */
static const char *start_name = "TESTKIT";	/* the name the start sets gocsdk up with */

static void start_up(void)
{
	char mm[80];

	snprintf(mm, sizeof(mm), "bt MM%s", start_name);
	pixels = calloc(T_SCREEN_W * T_SCREEN_H, 4);
	core_begin();
	if (!pixels || ui_display_init(pixels, ticks, NULL)) {
		printf("could not start the display\n");
		exit(2);
	}
	core_start(1);
	note_screen();
	/* stock's start frames, then gocsdk set up, 400 ms apart */
	THEN("mcu 01 00");
	THEN("mcu 61 3c");
	THEN("mcu 02 01");
	THEN("mcu 04 fc");
	THEN("bt P1");
	THEN("screen home");
	run(3000);
	THEN(mm);
	THEN("bt MN1234");
	THEN("bt MH");
	THEN("bt MQ");
	THEN("bt QB");
	THEN("bt CY");
	THEN("bt CA00");	/* not connectable: no phone comes by itself */
	cursor = 0;
	NOT("mcu 4c 00 00");	/* the car's picture is left alone */
	NOT("bt CCAABBCCDDEE01");
	NOT("bt CCAABBCCDDEE02");
}

/* The paged phone answers; CarPlay starts on it. */
static void phone_answers(const char *name, const char *addr)
{
	char want[80];

	int n = 2;

	bt("IB");
	memcpy(want, "JH", 2);	/* gocsdk: the address first, then the name */
	for (const char *c = addr; *c; c++)
		if (*c != ':')
			want[n++] = *c;
	want[n] = 0;
	bt(want);
	snprintf(want, sizeof(want), "SA%s", name);
	bt(want);
	THEN("cp requsetSetEnvValue 7 CARPLAY_RIGHTHAND_DRIVER 1");
	snprintf(want, sizeof(want), "cp requestSetPhoneBTAddr 7 %s 1", addr);
	THEN(want);
	THEN("cp requestLinkStatus 7 3");
}

/* The cover goes once 12 frames are decoded and half a second has passed. */
static void frames_decode(void)
{
	run(100);
	NOT("screen clear");
	NOT("screen banner");
	frames += 12;
	run(600);
}

/* carplay's statuses of a start, to CarPlay on the screen. */
static void session_comes_up(void)
{
	cp(3);
	THEN("mcu 04 02");
	cp(62);	/* the phone takes the screen ~0.3 s before the session is up */
	cp(4);
	run(900);
	THEN("bt CD");	/* Bluetooth off while CarPlay runs */
	THEN("bt P0");
	frames_decode();
}

/* Ignition on with the owner's key: profile 11 is phone one's. */
static void drive_with_profile(void)
{
	start_up();
	profile(0x11);
	run(1500);
	THEN("bt CCAABBCCDDEE01");
	NOT("screen wait connecting");	/* in the background: nothing of ours is shown for it */
	run(1500);
	phone_answers("Phone one", "AA:BB:CC:DD:EE:01");
	cp(3);
	THEN("mcu 04 02");
	cp(62);
	THEN("screen cover");	/* Home was drawn behind the car's picture: the cover first */
	THEN("mcu 4c 00 00");	/* then the picture comes to the unit */
	cp(4);
	run(900);
	THEN("bt CD");
	THEN("bt P0");
	cursor = 0;
	THEN("au requestAudioSource 4");
	frames_decode();
	THEN("screen banner");	/* CarPlay, with the offer of the other phone over it */
}

/* The bench, or a car that says nothing: the wait ends, Home waits. */
static void nobody_drives(void)
{
	start_up();
	run(6000);
	THEN("bt CD");	/* Bluetooth off: nothing connects by itself */
	THEN("bt P0");
	tap(640, 300);	/* a touch on the car's picture brings Home */
	THEN("mcu 4c 00 00");
}

/* ---- scenarios ---- */

/* No profile from the car (the bench): nothing is paged, ever; a touch
 * brings Home; a tile connects. */
static void s_no_profile(void)
{
	start_up();
	picture("home-waiting");
	CHECK(same(model()->chip_text, "") && model()->n == 2);
	CHECK(model()->phones[0].state == PHONE_LAST_USED && model()->phones[1].state == PHONE_PAIRED);
	run(4000);
	NOT("bt CD");	/* the wait for the car's profile: 8 s from the start */
	run(2000);
	THEN("bt CD");
	THEN("bt P0");
	run(60000);
	NOT("bt P1");
	NOT("bt CCAABBCCDDEE01");
	NOT("mcu 4c 00 00");
	CHECK(same(screen(), "home"));
	tap(640, 300);	/* on the car's picture: Home comes, and the touch does nothing else */
	THEN("mcu 4c 00 00");
	NOT("screen wait connecting");
	tap(TILE_X(0), TILE_Y);
	THEN("screen wait connecting");
	THEN("ap rotate");
	THEN("bt P1");
	picture("connecting");
	run(5000);
	bt("IS1");
	run(1500);
	THEN("bt CA00");
	THEN("bt CCAABBCCDDEE01");
	run(1500);
	phone_answers("Phone one", "AA:BB:CC:DD:EE:01");
	cp(3);
	cp(62);
	NOT("screen cover");	/* the waiting screen is the cover */
	cp(4);
	run(900);
	frames_decode();
	THEN("screen banner");
	picture("banner");
	CHECK(model()->phones[0].state == PHONE_CONNECTED && same(model()->chip_text, "Phone one") && model()->chip == CHIP_UP);
	/* a finger on CarPlay goes to CarPlay; one on the banner is ours */
	tap(640, 300);
	THEN("cp touch 1");
	THEN("cp touch 3");
}

/* The car's profile has a phone: it alone connects, by itself. */
static void s_profile(void)
{
	struct ui_display_stats d0, d;

	ui_display_stats(&d0);
	drive_with_profile();
	ui_display_stats(&d);
	CHECK(d.fades == d0.fades + 1);	/* Full: the cover faded away over CarPlay's picture, once */
	CHECK(model()->bt == UI_BT_OFF && model()->chip == CHIP_UP);	/* Bluetooth is off while CarPlay runs; the chip has its sign */
	CHECK(model()->profile_phone == 0 && model()->phones[0].profile == 2 && model()->phones[1].profile == 1);
	/* the banner goes after 8 s; nothing of ours is left over CarPlay */
	run(8200);
	THEN("screen clear");
}

/* The report comes after the 8 s wait: it still counts. */
static void s_profile_late(void)
{
	start_up();
	run(20000);
	THEN("bt P0");
	profile(0x11);
	THEN("bt P1");
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE01");
}

/* A profile with no phone: Home waits and says which. */
static void s_profile_without_phone(void)
{
	start_up();
	profile(0x22);
	run(1000);
	THEN("bt CD");
	THEN("bt P0");
	CHECK(same(model()->chip_text, "No phone for profile 3"));
	picture("home-no-phone-for-profile");
	run(60000);
	NOT("bt P1");
	NOT("bt CCAABBCCDDEE01");
	NOT("mcu 4c 00 00");
}

/* A profile without a phone: the phone the driver then connects becomes
 * that profile's phone, and is kept; the other profiles keep theirs. */
static void s_profile_takes_phone(void)
{
	state_kept = 1;
	start_up();
	state_kept = 0;
	profile(0x22);
	run(1000);
	CHECK(same(model()->chip_text, "No phone for profile 3"));
	cursor = 0;
	tap(640, 300);
	tap(TILE_X(1), TILE_Y);	/* "Phone two" */
	THEN("screen wait connecting");
	run(5000);
	bt("IS1");
	run(3000);
	THEN("bt CCAABBCCDDEE02");
	NOT("put /data/autotop-profiles");	/* not before it has connected */
	phone_answers("Phone two", "AA:BB:CC:DD:EE:02");
	cp(3);
	cp(62);
	cp(4);
	THEN("put /data/autotop-profiles 11 AA:BB:CC:DD:EE:01\n22 AA:BB:CC:DD:EE:02\naa -");
	CHECK(model()->phones[1].profile == 3 && model()->phones[0].profile == 2);
	CHECK(same(model()->chip_text, "Phone two"));
}

/* A profile that has its phone keeps it. */
static void s_profile_keeps_its_phone(void)
{
	state_kept = 1;
	drive_with_profile();
	state_kept = 0;
	NOT("put /data/autotop-profiles");	/* profile 2 has phone one: connecting it changes nothing */
	CHECK(model()->phones[0].profile == 2 && model()->phones[1].profile == 1);
}

/* The knob while CarPlay has the screen is CarPlay's: a wheel step, select
 * down and up, its Back key; the voice key is Siri. MENU stays the car's. */
static void s_knob_in_carplay(void)
{
	drive_with_profile();
	run(9000);	/* the banner is gone */
	cursor = 0;
	knob('r');
	THEN("cp requestWheelStatus 0 0 1");
	knob('l');
	THEN("cp requestWheelStatus 0 0 -1");
	knob('p');
	THEN("cp requestWheelStatus 0 1 0");
	THEN("cp requestWheelStatus 0 0 0");
	knob('b');
	THEN("cp requestKeyValue 3");
	mcu(0xD0, 5, 0x4B, 0x81, 0x00, 0x25, 0x00);
	run(10);
	THEN("cp requestKeyValue 4");
	NOT("screen list");	/* nothing of the launcher's moved */
	knob('m');
	THEN("screen cover");
	NOT("cp requestKeyValue");
}

/* The car takes its picture back over a running CarPlay (a long MENU, which
 * the MCU does by itself): the next touch brings the unit's picture, and is
 * not passed to the CarPlay that nobody can see; the one after is CarPlay's. */
static void s_car_picture_over_carplay(void)
{
	drive_with_profile();
	run(9000);
	cursor = 0;
	tap(640, 300);
	THEN("cp touch 1");	/* CarPlay has the screen: the touch is its own */
	mcu(0xDE, 2, 0x00, 0x01);
	run(100);
	cursor = 0;
	tap(640, 300);
	THEN("mcu 4c 00 00");
	NOT("cp touch");
	NOT("screen home");	/* CarPlay is behind it, not Home */
	mcu(0xDE, 2, 0x00, 0x00);
	run(100);
	cursor = 0;
	tap(640, 300);
	THEN("cp touch 1");
	knob('r');
	THEN("cp requestWheelStatus 0 0 1");
}

/* The car's time, about once a minute, is the clock; it runs on between. */
static void s_clock(void)
{
	nobody_drives();
	CHECK(same(model()->clock, ""));
	mcu(0xD0, 9, 0x10, 0x00, 0x00, 26, 11, 25, 19, 12, 30);
	run(100);
	CHECK(same(model()->clock, "19:12"));
	run(31000);
	CHECK(same(model()->clock, "19:13"));	/* it was half past the minute */
	mcu(0xD0, 9, 0x10, 0x00, 0x00, 26, 11, 25, 23, 59, 0);
	run(61000);
	CHECK(same(model()->clock, "00:00"));
	mcu(0xD0, 9, 0x10, 0x00, 0x00, 26, 11, 25, 99, 99, 0);	/* nonsense is not shown */
	run(100);
	CHECK(same(model()->clock, "00:00") || same(model()->clock, "00:01"));
}

/* "Connect at ignition" off (CONNECT_AT_IGNITION=0): the profile's phone waits too. */
static void s_connect_off(void)
{
	start_up();
	profile(0x11);
	run(1000);
	THEN("bt CD");
	THEN("bt P0");
	run(60000);
	NOT("bt CCAABBCCDDEE01");
	CHECK(same(model()->chip_text, ""));
}

/* Guest at ignition: no CarPlay by itself; what the guest chooses runs, with no banner. */
static void s_guest(void)
{
	start_up();
	profile(0xaa);
	run(1000);
	THEN("bt CD");
	THEN("bt P0");
	CHECK(same(model()->chip_text, "Guest") && model()->guest);
	picture("home-guest");
	run(30000);
	NOT("bt P1");
	tap(640, 300);
	THEN("mcu 4c 00 00");
	tap(TILE_X(1), TILE_Y);
	THEN("screen wait connecting");
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE02");
	phone_answers("Phone two", "AA:BB:CC:DD:EE:02");
	session_comes_up();
	THEN("screen clear");
	NOT("screen banner");	/* never under the Guest profile */
}

/* The profile changes to Guest while CarPlay runs: it ends, Home is there. */
static void s_guest_while_driving(void)
{
	drive_with_profile();
	profile(0xaa);
	THEN("cp requestLinkStatus 7 24");
	THEN("au releaseAudioSource 4");
	THEN("ap rotate");
	THEN("screen home");
	run(40000);
	NOT("bt P1");
	NOT("cp requestLinkStatus 7 3");
	/* and back to the owner's profile: his phone again, through "Connecting" */
	profile(0x11);
	THEN("bt P1");
}

/* The profile's phone is not there: no fuss, Home says so, and that one
 * phone is paged again every 30 s; nobody else may connect. */
static void s_unreachable_auto(void)
{
	start_up();
	profile(0x11);
	run(1500);
	THEN("bt CCAABBCCDDEE01");
	run(3000);
	bt("IA");	/* gocsdk's own time-out: the page ended without the phone */
	run(3000);
	THEN("bt CCAABBCCDDEE01");	/* the second of two attempts */
	run(3000);
	bt("IA");
	run(2000);
	CHECK(model()->phones[0].state == PHONE_UNREACHABLE);
	CHECK(same(screen(), "home"));
	picture("home-unreachable");
	NOT("screen wait failed");	/* nobody was watching */
	NOT("mcu 4c 00 00");
	NOT("bt P0");	/* Bluetooth stays on for the next page */
	bt("IB");	/* the other phone gets onto hands-free somehow: it gets no CarPlay */
	bt("SAPhone two");
	run(1000);
	NOT("cp requestLinkStatus 7 3");
	bt("IA");
	run(26000);
	NOT("bt CCAABBCCDDEE01");
	run(6000);
	THEN("bt CCAABBCCDDEE01");	/* 30 s later: paged again */
	CHECK(model()->phones[0].state != PHONE_UNREACHABLE);	/* cleared at the next attempt */
	run(1500);
	phone_answers("Phone one", "AA:BB:CC:DD:EE:01");	/* now it is there */
	session_comes_up();
	THEN("screen banner");
}

/* A phone the driver chose is not there: "Still trying", "Couldn't connect"
 * for 3 s, then Home with it "Unreachable"; nothing connects afterwards. */
static void s_unreachable_chosen(void)
{
	nobody_drives();
	tap(TILE_X(1), TILE_Y);
	THEN("screen wait connecting");
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE02");
	run(3000);
	bt("IA");
	run(3000);
	THEN("screen wait retrying");
	picture("still-trying");
	THEN("bt CCAABBCCDDEE02");
	run(3000);
	bt("IA");
	run(1500);
	THEN("bt P0");	/* Bluetooth restarted before the last of three attempts */
	run(3000);
	THEN("bt P1");
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE02");
	run(3000);
	bt("IA");
	run(1200);
	THEN("bt CD");	/* Bluetooth off: the driver's choice failed, and nothing else connects */
	THEN("screen wait failed");
	picture("couldnt-connect");
	run(1500);
	THEN("bt P0");
	NOT("screen home");
	run(1500);
	THEN("screen home");	/* after 3 s */
	CHECK(st().focus == 1 && model()->phones[1].state == PHONE_UNREACHABLE);
	run(60000);
	NOT("bt P1");
	NOT("cp requestLinkStatus 7 3");
}

/* Back while connecting: stop, Home, and nothing connects by itself afterwards. */
static void s_cancel(void)
{
	drive_with_profile();
	run(8200);
	tap(640, 300);
	cp(63);	/* (the driver left CarPlay by its car icon) */
	tap(640, 300);
	THEN("screen home");
	tap(TILE_X(1), TILE_Y);
	THEN("screen wait connecting");
	THEN("cp requestLinkStatus 7 24");
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE02");
	knob('b');
	THEN("bt CD");
	THEN("screen home");
	CHECK(st().focus == 1);
	run(1000);
	THEN("bt P0");
	run(60000);
	NOT("bt P1");	/* not even the profile's phone: the driver's stop stands */
	NOT("cp requestLinkStatus 7 3");
}

/* The banner's pill: the other phone, as a switch always went. */
static void s_switch(void)
{
	drive_with_profile();
	tap(PILL_X, PILL_Y);
	THEN("screen wait connecting");	/* the cover first, drawn at once */
	THEN("cp requestLinkStatus 7 24");	/* full stop of the old session */
	THEN("ap rotate");	/* the phone left behind cannot rejoin the Wi-Fi */
	THEN("bt P1");
	run(5000);
	NOT("bt CCAABBCCDDEE02");	/* nothing is paged before Bluetooth is up */
	bt("IS1");
	cp(5);	/* the old session's own down statuses, within 3 s of the stop */
	cp(8);
	run(1000);
	NOT("screen home");
	THEN("bt CA00");	/* the link policy first */
	THEN("bt MH");
	THEN("bt MQ");
	run(600);
	THEN("bt CCAABBCCDDEE02");	/* 1.2 s after IS1: behind three commands 400 ms apart */
	run(2000);
	phone_answers("Phone two", "AA:BB:CC:DD:EE:02");
	cp(3);
	cp(62);
	NOT("screen cover");	/* "Connecting" stays as the cover */
	cp(4);
	run(900);
	THEN("bt CD");
	THEN("bt P0");
	frames_decode();
	THEN("screen banner");
	CHECK(model()->phones[1].state == PHONE_CONNECTED);
	/* the driver's choice stands: the profile's phone is not paged behind his back */
	cp(8);	/* CarPlay drops */
	run(120000);
	NOT("bt CCAABBCCDDEE01");
}

/* gocsdk refuses a page ("connect pending"): Bluetooth restarted at once, once. */
static void s_refused_page(void)
{
	start_up();
	profile(0x11);
	run(1500);
	THEN("bt CCAABBCCDDEE01");
	bt("ER");
	run(100);
	THEN("bt P0");
	run(2500);
	THEN("bt P1");
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CA00");
	THEN("bt CCAABBCCDDEE01");
	bt("ER");	/* refused again: only one restart per seek */
	run(3000);
	NOT("bt P0");
}

/* Three failed starts on the profile's phone: quiet, Home "Unreachable". The
 * phone is there and CarPlay will not start: it is not asked again unless
 * the driver taps it. */
static void s_failed_starts(void)
{
	start_up();
	profile(0x11);
	run(3000);
	phone_answers("Phone one", "AA:BB:CC:DD:EE:01");
	run(50);
	CHECK(model()->bt == UI_BT_LINKED);	/* the mark has its lines: a phone is on Bluetooth */
	cp(8);	/* the start fails; the phone is still on hands-free */
	THEN("au releaseAudioSource 4");
	THEN("mcu 4c 00 01");	/* the car's picture, as it was */
	run(2900);
	NOT("cp requestLinkStatus 7 3");
	run(300);
	THEN("cp requestLinkStatus 7 3");	/* retried after 3 s */
	cp(8);
	run(3200);
	THEN("cp requestLinkStatus 7 3");
	cp(8);	/* the third failure: given up */
	run(1000);
	THEN("bt CD");
	THEN("bt P0");
	CHECK(same(screen(), "home") && model()->phones[0].state == PHONE_UNREACHABLE);
	NOT("bt CA01");	/* no phone may connect by itself */
	run(120000);
	NOT("cp requestLinkStatus 7 3");
	NOT("bt P1");
}

/* CarPlay's car icon: the BMW picture, CarPlay alive behind. A touch brings
 * Home; the connected phone's tile brings CarPlay back. */
static void s_car_icon(void)
{
	drive_with_profile();
	run(8200);
	cp(63);
	THEN("screen cover");
	THEN("mcu 4c 00 01");
	tap(640, 300);
	THEN("screen home");
	THEN("mcu 4c 00 00");
	CHECK(st().focus == 0 && model()->phones[0].state == PHONE_CONNECTED);
	picture("home-connected");
	tap(TILE_X(0), TILE_Y);
	THEN("screen cover");
	THEN("cp requestLinkStatus 7 3");
	cp(4);
	cp(62);
	/* A session coming back: two frames, not the twelve of a start. A
	 * still screen sent 4 in 2.5 s at the bench (2026-11-22), and the
	 * cover stayed 2.8 s. */
	run(300);
	NOT("screen clear");	/* nothing decoded yet: the layer shows the picture of before */
	frames += 2;
	run(60);
	THEN("screen clear");
	NOT("screen banner");	/* the banner is for a session that has just come up */

	/* And if no frame comes at all, the cover goes after 1.5 s. */
	cp(63);
	THEN("screen cover");
	tap(640, 300);
	THEN("screen home");
	tap(TILE_X(0), TILE_Y);
	THEN("screen cover");
	cp(62);
	run(1300);
	NOT("screen clear");
	run(300);
	THEN("screen clear");
}

/* Short MENU in CarPlay: the BMW picture too. The MCU's own switch back (a
 * long MENU sends no key) returns to CarPlay. The phone is not told that
 * it lost the screen, so on the way back nothing is waited for. */
static void s_menu(void)
{
	drive_with_profile();
	run(8200);
	knob('r');	/* the knob in CarPlay is CarPlay's: nothing of ours moves */
	NOT("screen home");
	knob('m');
	THEN("screen cover");
	THEN("mcu 4c 00 01");
	tap(640, 300);	/* a touch there brings Home, not a press into CarPlay */
	THEN("screen home");
	THEN("mcu 4c 00 00");
	NOT("cp touch 1");
	/* Back by the phone's tile. No status comes (the phone kept the
	 * screen), and none is waited for: 5.0 s of black at the bench on
	 * 2026-11-22, a fallback timer. */
	tap(TILE_X(0), TILE_Y);
	THEN("cp requestLinkStatus 7 3");
	THEN("screen clear");
	NOT("screen banner");

	knob('m');
	THEN("screen cover");
	THEN("mcu 4c 00 01");
	tap(640, 300);
	THEN("screen home");
	THEN("mcu 4c 00 00");
	knob('m');	/* MENU on a launcher screen: the car's picture again */
	THEN("screen cover");
	THEN("mcu 4c 00 01");
	mcu(0xDE, 2, 0x00, 0x00);	/* the MCU switched the picture to the unit by itself */
	THEN("cp requestLinkStatus 7 3");	/* CarPlay to the front */
	NOT("mcu 4c 00 00");
	run(60);
	THEN("screen clear");
}

/* The knob in the launcher; the settings that work, and those that are switched off. */
static void s_knob(void)
{
	nobody_drives();
	CHECK(same(screen(), "home") && st().focus == 0);
	knobs("r");
	CHECK(st().focus == 1);
	knobs("rr");	/* past "Pair a phone": the cog, at the right end of the top band */
	CHECK(st().focus == UI_FOCUS_COG);
	knobs("p");
	THEN("screen list Settings");
	picture("settings");
	CHECK(!st().list->rows[2].disabled);	/* Sound: the launcher's own sounds are in it */
	knobs("pw");
	THEN("screen list General");
	CHECK(st().list->rows[0].disabled && st().list->rows[1].disabled && st().focus == 2);	/* Name, Connect at ignition */
	picture("general");
	knobs("b" "r" "pw");
	THEN("screen list Display");
	knobs("p" "r" "p");	/* Brightness: adjust, one step up, keep */
	THEN("mcu 61 40");	/* 65 % of the MCU's 0-99 */
	knobs("r" "p");	/* Steering side: Back and the cog move at once */
	CHECK(!ui_frame_rhd() && !model()->set[SET_STEERING_RIGHT]);
	knobs("p");
	CHECK(ui_frame_rhd());
	knobs("b" "rr" "pw" "r" "pw");	/* past Sound to System, then Reboot: "Cancel" has the ring */
	THEN("screen list Reboot");
	NOT("reboot");
	knobs("r" "p");
	THEN("reboot");
	knobs("m");	/* MENU: the car's picture; no session behind, so Home stays ready */
	THEN("mcu 4c 00 01");
	CHECK(ui_frame_layer_now() == LAYER_SCREEN);
	knobs("r");	/* the knob is the car's again */
	tap(640, 300);
	THEN("mcu 4c 00 00");
}

/* Pairing: the line, the new phone straight into CarPlay; and Back. */
static void s_pairing(void)
{
	nobody_drives();
	tap(TILE_X(2), TILE_Y);	/* "Pair a phone" */
	THEN("bt P1");
	THEN("screen wait pairing");
	picture("pairing");
	run(5000);
	bt("IS1");
	run(500);
	THEN("bt CA11");	/* visible and connectable */
	paired("2,0,1,");	/* gocsdk has a third phone, and counts it first */
	run(1100);
	THEN("screen wait connecting");	/* a phone just paired goes straight into CarPlay */
	THEN("ap rotate");
	NOT("state save");
	run(3000);
	THEN("state save");	/* the new pairing is kept, 3 s after it was made */
	NOT("bt CA00");	/* the pairing policy stays 6 s: the new phone connects by itself */
	NOT("bt CCAABBCCDDEE03");
	phone_answers("Phone three", "AA:BB:CC:DD:EE:03");
	session_comes_up();
	THEN("screen banner");
	run(3500);
	THEN("state save");	/* and again with its first session up: carplay has its keys now */
	run(60000);
	NOT("state save");
}

static void s_pairing_cancel(void)
{
	nobody_drives();
	tap(TILE_X(2), TILE_Y);
	THEN("screen wait pairing");
	run(5000);
	bt("IS1");
	run(500);
	THEN("bt CA11");
	tap(CANCEL_X, CANCEL_Y);	/* "Cancel" under the line */
	THEN("screen home");
	CHECK(st().focus == 2);	/* the ring back on "Pair a phone" */
	run(1500);
	THEN("bt CD");	/* Bluetooth off again: nothing connects by itself */
	THEN("bt P0");
	NOT("bt CB");
	NOT("bt CA01");
}

/* Disconnect, in a phone's list: CarPlay ends at once, nothing reconnects. */
static void s_disconnect(void)
{
	drive_with_profile();
	run(8200);
	cp(63);
	tap(640, 300);
	THEN("screen home");
	knobs("rrr" "p");	/* past two phones and "Pair a phone" to the cog */
	THEN("screen list Settings");
	knobs("pw" "r" "pw" "pw");	/* General, Paired phones, the connected phone */
	THEN("screen list Phone one");
	CHECK(same(st().list->rows[1].label, "Disconnect") && st().focus == 1);
	CHECK(st().list->rows[2].disabled && st().list->rows[3].disabled);	/* profile, forget: not wired */
	picture("phone");
	knobs("p");
	THEN("cp requestLinkStatus 7 24");
	THEN("au releaseAudioSource 4");
	THEN("ap rotate");
	CHECK(same(st().list->rows[1].label, "Connect") && same(st().list->rows[0].value, "Last used"));
	run(60000);
	NOT("bt P1");
	NOT("cp requestLinkStatus 7 3");
}

/* The car's lights; a picture source the MCU changed is set again. */
static void s_night(void)
{
	drive_with_profile();
	mcu(0x8D, 1, 0x01);
	THEN("cp requestLinkStatus 7 31");
	mcu(0x8D, 1, 0x01);
	NOT("cp requestLinkStatus 7 31");
	mcu(0x8D, 1, 0x00);
	THEN("cp requestLinkStatus 7 32");
}

/* The unit's name (bench only until it can be kept): one name for
 * Bluetooth, the access point and CarPlay's list, and the phones stay paired. */
static void s_rename(void)
{
	setenv("BENCH", "1", 1);
	nobody_drives();	/* Home is up, the ring on the first phone */
	knobs("rrr" "p");	/* past the tiles to the cog: Settings */
	THEN("screen list Settings");
	knobs("pw");
	THEN("screen list General");
	CHECK(!st().list->rows[0].disabled && st().focus == 0 && same(st().list->rows[0].value, "TESTKIT"));
	knobs("pw");	/* "Name": the dial, on "A" */
	knobs("p" "l" "p");	/* type it, one step back to "OK", save */
	THEN("screen list General");
	CHECK(same(st().list->rows[0].value, "TESTKITA") && same(model()->name, "TESTKITA"));
	/* Bluetooth is off while nobody is being connected: its name waits. */
	NOT("bt MMTESTKITA");
	THEN("put /data/autotop-name TESTKITA");	/* kept for the next start, and for carplay's own */
	THEN("ap name TESTKITA");
	THEN("ap rotate");	/* hostapd reads its file again: nobody is on it */
	NOT("cp requestLinkStatus 7 24");	/* no carplay is running: nothing to stop */
	NOT("state save");
	run(3500);
	THEN("state save");	/* a few seconds after the change */
	knobs("rrrr" "p" "rrrr" "p");	/* "Back" twice: Home */
	THEN("screen home");
	tap(TILE_X(0), TILE_Y);	/* the phone is still paired: it is paged as before */
	THEN("screen wait connecting");
	THEN("bt P1");
	run(5000);
	bt("IS1");
	run(1500);
	THEN("bt MMTESTKITA");	/* Bluetooth is on: now it has the name */
	run(1500);
	THEN("bt CCAABBCCDDEE01");	/* and the phone is paged as ever */
	run(1500);
	phone_answers("Phone one", "AA:BB:CC:DD:EE:01");
	cursor = 0;
	NOT("CARPLAY_NAME");	/* carplay takes its names when its process starts, not from a call */
	unsetenv("BENCH");
}

/* Renamed while a session runs behind Home: the session is left alone, and
 * the carplay that has the old name is ended just before the next start. */
static void s_rename_in_session(void)
{
	setenv("BENCH", "1", 1);
	drive_with_profile();	/* phone one in CarPlay */
	run(8200);
	cp(63);	/* the car's icon */
	tap(640, 300);
	THEN("screen home");
	knobs("rrr" "p");
	THEN("screen list Settings");
	knobs("pw");
	knobs("pw");
	knobs("p" "l" "p");
	THEN("put /data/autotop-name TESTKITA");
	NOT("carplay end");	/* a session is running: not now */
	NOT("ap rotate");	/* nor is its Wi-Fi cut */
	knobs("rrrr" "p" "rrrr" "p");
	THEN("screen home");
	tap(TILE_X(1), TILE_Y);	/* the other phone */
	THEN("cp requestLinkStatus 7 24");
	NOT("carplay end");	/* not yet: the old session is still coming down */
	run(5000);
	bt("IS1");
	cp(5);
	cp(8);
	run(2500);
	THEN("bt CCAABBCCDDEE02");
	run(1500);
	bt("IB");
	bt("JHaabbccddee02");
	bt("SAPhone two");
	THEN("carplay end");	/* before the calls that start the next session */
	THEN("cp requsetSetEnvValue 7 CARPLAY_RIGHTHAND_DRIVER 1");
	THEN("cp requestLinkStatus 7 3");
	unsetenv("BENCH");
}

/* A phone the unit already lists is paired again (its owner had forgotten
 * the car on it): the list does not grow, the key file changes. */
static void s_pairing_again(void)
{
	put("/etc/goc/sc_db.db", "keys-before\n");
	nobody_drives();
	tap(TILE_X(2), TILE_Y);	/* "Pair a phone" */
	THEN("screen wait pairing");
	run(5000);
	bt("IS1");
	run(500);
	THEN("bt CA11");
	/* A known phone that only comes back by itself is left alone. */
	bt("IB");
	bt("JHaabbccddee01");
	bt("SAPhone one");
	run(2500);
	CHECK(same(screen(), "wait pairing"));
	NOT("cp requestLinkStatus 7 3");
	bt("IA");
	run(1000);
	/* Phone two is paired anew: gocsdk writes its keys, then it is on hands-free. */
	put("/etc/goc/sc_db.db", "keys-after\n");
	bt("IB");
	bt("JHaabbccddee02");
	bt("SAPhone two");
	run(1200);
	THEN("screen wait connecting");	/* the pairing screen does not stay */
	THEN("cp requestSetPhoneBTAddr 7 AA:BB:CC:DD:EE:02 1");	/* it is on hands-free already: CarPlay on it */
	THEN("cp requestLinkStatus 7 3");
	run(3200);
	THEN("state save");	/* and the new keys are kept */
}

/* Forget, in a phone's list, in two steps. The phone is off the screen at
 * once; gocsdk is told when Bluetooth is on and has listed its phones, by the
 * phone's place in gocsdk's own count; then its profile is free and the
 * state is saved. */
static void forget_from_list(const char *knobs_to_phone, const char *title)
{
	char screen_[80];

	knobs("rrr" "p");
	THEN("screen list Settings");
	knobs(knobs_to_phone);
	snprintf(screen_, sizeof(screen_), "screen list %s", title);
	THEN(screen_);
	CHECK(same(st().list->rows[3].label, "Forget this phone") && !st().list->rows[3].disabled);
	knobs("rr" "pw");	/* past "Driver profile" */
	snprintf(screen_, sizeof(screen_), "screen list Forget %s", title);
	THEN(screen_);
	knobs("r" "p");	/* past "Cancel": "Forget now" */
}

static void s_forget(void)
{
	setenv("BENCH", "1", 1);
	paired("1,0,");	/* phone two is gocsdk's first; slot 2 holds a phone dropped earlier */
	nobody_drives();
	CHECK(model()->n == 2);	/* the dropped slot is not a phone */
	forget_from_list("pw" "rr" "pw" "r" "pw", "Phone two");
	CHECK(model()->n == 1 && same(model()->phones[0].name, "Phone one"));	/* gone at once */
	THEN("bt P1");	/* gocsdk hears nothing while Bluetooth is off */
	run(5000);
	NOT("bt CV1");
	bt_up("1,0,");
	run(1500);	/* behind the commands that follow IS1 */
	THEN("bt CV1");	/* its place in gocsdk's count, not its slot */
	NOT("bt CV2");
	paired("0,");	/* gocsdk drops it; slot 1 keeps its lines */
	run(2500);
	THEN("sound off");
	CHECK(!st().message[0]);	/* nothing to say: the row has closed */
	THEN("put /data/autotop-profiles 11 AA:BB:CC:DD:EE:01\naa -");	/* its profile is free */
	THEN("bt CD");	/* Bluetooth off again */
	THEN("bt P0");
	CHECK(model()->n == 1);
	NOT("state save");
	run(3000);
	THEN("state save");
	/* The car reports the profile that was the forgotten phone's: no phone for it. */
	profile(0x00);
	run(1000);
	NOT("bt CCAABBCCDDEE02");
	NOT("bt CCAABBCCDDEE01");
	unsetenv("BENCH");
}

/* gocsdk's listing and its file do not agree on the phone's place (a phone
 * connected in between, say): nothing is dropped, and the phone is back. */
static void s_forget_unclear(void)
{
	setenv("BENCH", "1", 1);
	paired("1,0,");
	nobody_drives();
	forget_from_list("pw" "rr" "pw" "r" "pw", "Phone two");
	CHECK(model()->n == 1);
	run(5000);
	bt_up("0,1,");	/* by its listing phone two is the second, by its file the first */
	run(4000);
	NOT("bt CV1");
	NOT("bt CV2");
	NOT("put /data/autotop-profiles");
	CHECK(model()->n == 2 && same(model()->phones[1].name, "Phone two"));
	CHECK(same(st().message, "Couldn\xE2\x80\x99t forget Phone two") && ui_frame_message_shown());
	THEN("sound deny");
	run(5000);
	CHECK(!ui_frame_message_shown());	/* three seconds */
	NOT("state save");
	profile(0x00);	/* its profile still brings it up */
	run(6000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE02");
	unsetenv("BENCH");
}

/* No listing comes from gocsdk: after a while, by its file alone. */
static void s_forget_unlisted(void)
{
	setenv("BENCH", "1", 1);
	paired("1,0,");
	nobody_drives();
	forget_from_list("pw" "rr" "pw" "r" "pw", "Phone two");
	run(5000);
	bt("IS1");
	run(3000);
	NOT("bt CV1");
	run(1500);
	THEN("bt CV1");
	paired("0,");
	run(2500);
	CHECK(model()->n == 1 && same(model()->phones[0].name, "Phone one"));
	unsetenv("BENCH");
}

/* A phone is paired while another one's CarPlay session runs behind Home,
 * and gocsdk gives it the free slot before that one (bench, 2026-11-22: the
 * launcher took the new phone for the connected one, brought the old session
 * to the front and called it by the new phone's name). */
static void s_pairing_before_connected(void)
{
	/* One phone, in slot 1; slot 0 was dropped earlier and keeps its lines. */
	paired("1,");
	nobody_drives();
	CHECK(model()->n == 1 && same(model()->phones[0].name, "Phone two"));
	tap(TILE_X(1), TILE_Y);	/* two tiles: the phone's, then "Pair a phone" */
	THEN("screen wait connecting");
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE02");
	run(1500);
	phone_answers("Phone two", "AA:BB:CC:DD:EE:02");
	session_comes_up();	/* no banner: there is no other phone to offer */
	run(8200);
	cp(63);	/* the car's icon: Home, the session stays */
	tap(640, 300);
	THEN("screen home");
	CHECK(model()->phones[0].state == PHONE_CONNECTED);
	tap(TILE_X(2), TILE_Y);	/* "Pair a phone" */
	THEN("screen wait pairing");
	run(5000);
	bt("IS1");
	run(500);
	THEN("bt CA11");
	/* Phone one is paired: gocsdk puts it in slot 0, before phone two. */
	paired("0,1,");
	run(1100);
	THEN("screen wait connecting");
	THEN("cp requestLinkStatus 7 24");	/* a real switch: phone two's session is ended */
	cp(5);
	cp(8);
	run(1000);
	phone_answers("Phone one", "AA:BB:CC:DD:EE:01");	/* CarPlay starts on the new phone */
	cp(3);
	cp(62);
	cp(4);
	run(900);
	frames_decode();
	CHECK(model()->n == 2 && same(model()->phones[0].name, "Phone one") && same(model()->phones[1].name, "Phone two"));
	CHECK(model()->phones[0].state == PHONE_CONNECTED && model()->phones[1].state != PHONE_CONNECTED);
}

/* A list with a dropped slot in the middle (slot 1), the phone in slot 2
 * gocsdk's first: only the paired slots are phones, and Forget counts as
 * gocsdk does. */
static void s_forget_order(void)
{
	setenv("BENCH", "1", 1);
	paired("2,0,");
	nobody_drives();
	CHECK(model()->n == 2 && same(model()->phones[0].name, "Phone one") && same(model()->phones[1].name, "Phone three"));
	forget_from_list("pw" "rr" "pw" "pw", "Phone one");
	CHECK(model()->n == 1 && same(model()->phones[0].name, "Phone three"));
	run(5000);
	bt_up("2,0,");
	run(1500);
	THEN("bt CV2");	/* phone one is gocsdk's second */
	NOT("bt CV1");
	paired("2,");
	run(2500);
	CHECK(model()->n == 1 && same(model()->phones[0].name, "Phone three"));
	/* Phone three is paged by its address, as ever. */
	knobs("b" "b" "b");	/* Paired phones, General, Settings: Home */
	THEN("screen home");
	tap(TILE_X(1), TILE_Y);	/* two tiles now: the phone's is the first of them */
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE03");
	unsetenv("BENCH");
}

/* The profile mapping kept by address stands in place of PROFILE_PHONES. */
static void s_kept_profiles(void)
{
	put("/data/autotop-profiles", "11 AA:BB:CC:DD:EE:02\naa -\n");	/* profile 11 is phone two's now */
	start_up();
	profile(0x11);
	run(6000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE02");
	NOT("bt CCAABBCCDDEE01");
}

/* An app update over the unit's Wi-Fi: "Start" begins listening and shows
 * how to join; the receiver's line is the screen's; a refusal leaves it
 * listening; a bundle that is in restarts the unit; "Cancel" and a screen
 * that goes away end the listening. */
static void s_update(void)
{
	state_kept = 1;
	nobody_drives();
	state_kept = 0;
	cursor = 0;
	CHECK(model()->can_update);
	knobs("rrr" "p" "rrr" "pw" "pw");
	CHECK(same(screen(), "list System info"));
	knobs("rrr" "pw");	/* "Software update": its instruction page */
	NOT("update start");	/* nothing listens before "Start" */
	knobs("r" "p");
	THEN("update start");
	CHECK(same(screen(), "receive") && ui_nav_receive_step() == 1);	/* how to join the Wi-Fi */
	run(4500);
	CHECK(ui_nav_receive_step() == 1);	/* nobody has joined */
	ap_now = 1;
	run(2500);
	CHECK(ui_nav_receive_step() == 2);	/* a phone joined: the address, by itself */
	update_status = "receiving 500000 of 1000000";
	run(500);
	update_status = "refused app: refused: the signature is not ours";
	run(500);
	CHECK(same(screen(), "receive"));	/* still listening: another file may follow */
	NOT("update stop");
	knobs("b");	/* "Cancel" */
	THEN("update stop");
	CHECK(same(screen(), "list System info"));
	update_status = "waiting";	/* a new receiver starts with that */
	knobs("pw" "r" "p");	/* again, the phone still on the Wi-Fi: a file that comes brings step 2 too */
	THEN("update start");
	CHECK(ui_nav_receive_step() == 1);
	update_status = "receiving 100 of 1000000";
	run(500);
	CHECK(ui_nav_receive_step() == 2);
	update_status = "done app: version 7 kept in slot a: it runs from the next start";
	run(500);
	CHECK(same(screen(), "wait updating"));
	NOT("reboot");
	run(3000);
	THEN("reboot");
	update_status = NULL;
	ap_now = 0;
}

/* A minute of running well ends an update's trial. */
static void s_app_confirm(void)
{
	nobody_drives();
	cursor = 0;
	run(40000);
	NOT("app confirm");
	run(25000);
	THEN("app confirm");
	run(120000);
	NOT("app confirm");	/* once */
}

/* Developer access: an image without a developer's key has no such row;
 * with one, System has it between "System info" and "Reboot", showing what
 * the system says, and a push tells the system's script, at once. */
static void s_developer(void)
{
	nobody_drives();
	cursor = 0;
	CHECK(!model()->can_develop);
	knobs("rrr" "p" "rrr" "pw");
	CHECK(same(screen(), "list System"));
	knobs("r" "pw");
	CHECK(same(screen(), "list Reboot"));	/* the second row is "Reboot": no developer row */
	NOT("developer access on");
}

static void s_developer_key(void)
{
	dev_state = 1;	/* a key in the image, access off */
	nobody_drives();
	dev_state = 0;
	cursor = 0;
	CHECK(model()->can_develop && !model()->set[SET_DEVELOPER]);
	knobs("rrr" "p" "rrr" "pw");
	CHECK(same(screen(), "list System"));
	knobs("r" "p");
	THEN("developer access on");
	THEN("ap rotate");	/* the passphrase that stays from now on, at once: nobody is in CarPlay */
	CHECK(model()->set[SET_DEVELOPER] == 1 && same(screen(), "list System"));
	knobs("p");
	THEN("developer access off");
	THEN("ap rotate");	/* and a new one: the one that stayed is dead */
	knobs("r" "pw");
	CHECK(same(screen(), "list Reboot"));	/* "Reboot" follows it */
}

static void s_developer_on(void)
{
	dev_state = 3;	/* switched on at an earlier start */
	nobody_drives();
	dev_state = 0;
	CHECK(model()->can_develop && model()->set[SET_DEVELOPER] == 1);
	NOT("developer access on");	/* the system started it itself: nothing to tell */
}

/* carplay's process dies under a running session and reports nothing: the
 * launcher sees it within a few seconds and is back on Home, not behind a
 * frozen picture. */
static void s_carplay_gone(void)
{
	drive_with_profile();
	run(9000);
	THEN("screen clear");
	carplay_alive = 0;
	run(3500);
	THEN("au releaseAudioSource 4");
	CHECK(same(screen(), "home"));
}

/* Launcher sounds (design/feedback.md): "Actions" as shipped, without the
 * per-detent tick; the level is the driver's and is kept; the link's own
 * sounds; none while CarPlay has the screen. */
static void s_sounds(void)
{
	nobody_drives();
	cursor = 0;
	NOT("sound connected");
	knobs("r");
	NOT("sound tick");	/* Actions: a detent is silent */
	knobs("rr" "p");	/* the cog */
	THEN("sound select");
	THEN("screen list Settings");
	knobs("b");
	THEN("sound back");
	CHECK(same(screen(), "home"));
	knobs("p" "rr" "pw");	/* Settings, Sound */
	THEN("screen list Sound");
	CHECK(same(st().list->rows[st().focus].label, "Launcher sounds") && same(st().list->rows[st().focus].value, "Actions"));
	knobs("p");	/* the next level: All */
	CHECK(model()->set[SET_SOUNDS] == 2);
	THEN("put /data/autotop-settings sounds 2\neffects 1");	/* Full is the default */
	knobs("r");
	THEN("sound tick");	/* All: detents tick */
	run(3500);
	THEN("state save");	/* the level is kept */
	knobs("l" "p");	/* on to Off */
	CHECK(model()->set[SET_SOUNDS] == 0);
	knobs("b");
	NOT("sound back");
	knobs("pw" "p");	/* Sound again, and on to Actions */
	CHECK(model()->set[SET_SOUNDS] == 1);
	knobs("b" "b");
	CHECK(same(screen(), "home"));
	/* A phone comes up while the driver watches: the chime, under the cover. */
	tap(TILE_X(0), TILE_Y);
	THEN("screen wait connecting");
	run(5000);
	bt("IS1");
	run(2000);
	phone_answers("Phone one", "AA:BB:CC:DD:EE:01");
	cp(3);
	cp(62);
	cp(4);
	THEN("sound connected");
	run(900);
	frames_decode();	/* CarPlay has the screen */
	cursor = n_did;
	tap(PILL_X, PILL_Y);	/* the banner's switch: over CarPlay, so silent */
	NOT("sound select");
}

/* The level chosen earlier is the level at the next start. */
static void s_sounds_kept(void)
{
	put("/data/autotop-settings", "sounds 2\neffects 0\n");
	nobody_drives();
	CHECK(model()->set[SET_SOUNDS] == 2 && model()->set[SET_EFFECTS] == 0);	/* the driver's Lite stands against the default */
	cursor = 0;
	knobs("r");
	THEN("sound tick");
}

/* A name kept from an earlier run: it is the unit's from the start, for
 * Bluetooth when that is on and for the access point at once. */
static void s_stored_name(void)
{
	put("/data/autotop-name", "KEPT NAME\n");
	start_name = "KEPT NAME";	/* gocsdk is set up with it */
	nobody_drives();
	CHECK(same(model()->name, "KEPT NAME"));
	cursor = 0;
	THEN("ap name KEPT NAME");
	THEN("ap rotate");
	NOT("put /data/autotop-name");	/* nothing was changed: nothing is written */
	run(10000);
	NOT("state save");
}

/* Two phones with one name (bench, 2026-11-22: the owner's second phone and
 * a third one): the phone on hands-free is known by its address. */
static void s_same_name(void)
{
	put("/etc/goc/bt_conf.ini",
	    "pairlisttotal=3\n" PHONE1 PHONE2 "name2=Phone two\naddr2=aabbccddee03\n" "lastaddr=aabbccddee01\n");
	nobody_drives();
	tap(TILE_X(0), TILE_Y);	/* four tiles now: the second phone's */
	THEN("screen wait connecting");
	run(5000);
	bt("IS1");
	run(2000);
	THEN("bt CCAABBCCDDEE02");
	run(2000);
	phone_answers("Phone two", "AA:BB:CC:DD:EE:02");	/* CarPlay on it: not taken for its namesake */
	NOT("bt CD");
	session_comes_up();
	THEN("screen banner");
	CHECK(model()->phones[1].state == PHONE_CONNECTED && model()->phones[2].state != PHONE_CONNECTED);
}

/* Another phone answers each page: dropped, and the count of tries holds. */
static void s_wrong_phone(void)
{
	paired("0,1,2,");
	nobody_drives();
	tap(TILE_X(0), TILE_Y);
	THEN("screen wait connecting");
	run(5000);
	bt("IS1");
	run(2000);
	for (int i = 0; i < 3; i++) {
		THEN("bt CCAABBCCDDEE02");
		run(2000);
		bt("IB");
		bt("JHaabbccddee03");
		bt("SAPhone three");
		THEN("bt CD");
		NOT("cp requestLinkStatus 7 3");	/* no CarPlay for a phone nobody asked for */
		run(300);
		bt("IA");
		run(1500);
	}
	NOT("bt CCAABBCCDDEE02");	/* three tries, not a fourth */
	THEN("screen wait failed");
	run(3500);
	THEN("screen home");
	CHECK(model()->phones[1].state == PHONE_UNREACHABLE);
}

static const struct {
	const char *name;
	void (*run)(void);
	const char *profiles;	/* PROFILE_PHONES, as PLANA.CFG gives it */
	const char *connect;	/* CONNECT_AT_IGNITION */
} scenarios[] = {
	{ "no-profile", s_no_profile, "11:1 00:2 aa:0", NULL },
	{ "profile", s_profile, "11:1 00:2 aa:0", NULL },
	{ "profile-late", s_profile_late, "11:1 00:2 aa:0", NULL },
	{ "profile-without-phone", s_profile_without_phone, "11:1 00:2 aa:0", NULL },
	{ "connect-off", s_connect_off, "11:1 00:2 aa:0", "0" },
	{ "guest", s_guest, "11:1 00:2 aa:0", NULL },
	{ "guest-while-driving", s_guest_while_driving, "11:1 00:2 aa:0", NULL },
	{ "unreachable-auto", s_unreachable_auto, "11:1 00:2 aa:0", NULL },
	{ "unreachable-chosen", s_unreachable_chosen, "11:1 00:2 aa:0", NULL },
	{ "cancel", s_cancel, "11:1 00:2 aa:0", NULL },
	{ "switch", s_switch, "11:1 00:2 aa:0", NULL },
	{ "refused-page", s_refused_page, "11:1 00:2 aa:0", NULL },
	{ "failed-starts", s_failed_starts, "11:1 00:2 aa:0", NULL },
	{ "car-icon", s_car_icon, "11:1 00:2 aa:0", NULL },
	{ "menu", s_menu, "11:1 00:2 aa:0", NULL },
	{ "knob", s_knob, "11:1 00:2 aa:0", NULL },
	{ "pairing", s_pairing, "11:1 00:2 aa:0", NULL },
	{ "pairing-cancel", s_pairing_cancel, "11:1 00:2 aa:0", NULL },
	{ "disconnect", s_disconnect, "11:1 00:2 aa:0", NULL },
	{ "night", s_night, "11:1 00:2 aa:0", NULL },
	{ "rename", s_rename, "11:1 00:2 aa:0", NULL },
	{ "rename-in-session", s_rename_in_session, "11:1 00:2 aa:0", NULL },
	{ "pairing-again", s_pairing_again, "11:1 00:2 aa:0", NULL },
	{ "forget", s_forget, "11:1 00:2 aa:0", NULL },
	{ "forget-unclear", s_forget_unclear, "11:1 00:2 aa:0", NULL },
	{ "forget-unlisted", s_forget_unlisted, "11:1 00:2 aa:0", NULL },
	{ "pairing-before-connected", s_pairing_before_connected, "11:1 00:2 aa:0", NULL },
	{ "forget-order", s_forget_order, "11:1 00:2 aa:0", NULL },
	{ "kept-profiles", s_kept_profiles, "11:1 00:2 aa:0", NULL },
	{ "carplay-gone", s_carplay_gone, "11:1 00:2 aa:0", NULL },
	{ "update", s_update, "11:1 00:2 aa:0", NULL },
	{ "app-confirm", s_app_confirm, "11:1 00:2 aa:0", NULL },
	{ "profile-takes-phone", s_profile_takes_phone, "11:1 00:2 aa:0", NULL },
	{ "profile-keeps-its-phone", s_profile_keeps_its_phone, "11:1 00:2 aa:0", NULL },
	{ "knob-in-carplay", s_knob_in_carplay, "11:1 00:2 aa:0", NULL },
	{ "car-picture-over-carplay", s_car_picture_over_carplay, "11:1 00:2 aa:0", NULL },
	{ "clock", s_clock, "11:1 00:2 aa:0", NULL },
	{ "developer", s_developer, "11:1 00:2 aa:0", NULL },
	{ "developer-key", s_developer_key, "11:1 00:2 aa:0", NULL },
	{ "developer-on", s_developer_on, "11:1 00:2 aa:0", NULL },
	{ "sounds", s_sounds, "11:1 00:2 aa:0", NULL },
	{ "sounds-kept", s_sounds_kept, "11:1 00:2 aa:0", NULL },
	{ "stored-name", s_stored_name, "11:1 00:2 aa:0", NULL },
	{ "same-name", s_same_name, "11:1 00:2 aa:0", NULL },
	{ "wrong-phone", s_wrong_phone, "11:1 00:2 aa:0", NULL },
};
#define SCENARIOS (sizeof(scenarios) / sizeof(scenarios[0]))

static int one(size_t i)
{
	make_root();
	setenv("PROFILE_PHONES", scenarios[i].profiles, 1);
	if (scenarios[i].connect)
		setenv("CONNECT_AT_IGNITION", scenarios[i].connect, 1);
	else
		unsetenv("CONNECT_AT_IGNITION");
	scenarios[i].run();
	remove_root();
	printf("%-22s %3d checks, %d failed\n", scenarios[i].name, checks, fails);
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	int failed = 0;

	if (argc == 3 && !strcmp(argv[1], "-v")) {
		verbose = 1;
		setvbuf(stdout, NULL, _IONBF, 0);	/* a scenario that crashes still shows how far it came */
		for (size_t i = 0; i < SCENARIOS; i++)
			if (!strcmp(scenarios[i].name, argv[2]))
				return one(i);
		fprintf(stderr, "no scenario %s\n", argv[2]);
		return 2;
	}
	for (size_t i = 0; i < SCENARIOS; i++) {
		int status = 0;
		pid_t pid;

		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			int bad = one(i);

			fflush(stdout);
			_exit(bad);
		}
		if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
			failed++;
	}
	printf("%zu scenarios, %d failed\n", SCENARIOS, failed);
	return failed ? 1 : 0;
}
