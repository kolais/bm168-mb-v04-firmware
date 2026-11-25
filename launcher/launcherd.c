/* SPDX-License-Identifier: MIT
 * launcherd: the new Launcher's logic (docs/launcher-design.md). It decides;
 * the unit it runs on is behind launcher/io.h, and what the driver sees and
 * does is the navigation in launcher/ui/ (design/). It replaces stock
 * Launcher's start-up jobs and CarPlay handling:
 *
 *  - MCU start frames as stock sends them (status, backlight, version,
 *    source; volume stays dropped by the proxy), picture switching;
 *  - Bluetooth (gocsdk): device name and PIN from /data/Blutooth.ini,
 *    the unit's address for carplay (/tmp/bluetoothaddress);
 *  - wireless CarPlay started by itself on the phone of the car's driver
 *    profile, and on no other; otherwise Home waits for the driver's choice;
 *  - when CarPlay is up: Bluetooth released (CD, P0, as stock), audio source
 *    4 claimed, GUI layer transparent, picture switched to the unit;
 *    phone hands the screen back (63): BMW picture; takes it (62): unit;
 *  - touch forwarded to carplay while a session is up.
 *
 * Runs as root outside the stock chroot (/mnt/stock). Logs codes only:
 * never Bluetooth names, addresses or PINs.
 */
#include "io.h"
#include "phones.h"
#include "display.h"
#include "nav.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CP_DEST "com.arkmicro.carplay"
#define CP_PATH "/com/arkmicro/carplay"
#define CP_IFACE "Local.DbusServer.Carplay"
#define AU_DEST "com.arkmicro.audio"
#define AU_PATH "/com/arkmicro/audio"
#define AU_IFACE "Local.DbusServer.Audio"

enum { IDLE, STARTING, UP, SWITCHING };

static int state = IDLE, phone_connected, bt_released;
static time_t start_at, next_try;
static struct phone phones[MAX_PHONES];
static int nphones, target = -1, ui_ok, phone_screen;
static long reveal_at_ms;
static long reveal_from_ms, reveal_from_frames;	/* measurement: reveal after how long */
static int banner_pending;
static int pairing, pair_known;
static int switch_started;	/* the chosen phone's start has been requested */
static int bt_phone = -1;	/* index of the phone on hands-free, -1 unknown */
static char bt_addr[18];	/* its address (JH<addr>, which comes before SA<name>); "" unknown */
static time_t stopped_at;	/* our full carplay stop (24): its own down statuses follow */
static int start_fails;	/* failed starts on the current target */
static char pair_before[MAX_PHONES][18];	/* addresses known when pairing began */
static char pair_keys[4096];	/* gocsdk's key file when pairing began */
static int pair_keys_len;
static time_t pair_until, pair_check;

/* What the driver is shown (launcher/ui/nav.h). */
static struct ui_model model;
static struct ui_phone shown_phones[MAX_PHONES];
static char chip[96], unit_name[64] = "CARKIT";
#define NAME_FILE "/data/autotop-name"	/* the name the driver gave the unit, if any; in the saved state */
static int name_pending;	/* a new name waits for Bluetooth to be on */
static int names_stale;		/* the running carplay still has the unit's old name */
static char unreachable[MAX_PHONES][18];	/* phones whose last attempt failed (by address) */
static int watching;		/* the driver is on the waiting screen, watching this connect */
static long failed_until_ms;	/* "Couldn't connect" stays until then; Home follows */
static int failed_phone;
static int rhd;			/* steering on the right: Back and the cog's end of the band, and CarPlay's layout */
/* The one phone that connects without being asked: the car's driver
 * profile's. -1: the profile has none, Guest, "Connect at ignition" is off,
 * or the driver has chosen for himself, which stands. */
static int auto_phone = -1;
static time_t auto_page_at;	/* it was not there: page it again then */
static int decided;		/* start-up: the car's profile has arrived, or the wait for it is over */
#define PICK_PAIR -2		/* switch_to(): pair a phone; cancel pairing */
#define PICK_CANCEL -3

/* Bluetooth connection policy (docs/launcher-design.md, 2026-11-20). gocsdk's
 * link policy is set with AT#CA<visible><connectable> (its PAIR_MODE handler
 * takes two digits; gocsdk itself sets "01" when Bluetooth starts and "00" on
 * CB). SEEK keeps every phone out (not connectable) while the unit pages the
 * chosen one; CLOSED is the same without a page in flight; PAIR is visible
 * for "Pair a phone"; OFF while CarPlay runs (P0, as stock): no phone on
 * hands-free then, so a passenger's calls stay on the passenger's phone.
 * No mode lets a paired phone connect by itself (owner, 2026-11-21: only the
 * profile's phone, or the driver's choice). */
enum { BT_OFF, BT_CLOSED, BT_SEEK, BT_PAIR };
static int bt_mode = BT_CLOSED;
static int bt_ready = 1;	/* gocsdk up and powered: IS1 after our P1 */
static int seek_active, seek_phone = -1, seek_tries, seek_max, seek_for_switch;
static long seek_next_ms;	/* next paging attempt (0: none scheduled) */
static long seek_deadline_ms;	/* the attempt in flight gives up (0: none) */
static long seek_attempt_ms;	/* when the attempt in flight started */
static time_t power_on_at;	/* our P1: IS1 expected within ~5 s */
static long switch_grace_ms;	/* first page of the next switch delayed (just paired) */
static int av_connected;	/* A2DP up (MU2/MU3): a phone on music only */
static int seek_phase;	/* 0: disconnect step next, 1: page next */

/* BMW driver profile from the MCU patch (docs/mcu-profile-patch.md): MCU
 * report E6 02 3A <profile> <fresh>, profile = CAN 0x23A byte 0 (00, 11, 22;
 * aa = Guest), fresh = seen since the car's bus woke. PROFILE_PHONES maps it
 * to a picker phone number, 0 for no CarPlay at all (stay in the launcher):
 * "11:1 00:2 aa:0" (owner, 2026-11-21). PROFILE_SWITCH=0 applies it at
 * start-up only. Nothing chosen under a no-CarPlay profile is ever kept
 * (storage is read-only at run time; owner: a guest's choice must never
 * persist). */
enum { PROFILE_UNMAPPED = -2, PROFILE_NONE = -1 };
/* A profile's phone is held by the phone's address: its place in the list
 * changes as phones come and go. "" = no phone for the profile, "-" = no
 * CarPlay under it. */
static char profile_addr[256][18];
static int profile_switch = 1;
static int car_profile = -1;	/* last fresh profile reported, -1 none yet */
static int profile_applied = -1;	/* the profile acted on */
static long start_ms;	/* launcherd start: window for holding the first page */
#define PROFILE_HOLD_MS 8000	/* first page waits at most this long for a fresh profile */
static int seek_cycled;	/* Bluetooth restarted during this seek */
static int page_pending;	/* our CC not yet answered by IB or IA */
/* gocsdk ends a page itself (IA, "connect timeout"); a new CC before that is
 * rejected ("connect pending"). This only catches a lost result. */
#define SEEK_ATTEMPT_MS 30000
static void give_up(int phone, int page_again);
static void carplay_to_front(void);
static long now_ms(void);
static long reveal_frames;	/* decoder count to reach before revealing */
static long reveal_limit_ms;	/* reveal anyway after this */

/* Reveal CarPlay's picture once its stream has settled: a restarted stream
 * can show a few frames with the wrong geometry (owner's photo, 2026-11-20:
 * tiled, green/purple; a faint flicker remained with 8 frames / 300 ms), so
 * wait for 12 decoded frames, at least 500 ms, at most 2.5 s. */
/* A session coming back to the front, as opposed to one starting: the
 * decoder is running and the phone's first frame is a whole picture, so
 * two frames are enough. A starting session's first frames are garbled
 * (2026-11-20), and a still screen sends few: 4 in 2.5 s at the bench on
 * 2026-11-22, which kept the cover up to the time limit. */
static int returning;
/* The phone has handed the screen back (63) and has not taken it again:
 * its picture on the video layer is stale. After a short MENU it is not
 * told, goes on sending, and the layer stays current. */
static int phone_released;

static void reveal_after_frames(void)
{
	long f = io_vdec_frames();

	reveal_from_ms = now_ms();
	reveal_from_frames = f;
	reveal_frames = f >= 0 ? f + (returning ? 2 : 12) : -1;
	reveal_at_ms = now_ms() + (returning ? 100 : 500);
	reveal_limit_ms = now_ms() + (returning ? 1500 : 2500);
}	/* clear the cover this long after CarPlay has the screen */
static char lastaddr[32];

/* A path inside the stock root (/mnt/stock on the unit). */
static const char *rp(const char *rel)
{
	static char buf[4][160];
	static int k;

	k = (k + 1) % 4;
	snprintf(buf[k], sizeof(buf[k]), "%s%s", io_root(), rel);
	return buf[k];
}

/* A phone's place in the list, -1 if it is not there. A phone is known by
 * its address: names repeat, and places change. */
static int phone_at(const char *addr)
{
	for (int i = 0; i < nphones; i++)
		if (!strcmp(phones[i].addr, addr))
			return i;
	return -1;
}

/* The phones as gocsdk has them now. Their places in the list are not
 * fixed: gocsdk gives a new phone the lowest free slot, so it can come in
 * before the others. Whatever holds a phone by its place (the one in
 * CarPlay, the one sought, the one on hands-free) follows the phone by its
 * address; a phone that is gone leaves it empty. */
static int *const held[] = { &target, &auto_phone, &seek_phone, &bt_phone };
#define HELD ((int)(sizeof(held) / sizeof(held[0])))

static void held_note(char was[HELD][18])
{
	for (int k = 0; k < HELD; k++) {
		was[k][0] = 0;
		if (*held[k] >= 0 && *held[k] < nphones)
			memcpy(was[k], phones[*held[k]].addr, sizeof(was[k]));
	}
}

static void held_follow(char was[HELD][18])
{
	for (int k = 0; k < HELD; k++)
		if (was[k][0])
			*held[k] = phone_at(was[k]);
}

static int phones_reload(void)
{
	char was[HELD][18];

	held_note(was);
	nphones = phones_load(rp("/etc/goc/bt_conf.ini"), phones);
	held_follow(was);
	return nphones;
}

/* Phone i off the list in memory, ahead of gocsdk (Forget). */
static void phones_drop(int i)
{
	char was[HELD][18];

	held_note(was);
	memmove(&phones[i], &phones[i + 1], (nphones - i - 1) * sizeof(phones[0]));
	nphones--;
	held_follow(was);
}

/* The place in the list of a profile's phone; PROFILE_NONE: no CarPlay under
 * this profile; PROFILE_UNMAPPED: it has no phone, or its phone is gone. */
static int profile_phone(int code)
{
	if (code < 0 || code > 255 || !profile_addr[code][0])
		return PROFILE_UNMAPPED;
	if (!strcmp(profile_addr[code], "-"))
		return PROFILE_NONE;
	return phone_at(profile_addr[code]) >= 0 ? phone_at(profile_addr[code]) : PROFILE_UNMAPPED;
}

void say(const char *fmt, ...)
{
	char line[320];
	va_list ap;
	int n = snprintf(line, sizeof(line), "%6.1f ", io_now_ms() / 1000.0);

	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	io_log(line);
}

static long now_ms(void)
{
	return io_now_ms();
}

static time_t now(void)
{
	return io_now_ms() / 1000;
}

/* ---- files ---- */

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return n;
}

/* KEY=value from an INI file (first match, CR stripped). */
static int ini_get(const char *path, const char *key, char *out, size_t len)
{
	char buf[4096], *p;
	size_t k = strlen(key);

	if (read_file(path, buf, sizeof(buf)) < 0)
		return -1;
	for (p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
		if (!strncmp(p, key, k) && p[k] == '=') {
			size_t n = strcspn(p + k + 1, "\r\n");
			if (n >= len)
				n = len - 1;
			memcpy(out, p + k + 1, n);
			out[n] = 0;
			return 0;
		}
	}
	return -1;
}

static void copy_file(const char *from, const char *to)
{
	char buf[256];
	int n = read_file(from, buf, sizeof(buf)), fd;

	if (n <= 0)
		return;
	fd = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, buf, n) != n)
			say("short write to %s", to);
		close(fd);
	}
}

/* ---- MCU (via mcuproxy's pty; the proxy's allow-list still applies) ---- */

static void mcu_send(int cmd, const unsigned char *data, int n)
{
	unsigned char f[64];
	int len = n + 2, sum = 0, i = 0;

	if (!io_mcu_open() || n > 50)
		return;
	f[i++] = 0xAA;
	f[i++] = 0x55;
	f[i++] = 0;
	f[i++] = len;
	f[i++] = cmd;
	memcpy(f + i, data, n);
	i += n;
	for (int k = 2; k < i; k++)
		sum += f[k];
	f[i++] = (-sum) & 0xFF;
	if (io_mcu_write(f, i))
		say("mcu write failed");
	say("mcu > %02x %s", cmd, n ? "" : "(no data)");
}

static void mcu1(int cmd, int a)
{
	unsigned char d[1] = { a };
	mcu_send(cmd, d, 1);
}

static void mcu2(int cmd, int a, int b)
{
	unsigned char d[2] = { a, b };
	mcu_send(cmd, d, 2);
}

/* Picture source as the MCU last reported it (DE 00 xx, every 3 s and after
 * a change) or as we last set it; -1 unknown. A repeated 4C 00 00 while the
 * picture is already the unit's is not sent: suspected of a short whole-
 * screen glitch ("analog TV" lines over the cover; status 62 and 4 each sent
 * one during every switch, 2026-11-21). */
static int pic_source = -1;
static unsigned pic_skipped;

static void picture_to(int src)
{
	if (pic_source == src) {
		pic_skipped++;
		return;
	}
	mcu2(0x4C, 0x00, src);
	pic_source = src;
}
static void picture_to_unit(void) { picture_to(0); }
static void picture_to_car(void) { picture_to(1); }


static int night = -1;	/* car illumination: -1 unknown, 0 day, 1 night */

static void cp_link(int status);
static void profile_report(int p, int fresh);

/* One MCU report (frame verified). Phase 1 acts on illumination and logs
 * keys and ACC by code for the car session (docs/launcher-analysis.md,
 * MCU -> Launcher commands); key mapping and the ACC sequence follow. */
static void mcu_report(int cmd, const unsigned char *d, int n)
{
	switch (cmd) {
	case 0x8D:	/* illumination: CarPlay night mode, as stock */
		if (n >= 1 && night != (d[0] == 1)) {
			night = d[0] == 1;
			say("illumination %s", night ? "on: night mode" : "off: day mode");
			cp_link(night ? 31 : 32);
		}
		break;
	case 0x8A:
		say("ACC %s (data %d)", n >= 1 && d[0] == 0 ? "OFF" : "on", n >= 1 ? d[0] : -1);
		break;
	case 0xB2:
		say("panel key state %d code 0x%02x", n >= 1 ? d[0] : -1, n >= 2 ? d[1] : 0);
		break;
	case 0xD0:
		if (n >= 4 && d[0] == 0x4B && d[1] == 0x81) {
			/* iDrive (car, 2026-11-21): rotation 01 47 right, 01 46 left, one
			 * frame per detent; keys 00 61 push, 00 62 Back, 00 63 MENU short;
			 * 00 00 is a release. */
			char c = d[2] == 1 ? (d[3] == 0x47 ? 'r' : d[3] == 0x46 ? 'l' : 0) :
				 d[2] == 0 ? (d[3] == 0x61 ? 'p' : d[3] == 0x62 ? 'b' : d[3] == 0x63 ? 'm' : 0) : 0;

			if (d[2] || d[3])
				say("CAN key %02x %02x%s", d[2], d[3], c ? "" : " (no use for it)");
			if (c)
				core_key(c);
		} else if (n >= 1 && d[0] == 0x4B) {
			say("CAN key: %d bytes, sub 0x%02x 0x%02x 0x%02x", n, n > 1 ? d[1] : 0,
			    n > 2 ? d[2] : 0, n > 3 ? d[3] : 0);
		}
		break;
	case 0xDE:
		if (n >= 2 && d[0] == 0x00) {
			/* raw value: 00 unit, 01 car, 02-04 camera inputs */
			int changed = pic_source != d[1];

			if (changed)
				say("picture source: %s %d (repeats not sent: %u)", d[1] ? "car" : "unit", d[1], pic_skipped);
			pic_source = d[1];
			/* The MCU brought the picture back to the unit by itself (a
			 * long MENU) while CarPlay waits behind the cover: to the
			 * front with it. */
			if (changed && d[1] == 0 && state == UP && !phone_screen && ui_ok &&
			    ui_frame_layer_now() == LAYER_GROUND) {
				say("picture back at the unit: CarPlay to the front");
				carplay_to_front();
			}
		}
		break;
	case 0xE6:	/* MCU patch: driver profile (CAN 0x23A byte 0) */
		if (n >= 4 && d[0] == 0x02 && d[1] == 0x3A)
			profile_report(d[2], d[3]);
		break;
	}
}

void core_mcu_bytes(const unsigned char *b, int r)
{
	static unsigned char f[64];
	static int n;

	for (int i = 0; i < r; i++) {
		unsigned char c = b[i];

		if (n == 0 && c != 0xAA)
			continue;
		if (n == 1 && c != 0x55) {
			n = c == 0xAA;
			continue;
		}
		f[n++] = c;
		if (n >= 4 && (f[2] != 0 || f[3] < 2 || f[3] > 55)) {
			n = 0;	/* bad length: resync */
			continue;
		}
		if (n >= 4 && n == 4 + f[3]) {
			int sum = 0;

			for (int k = 2; k < n - 1; k++)
				sum += f[k];
			if (((-sum) & 0xFF) == f[n - 1])
				mcu_report(f[4], f + 5, f[3] - 2);
			n = 0;
		}
	}
}

/* ---- Bluetooth (gocsdk AT interface) ---- */

/* gocsdk drops a command that arrives together with the previous one: P1,
 * MM, MN, MH written back to back reached it as P1 only, and CD + CC as CD
 * (2026-11-20, its command_reader log). Commands are queued and written
 * BT_GAP_MS apart. */
#define BT_GAP_MS 400
#define BT_QUEUE 16
static char bt_q[BT_QUEUE][96];
static int bt_qhead, bt_qlen;
static long bt_next_ms;

static void bt_flush(void)
{
	if (!io_bt_open() || !bt_qlen || now_ms() < bt_next_ms)
		return;
	char *line = bt_q[bt_qhead];
	int n = strlen(line);

	if (io_bt_write(line, n))
		say("bt write failed");
	char code[3] = { line[3], line[4], 0 };
	say("bt > %s", code);	/* code only: never the argument */
	bt_qhead = (bt_qhead + 1) % BT_QUEUE;
	bt_qlen--;
	bt_next_ms = now_ms() + BT_GAP_MS;
}

static void bt_cmd(const char *code, const char *arg)
{
	if (!io_bt_open())
		return;
	if (bt_qlen == BT_QUEUE) {
		say("bt queue full: %s dropped", code);
		return;
	}
	snprintf(bt_q[(bt_qhead + bt_qlen) % BT_QUEUE], sizeof(bt_q[0]), "AT#%s%s\r\n", code,
		 arg ? arg : "");
	bt_qlen++;
	bt_flush();
}

static void bt_open(void)
{
	char name[64] = "CARKIT", pin[16] = "0000";

	if (io_bt_connect())	/* gocsdk's serial link: not there yet */
		return;
	ini_get(rp("/data/Blutooth.ini"), "DeviceName", name, sizeof(name));
	ini_get(rp("/data/Blutooth.ini"), "PinCode", pin, sizeof(pin));
	snprintf(unit_name, sizeof(unit_name), "%s", name);
	/* The driver's own name for the unit, kept from an earlier run: for
	 * Bluetooth in the set-up below, and for the access point, whose file
	 * is made anew at every start. */
	if (read_file(rp(NAME_FILE), name, sizeof(name)) > 0) {
		name[strcspn(name, "\r\n")] = 0;
		if (name[0]) {
			snprintf(unit_name, sizeof(unit_name), "%s", name);
			io_ap_name(unit_name);
			io_rotate_ap();	/* hostapd reads the file again: nobody is on it yet */
			say("the unit's stored name is in use (%d characters)", (int)strlen(unit_name));
		}
	}
	/* Power on first: a previous session (or crash) may have left Bluetooth
	 * off (P0 is sent when CarPlay is up), and the phone needs Bluetooth
	 * for the iAP2 hand-shake of the next start. */
	bt_cmd("P1", NULL);
	bt_cmd("MM", name);
	bt_cmd("MN", pin);
	/* Connection policy belongs to the Launcher: gocsdk's own auto-connect
	 * retried a deleted phone's address and fought phone switching
	 * (2026-11-20). Phones that connect by themselves are still accepted. */
	bt_cmd("MH", NULL);
	/* No automatic answering of calls on hands-free (stock bt_conf.ini has
	 * autoanswer=1): a phone on hands-free outside CarPlay would otherwise
	 * have its calls taken by the car. */
	bt_cmd("MQ", NULL);
	/* A phone may already be connected (launcherd restarted): its name (QB ->
	 * SA) and the hands-free state (CY -> MG3). Connecting it again broke
	 * its link (2026-11-20). */
	bt_cmd("QB", NULL);
	bt_cmd("CY", NULL);
}

static void start_carplay(void);
static void switch_to(int i);
static void bt_power_on(void);
static void bt_release(void);
static void bt_set_mode(int mode);

/* ---- what the driver is shown ---- */

static int is_unreachable(const char *addr)
{
	for (int k = 0; k < MAX_PHONES; k++)
		if (!strcmp(unreachable[k], addr))
			return 1;
	return 0;
}

/* "Unreachable": the last attempt failed. Cleared at the next attempt. */
static void set_unreachable(const char *addr, int on)
{
	for (int k = 0; k < MAX_PHONES; k++)
		if (!strcmp(unreachable[k], on ? "" : addr)) {
			snprintf(unreachable[k], sizeof(unreachable[k]), "%s", on ? addr : "");
			if (on)
				return;
		}
}

/* The car's profiles by their CAN value: 00, 11, 22 are profiles 1 to 3;
 * aa is Guest. */
static int profile_number(int code)
{
	return code == 0x00 ? 1 : code == 0x11 ? 2 : code == 0x22 ? 3 : 0;
}

/* The facts the navigation draws from, from our state. Names go to the
 * screen only: never to the log. */
/* BENCH=1 in PLANA.CFG, for the bench (owner, 2026-11-22): a stand-in time
 * in the top band, and the Name row works although a new name cannot be
 * kept yet (nothing on the unit is writable: it holds until power-off).
 * Never for the car. */
static int bench;
static char clock_text[8];
static long clock_minute = -1;

/* The Bluetooth mark: nothing while Bluetooth is off (it is, while CarPlay
 * runs and while nobody is being connected), the rune once gocsdk is up
 * after our power-on, with its lines while a phone is on hands-free. */
static enum ui_bt bt_mark(void)
{
	return bt_released || !bt_ready ? UI_BT_OFF : phone_connected ? UI_BT_LINKED : UI_BT_ON;
}

static void model_update(void)
{
	static const int codes[3] = { 0x00, 0x11, 0x22 };
	int connected = -1;

	for (int i = 0; i < nphones; i++) {
		struct ui_phone *s = &shown_phones[i];

		s->name = phones[i].name;
		s->state = state == UP && i == target ? PHONE_CONNECTED :
			   is_unreachable(phones[i].addr) ? PHONE_UNREACHABLE :
			   phones[i].last ? PHONE_LAST_USED : PHONE_PAIRED;
		s->profile = 0;
		for (int k = 0; k < 3; k++)
			if (profile_phone(codes[k]) == i)
				s->profile = k + 1;
		if (s->state == PHONE_CONNECTED)
			connected = i;
	}
	model.phones = shown_phones;
	model.n = nphones;
	model.guest = profile_applied == 0xaa;
	model.profile_phone = profile_phone(profile_applied) < 0 ? -1 : profile_phone(profile_applied);
	model.chip = connected >= 0 ? CHIP_UP : CHIP_IDLE;
	if (connected >= 0)
		snprintf(chip, sizeof(chip), "%s", phones[connected].name);	/* behind the CarPlay sign */
	else if (model.guest)
		snprintf(chip, sizeof(chip), "Guest");
	else if (profile_applied >= 0 && profile_number(profile_applied) && model.profile_phone < 0)
		snprintf(chip, sizeof(chip), "No phone for profile %d", profile_number(profile_applied));
	else
		chip[0] = 0;	/* no chip: that no phone is connected is on the tiles (owner, 2026-11-23) */
	model.chip_text = chip;
	model.bt = bt_mark();
	model.clock = "";	/* the car's time: not decoded yet */
	if (bench) {
		/* 12:00 at the launcher's start, and it runs. A clock that is
		 * wrong is worse than none, so not in the car. */
		long minutes = 12 * 60 + now_ms() / 60000;

		snprintf(clock_text, sizeof(clock_text), "%02ld:%02ld", minutes / 60 % 24, minutes % 60);
		model.clock = clock_text;
	}
	model.name = unit_name;
	model.set[SET_STEERING_RIGHT] = rhd;
}

/* Our state changed: a launcher screen that is up shows it. */
static void ui_sync(void)
{
	if (!ui_ok)
		return;
	model_update();
	ui_nav_refresh();
}

static void show_home(int focus)
{
	model_update();
	ui_nav_home(focus);
}

/* The waiting screen is what is up. */
static int waiting_shown(void)
{
	struct ui_nav_state s;

	ui_nav_state(&s);
	return ui_frame_layer_now() == LAYER_SCREEN && s.page == PAGE_WAIT;
}

/* The layer when nobody is being connected: clear with CarPlay in front;
 * the cover with a session alive behind the car's picture; otherwise Home,
 * ready for the moment the picture comes to the unit (a touch, or the MCU's
 * own switch on a long MENU, which sends no key). */
static void idle_screen(void)
{
	if (!ui_ok)
		return;
	if (state == UP && phone_screen)
		ui_nav_hide();
	else if (state == UP)
		ui_nav_cover();
	else
		show_home(UI_FOCUS_DEFAULT);
}

/* Bluetooth when nobody is being connected: off, unless the profile's phone
 * is still to be found: then on, not connectable, and paged every 30 s. */
static void idle_bluetooth(void)
{
	if (auto_phone >= 0) {
		bt_power_on();
		bt_set_mode(BT_CLOSED);
		auto_page_at = now() + 30;
	} else {
		bt_release();
	}
}

/* Page phone i: gocsdk's CC takes the 12-digit address. Anything else is
 * "invalid" and gocsdk then connects the most recent phone instead
 * (pairlistindex) - "CC<index>" always paged the last phone (2026-11-20). */
static void bt_page(int i)
{
	char hex[13];
	int n = 0;

	for (const char *c = phones[i].addr; *c && n < 12; c++)
		if (*c != ':')
			hex[n++] = *c;
	hex[n] = 0;
	if (n == 12) {
		bt_cmd("CC", hex);	/* logged as the code only */
		page_pending = 1;
	}
	else
		say("phone %d: no usable address", i + 1);
}

/* gocsdk link policy for the current mode; re-applied on every IS1, since
 * gocsdk sets "connectable" itself whenever Bluetooth starts. */
static long ca_sent_ms;	/* last link policy sent */

/* What the driver changed on the unit is kept across power-off (the state
 * archive, recovery/plan-a/state-save): asked for a few seconds after the
 * change, so gocsdk and carplay have written their own files, and once
 * more when the first CarPlay session of a new phone is up, which is when
 * carplay has its keys. */
static long save_at_ms;
static int save_after_up;

static void save_soon(const char *why)
{
	save_at_ms = now_ms() + 3000;
	say("state: a save is due (%s)", why);
}

/* A launcher sound (design/feedback.md), mixed under whatever plays. The
 * navigation has already applied the "Launcher sounds" level to its own;
 * the level is applied here to the launcher's. None while CarPlay or the
 * car owns the screen (nothing of the launcher's is on it but, at most, the
 * banner), and at most 20 ticks a second. */
static void play(enum ui_sound s)
{
	static long last_tick_ms;

	if (!model.set[SET_SOUNDS] || !ui_ok || ui_frame_layer_now() == LAYER_CLEAR)
		return;
	if (s == SND_TICK) {
		if (now_ms() - last_tick_ms < 50)
			return;
		last_tick_ms = now_ms();
	}
	io_sound(s);
}

/* The launcher's own settings, in the saved state: "sounds 1", "effects 0". */
#define SETTINGS_FILE "/data/autotop-settings"

static void settings_store(void)
{
	char text[64];

	snprintf(text, sizeof(text), "sounds %d\neffects %d", model.set[SET_SOUNDS], model.set[SET_EFFECTS]);
	io_put(SETTINGS_FILE, text);
	save_soon("setting");
}

static void settings_restore(void)
{
	char text[128];
	int v;

	if (read_file(rp(SETTINGS_FILE), text, sizeof(text)) <= 0)
		return;
	if (strstr(text, "sounds ") && sscanf(strstr(text, "sounds "), "sounds %d", &v) == 1 && v >= 0 && v <= 2)
		model.set[SET_SOUNDS] = v;
	if (strstr(text, "effects ") && sscanf(strstr(text, "effects "), "effects %d", &v) == 1 && v >= 0 && v <= 1)
		model.set[SET_EFFECTS] = v;
}

/* Forget (design/screens.md). gocsdk drops a pairing and its keys on
 * AT#CV<n>, where n is the phone's place in gocsdk's own count: the most
 * recently connected first, so it changes with every connection. The count
 * is read at the moment of sending, from gocsdk's file, and held against the
 * listing gocsdk gives when Bluetooth comes on (MX<n><address>, then PO).
 * If the two differ, nothing is dropped: the command cannot be taken back.
 * Like every command it is lost while Bluetooth is off, so Bluetooth is
 * switched on for it, closed to everyone. */
enum { FORGET_NONE, FORGET_WAITING, FORGET_ASKED };
static int forget_phase;	/* WAITING for Bluetooth and its listing; ASKED: AT#CV went out, or was held back */
static long forget_at_ms;	/* WAITING: send by then, listing or not; ASKED: read the list again then */
static char forget_addr[18];	/* the phone being dropped */
static int forget_bt;		/* Bluetooth is on for this only */
static char listed[MAX_PHONES + 1][18];	/* gocsdk's listing: [n] its n-th phone */
static int listed_whole;	/* to its end, and nobody has connected since */

static void forget_send(void)
{
	struct phone now[MAX_PHONES];
	int n = phones_load(rp("/etc/goc/bt_conf.ini"), now), by_file = 0, by_listing = 0;
	char place[8];

	for (int i = 0; i < n && i < MAX_PHONES; i++)	/* the bound: for the compiler */
		if (!strcmp(now[i].addr, forget_addr))
			by_file = now[i].index;
	for (int d = 1; d <= MAX_PHONES; d++)
		if (!strcmp(listed[d], forget_addr))
			by_listing = d;
	forget_phase = FORGET_ASKED;
	/* The command waits its turn behind the others (400 ms each), and
	 * gocsdk then needs a moment to write its file. */
	forget_at_ms = now_ms() + 3500;
	if (!by_file || (listed_whole && by_listing != by_file)) {
		say("forget: gocsdk's count is not clear (%d by its file, %d by its listing): nothing dropped",
		    by_file, by_listing);
		return;
	}
	snprintf(place, sizeof(place), "%d", by_file);
	bt_cmd("CV", place);
}

static void bt_apply(void)
{
	if (!bt_ready || bt_released)
		return;
	if (name_pending) {
		bt_cmd("MM", unit_name);
		name_pending = 0;
	}
	if (forget_phase == FORGET_WAITING && !forget_bt)
		forget_send();	/* Bluetooth was on already: no listing is coming */
	ca_sent_ms = now_ms();
	if (bt_mode == BT_SEEK || bt_mode == BT_CLOSED)
		bt_cmd("CA", "00");	/* invisible, not connectable */
	else if (bt_mode == BT_PAIR)
		bt_cmd("CA", "11");	/* visible, connectable */
}

/* Nobody is on hands-free (any more). */
static void bt_nobody(void)
{
	phone_connected = 0;
	bt_phone = -1;
	bt_addr[0] = 0;
}

static void bt_power_on(void)
{
	if (bt_released) {
		bt_cmd("P1", NULL);	/* IS1 follows in ~5 s; commands before it are lost */
		bt_released = 0;
		bt_ready = 0;
		page_pending = 0;
		power_on_at = now();
	}
}

/* CarPlay runs: Bluetooth off, as stock (no phone on hands-free). */
static void bt_release(void)
{
	if (bt_released)
		return;
	bt_cmd("CD", NULL);
	bt_cmd("P0", NULL);
	bt_released = 1;
	bt_ready = 0;
	bt_mode = BT_OFF;
	seek_active = 0;
	av_connected = 0;
	bt_nobody();
}

static void bt_set_mode(int mode)
{
	if (mode != BT_SEEK)
		seek_active = 0;
	bt_mode = mode;
	bt_apply();
}

/* Get phone i onto hands-free and nothing else: the unit stops accepting
 * connections and pages i itself, up to `tries` times. A phone that reconnects
 * by itself (the one CarPlay just left) can no longer take the link. `grace`
 * delays the first page (a phone just paired connects by itself). */
static void seek(int i, int tries, int for_switch, long grace_ms)
{
	if (i < 0 || i >= nphones)
		return;
	bt_power_on();
	seek_active = 1;
	seek_phone = i;
	seek_tries = 0;
	seek_max = tries;
	seek_for_switch = for_switch;
	seek_next_ms = now_ms() + grace_ms;
	seek_deadline_ms = 0;
	seek_phase = 0;
	seek_cycled = 0;
	bt_mode = BT_SEEK;
	set_unreachable(phones[i].addr, 0);
	if (!grace_ms)
		bt_apply();
	if (phone_connected && bt_phone == i && !bt_released)
		seek_next_ms = now_ms();	/* already there (e.g. just paired) */
	say("seek: phone %d of %d (%d tries)", i + 1, nphones, tries);	/* index only */
}

static void seek_done(void)
{
	say("seek: phone %d on hands-free", seek_phone + 1);
	seek_active = 0;
	seek_deadline_ms = 0;
	target = seek_phone;
	/* Stays not connectable until CarPlay is up (Bluetooth is then off). */
	if (state == IDLE || (state == SWITCHING && !switch_started))
		start_carplay();
}

static void seek_fail(void)
{
	say("seek: phone %d not reachable", seek_phone + 1);
	seek_active = 0;
	seek_deadline_ms = 0;
	give_up(seek_phone, 1);
}

/* One attempt in two steps: disconnect whatever is there (CD also cancels a
 * page gocsdk still has pending), then 1.5 s later page the chosen phone by
 * address. Before the last attempt Bluetooth is restarted: that cleared a
 * stuck "connect pending" at the bench (2026-11-20). */
/* Bluetooth off and on again within a seek: clears a connect gocsdk still
 * holds as pending (it refuses every new page until its own timeout). */
static void seek_restart_bt(void)
{
	seek_cycled = 1;
	seek_deadline_ms = 0;
	bt_cmd("P0", NULL);
	bt_released = 1;
	bt_ready = 0;
	av_connected = 0;
	bt_nobody();
	seek_next_ms = now_ms() + 2500;	/* seek_tick powers on again */
}

static void seek_attempt(void)
{
	if (phone_connected && bt_phone < 0) {	/* IB seen, SA (which phone) pending */
		seek_next_ms = now_ms() + 1000;
		return;
	}
	if (phone_connected && bt_phone == seek_phone) {
		seek_done();
		return;
	}
	/* The count holds whichever way the next page was reached: another
	 * phone answering, and being dropped, leads straight to it. */
	if (seek_tries >= seek_max) {
		seek_fail();
		return;
	}
	if (seek_phase == 0) {
		if (seek_tries >= 2 && !seek_cycled) {
			say("seek: Bluetooth restart before the last attempt");
			seek_restart_bt();
			return;
		}
		if (now_ms() - ca_sent_ms > 1500) {	/* not just sent on IS1 */
			bt_cmd("CA", "00");	/* re-assert: gocsdk sets connectable on its own at start */
			ca_sent_ms = now_ms();
		}
		if (phone_connected || av_connected || page_pending) {
			if (av_connected)
				bt_cmd("DA", NULL);
			bt_cmd("CD", NULL);
			seek_phase = 1;
			seek_next_ms = now_ms() + 1500;
			return;
		}
		/* Nothing to disconnect (e.g. right after power-on): page now. */
	}
	seek_phase = 0;
	seek_tries++;
	if (watching && ui_ok && seek_tries > 1)	/* an attempt failed and the unit tries again */
		ui_nav_wait(WAIT_RETRYING, phones[seek_phone].name);
	bt_page(seek_phone);
	seek_next_ms = 0;
	seek_attempt_ms = now_ms();
	seek_deadline_ms = seek_attempt_ms + SEEK_ATTEMPT_MS;
	say("seek: paging phone %d (attempt %d of %d)", seek_phone + 1, seek_tries, seek_max);
}

/* Periodic: the next paging attempt, the attempt in flight timing out. */
static void seek_tick(void)
{
	if (!bt_ready && !bt_released && power_on_at && now() - power_on_at > 12) {
		say("bt: no IS1 after power-on; carrying on");
		bt_ready = 1;
		bt_apply();
	}
	if (!seek_active)
		return;
	if (bt_released) {	/* restart within the seek: power on again */
		if (seek_next_ms && now_ms() >= seek_next_ms) {
			seek_next_ms = 0;
			bt_power_on();
		}
		return;
	}
	if (!bt_ready)
		return;
	if (seek_deadline_ms && now_ms() >= seek_deadline_ms) {
		say("seek: attempt %d timed out", seek_tries);
		seek_deadline_ms = 0;
		seek_next_ms = now_ms();
	}
	if (seek_next_ms && now_ms() >= seek_next_ms)
		seek_attempt();
}

static void bt_event(const char *line)
{
	char code[4] = { 0 };

	if (strlen(line) < 2)
		return;
	/* The code only: with its digit where that is a state or a count, never
	 * where an address begins (JH). */
	memcpy(code, line, line[2] >= '0' && line[2] <= '9' && strncmp(line, "JH", 2) ? 3 : 2);
	say("bt < %s", code);
	/* gocsdk's own listing of its phones, after Bluetooth has come on. */
	if (!strncmp(line, "MX", 2) && line[2] >= '1' && line[2] <= '0' + MAX_PHONES) {
		if (phones_colon(line + 3, listed[line[2] - '0']))
			listed[line[2] - '0'][0] = 0;
	} else if (!strncmp(line, "PO", 2)) {
		listed_whole = 1;
		if (forget_phase == FORGET_WAITING)
			forget_send();
	}
	if (!strcmp(code, "IS1")) {	/* Bluetooth (re)started: gocsdk set "01" itself */
		bt_ready = 1;
		memset(listed, 0, sizeof(listed));
		listed_whole = 0;
		/* The link policy first: gocsdk is connectable from here until our
		 * CA arrives. Behind MH and MQ it went out 0.8 s later, and the
		 * phone being left behind got onto hands-free in that window
		 * (2026-11-21: a 59 s switch). Then settings gocsdk may have
		 * missed while it was starting. */
		bt_apply();
		bt_cmd("MH", NULL);	/* its own auto-connect off */
		bt_cmd("MQ", NULL);	/* no automatic answering */
		if (seek_active && !seek_deadline_ms && (!seek_next_ms || seek_next_ms < now_ms() + 500))
			seek_next_ms = now_ms() + 500;
	}
	/* ER right after our page: gocsdk still holds a connect as pending and
	 * refuses this one; it stayed so for the whole 30 s attempt. Restart
	 * Bluetooth now (once per seek) rather than wait (2026-11-21). */
	if (!strcmp(code, "ER") && seek_active && seek_deadline_ms && now_ms() - seek_attempt_ms < 1500 &&
	    !seek_cycled) {
		say("seek: page refused (connect pending); Bluetooth restart");
		seek_restart_bt();
	}
	if (!strncmp(line, "JH", 2) && phones_colon(line + 2, bt_addr))
		bt_addr[0] = 0;
	if (!strncmp(line, "SA", 2) && line[2]) {	/* name of the phone on hands-free */
		phones_reload();
		/* Which phone: by the address gocsdk gave just before (JH). By
		 * name only without one: two phones may carry one name, so the
		 * phone being sought is taken first. */
		bt_phone = -1;
		if (bt_addr[0])
			bt_phone = phone_at(bt_addr);
		else if (seek_active && seek_phone >= 0 && seek_phone < nphones && !strcmp(phones[seek_phone].name, line + 2))
			bt_phone = seek_phone;
		for (int i = 0; i < nphones && !bt_addr[0] && bt_phone < 0; i++)
			if (!strcmp(phones[i].name, line + 2))
				bt_phone = i;
		say("bt: phone %d of %d on hands-free", bt_phone + 1, nphones);	/* index only */
		if (seek_active) {
			if (bt_phone == seek_phone) {
				seek_done();
			} else if (bt_phone >= 0) {
				/* Connected before the seek began: it cannot come back
				 * while the unit is not connectable. */
				if (av_connected)
					bt_cmd("DA", NULL);
				bt_cmd("CD", NULL);
				seek_deadline_ms = 0;
				seek_phase = 1;	/* page next */
				seek_next_ms = now_ms() + 1500;
			}
		}
		/* A phone on hands-free that nobody asked for (left from before a
		 * restart) gets no CarPlay: it goes when Bluetooth goes off, or
		 * at the next seek. */
	}
	if (!strcmp(code, "IB") || !strcmp(code, "MG3")) {
		listed_whole = 0;	/* gocsdk counts the phone that connects first from now on */
		if (!phone_connected)
			say("phone on hands-free");
		phone_connected = 1;
		page_pending = 0;
	} else if (!strcmp(code, "IA") || !strcmp(code, "MG1")) {
		bt_nobody();
		if (!strcmp(code, "IA"))
			page_pending = 0;
	}
	if (!strncmp(line, "MU", 2) && line[2] >= '0' && line[2] <= '9')
		av_connected = line[2] >= '2';	/* MU1 down, MU2 connected, MU3 playing */
	/* The page in flight ended without the phone: gocsdk's own timeout
	 * (IA) or a failed hands-free connect (IY<non-zero>). Not the IA of
	 * our own CD, which comes within a second of it. */
	if (seek_active && seek_deadline_ms && now_ms() - seek_attempt_ms > 2500 && !phone_connected &&
	    (!strcmp(code, "IA") || (!strncmp(line, "IY", 2) && line[2] && strcmp(line + 2, "0")))) {
		say("seek: attempt %d failed", seek_tries);
		seek_deadline_ms = 0;
		seek_next_ms = now_ms() + 1000;
	}
}

void core_bt_bytes(const char *b, int r)
{
	static char acc[512];
	static size_t n;

	for (int i = 0; i < r; i++) {
		if (b[i] == '\r' || b[i] == '\n') {
			if (n) {
				acc[n] = 0;
				bt_event(acc);
				n = 0;
			}
		} else if (n < sizeof(acc) - 1) {
			acc[n++] = b[i];
		}
	}
}

/* ---- D-Bus: carplay and audio ---- */




static void cp_link(int status)
{
	struct dbm_arg a[2] = { { 'i', 7, 0 }, { 'i', status, 0 } };
	io_call(CP_DEST, CP_PATH, CP_IFACE, "requestLinkStatus", a, 2);
}

static void audio(const char *member, int source)
{
	struct dbm_arg a[1] = { { 'i', source, 0 } };
	io_call(AU_DEST, AU_PATH, AU_IFACE, member, a, 1);
}

static void clear_gui(void)
{
	if (ui_ok) {
		ui_nav_hide();	/* the layer goes clear: CarPlay shows through */
		ui_display_tick();
		return;
	}
	io_fb_clear();
}

static void start_carplay(void)
{
	phones_reload();
	if (!nphones) {
		say("no paired phone: CarPlay not started");
		next_try = now() + 30;
		return;
	}
	if (target < 0 || target >= nphones) {
		target = 0;
		for (int i = 0; i < nphones; i++)
			if (phones[i].last)
				target = i;
	}
	snprintf(lastaddr, sizeof(lastaddr), "%s", phones[target].addr);
	/* carplay parses the address with QString::mid(0/3/6/9/12/15, 2) and
	 * toInt(16): it must be "AA:BB:CC:DD:EE:FF". gocsdk stores 12 hex
	 * digits. A wrong form fails later as "bt addr is not same". */
	char colon[18];
	size_t len = strlen(lastaddr);
	if (len == 12) {
		for (int i = 0; i < 6; i++) {
			colon[i * 3] = lastaddr[i * 2];
			colon[i * 3 + 1] = lastaddr[i * 2 + 1];
			colon[i * 3 + 2] = i < 5 ? ':' : 0;
		}
	} else if (len == 17) {
		memcpy(colon, lastaddr, 18);
	} else {
		say("unexpected address format (length %zu)", len);
		next_try = now() + 30;
		return;
	}
	if (names_stale) {	/* the unit was renamed while this carplay ran */
		names_stale = 0;
		io_carplay_end();
		say("carplay ended: a fresh one takes the unit's new name");
	}
	/* Driving side, as stock Launcher: /etc/driverDirection ("1" = right-hand
	 * drive; "Steering side" in Display since) becomes
	 * CARPLAY_RIGHTHAND_DRIVER in carplay's environment (requsetSetEnvValue,
	 * sic: carplay's spelling), before every start. */
	struct dbm_arg env[3] = { { 'i', 7, 0 }, { 's', 0, "CARPLAY_RIGHTHAND_DRIVER" },
				  { 's', 0, rhd ? "1" : "0" } };
	io_call(CP_DEST, CP_PATH, CP_IFACE, "requsetSetEnvValue", env, 3);
	say("driving side: %s", rhd ? "right-hand" : "left-hand");
	struct dbm_arg a[3] = { { 'i', 7, 0 }, { 's', 0, colon }, { 'i', 1, 0 } };
	io_call(CP_DEST, CP_PATH, CP_IFACE, "requestSetPhoneBTAddr", a, 3);
	cp_link(3);
	if (state == SWITCHING)
		switch_started = 1;
	else
		state = STARTING;
	returning = 0;		/* a new session: its first frames are garbled */
	phone_released = 0;
	start_at = now();
	say("CarPlay start requested (phone %d of %d)", target + 1, nphones);
}

/* CarPlay went down or a start failed (not our own stop). The phone has to
 * be on hands-free for the next start; a start that never came up counts,
 * three in a row give up (any phone may connect then). */
static void carplay_down(void)
{
	int was_switch = state == SWITCHING, was_up = state == UP;

	audio("releaseAudioSource", 4);
	switch_started = 0;
	start_fails = was_up ? 0 : start_fails + 1;
	if (!was_switch)
		state = IDLE;
	if (!was_switch && !watching) {
		/* No live stream behind the video layer any more: it shows a stale
		 * frame read with the wrong layout (owner's photo, 2026-11-20).
		 * Home takes the layer, and the picture goes to the car. */
		idle_screen();
		picture_to_car();
	}
	if (start_fails > 2) {
		say("CarPlay: 3 failed starts on phone %d", target + 1);
		give_up(target, 0);
		return;
	}
	if (!bt_released && phone_connected && bt_phone == target) {
		say("CarPlay start failed (%d): retry in 3 s", start_fails);
		next_try = now() + 3;
	} else {
		say("CarPlay down: seeking phone %d", target + 1);
		seek(target, 2, was_switch, 0);
	}
	if (watching && ui_ok && target >= 0 && target < nphones)
		ui_nav_wait(WAIT_RETRYING, phones[target].name);
}

/* The unit has given up on a phone (design/screens.md, Connecting): if the
 * driver is watching, "Couldn't connect" for 3 s and then Home; the phone is
 * "Unreachable" until the next attempt. No other phone may connect by
 * itself. page_again: the phone was not found, and if it is the profile's it
 * is paged again every 30 s. Not after failed starts: there the phone is on
 * hands-free and CarPlay will not come up, and asking it again and again
 * could make iOS ignore the unit; the driver's tap tries again. */
static void give_up(int phone, int page_again)
{
	if (phone >= 0 && phone < nphones)
		set_unreachable(phones[phone].addr, 1);
	if (!page_again)
		auto_phone = -1;
	state = IDLE;
	switch_started = 0;
	start_fails = 0;
	next_try = 0;
	idle_bluetooth();
	if (!ui_ok)
		return;
	if (watching && phone >= 0 && phone < nphones) {
		play(SND_LOST);
		ui_nav_wait(WAIT_FAILED, phones[phone].name);
		failed_until_ms = now_ms() + 3000;
		failed_phone = phone;
	} else {
		watching = 0;
		if (ui_frame_layer_now() == LAYER_SCREEN)
			ui_sync();
		else
			idle_screen();
	}
}

static void link_status(int type, int status)
{
	say("carplay status %d/%d", type, status);
	if (type != 7)
		return;
	switch (status) {
	case 3:
		mcu1(0x04, 0x02);
		break;
	case 4:
		if (state == SWITCHING && !switch_started) {
			say("old phone's session came up during the switch: ending it");
			cp_link(7);
			break;
		}
		mcu1(0x04, 0x02);
		phone_screen = 1;
		phone_released = 0;
		if (state != UP)
			returning = 0;
		if (reveal_at_ms)
			reveal_after_frames();
		if (state != UP) {
			state = UP;
			if (save_after_up) {
				save_after_up = 0;
				save_soon("first session of a new phone");
			}
			start_fails = 0;
			watching = 0;
			failed_until_ms = 0;
			if (target >= 0 && target < nphones)
				set_unreachable(phones[target].addr, 0);
			/* Bluetooth off while CarPlay runs, as stock: no phone on
			 * hands-free, so a passenger's calls stay on their phone. */
			bt_release();
			audio("requestAudioSource", 4);
			/* Black cover while the first frames decode, then reveal
			 * (with the switch banner if another phone is paired). */
			if (ui_ok) {
				if (!waiting_shown())
					ui_nav_cover();	/* the waiting screen, if it is up, stays as the cover */
				reveal_after_frames();
				banner_pending = nphones > 1 && target >= 0;
			} else {
				clear_gui();
			}
			picture_to_unit();
			if (night >= 0)
				cp_link(night ? 31 : 32);
			say("CarPlay up: picture to unit");
			play(SND_CONNECTED);
		}
		break;
	case 62:
		phone_screen = 1;
		phone_released = 0;
		if (state != UP)
			returning = 0;
		/* A starting session takes the screen (62) ~0.3 s before it reports
		 * up (4), its first frames still garbled. The cover used to go
		 * here, so the frame wait from 4 never covered anything (owner,
		 * 2026-11-20: distorted frames into the new CarPlay): now it stays
		 * until the stream has settled. */
		if (reveal_at_ms || state == SWITCHING || state == STARTING) {
			/* Anything but the waiting screen gives way to the cover:
			 * Home was drawn behind the car's picture, and the picture
			 * is about to come to the unit. */
			if (ui_ok && !waiting_shown() && ui_frame_layer_now() != LAYER_GROUND) {
				ui_nav_cover();
				ui_display_draw_now();
			}
			reveal_after_frames();
		} else {
			clear_gui();
		}
		picture_to_unit();
		break;
	case 12:	/* CarPlay call starts: call audio source, as stock */
		audio("requestAudioSource", 5);
		say("call audio");
		break;
	case 13:	/* call ends: back to projection media */
		audio("releaseAudioSource", 5);
		if (state == UP)
			audio("requestAudioSource", 4);
		say("call ended");
		break;
	case 63:	/* CarPlay's car icon: the phone hands the screen back */
		phone_screen = 0;
		phone_released = 1;
		if (state == SWITCHING)	/* the old phone leaving: keep "Connecting" */
			break;
		/* The session stays alive behind the car's picture: the cover, so
		 * that the MCU's own switch back (a long MENU) lands on black and
		 * not on a stale frame, until CarPlay is in front again. */
		if (ui_ok) {
			ui_nav_cover();
			ui_display_draw_now();
		}
		picture_to_car();
		break;
	case 2: case 5: case 7: case 8:
		/* The old session's statuses after our own stop (5, -1001, 8 within
		 * a second) are not the new start failing (2026-11-20). */
		if (state == SWITCHING && (!switch_started || now() - stopped_at <= 3)) {
			audio("releaseAudioSource", 4);
			break;
		}
		if (state != IDLE)
			carplay_down();
		break;
	}
}

/* Pairing over without a new phone. Not CB: gocsdk's CB makes the unit
 * invisible *and* not connectable, so no phone could connect afterwards.
 * With CarPlay running, Bluetooth goes off again (passenger stays out). */
static void pairing_end(void)
{
	if (state == UP)
		bt_release();
	else
		idle_bluetooth();
}

/* CarPlay, alive behind the car's picture or behind Home, back to the front:
 * the cover until the phone has the screen again (the video layer shows a
 * stale frame until the first full frame is decoded). */
static void carplay_to_front(void)
{
	if (ui_ok) {
		ui_nav_cover();
		ui_display_draw_now();
	}
	cp_link(3);	/* carplay replies 4, the phone 62 */
	picture_to_unit();
	returning = 1;
	if (!phone_released) {
		/* The phone kept the screen (a short MENU): nothing changes for
		 * it, so no status comes (bench, 2026-11-22: 5.0 s of black, the
		 * fallback below), and the layer is current: the cover goes at
		 * the next tick. */
		reveal_at_ms = now_ms();
		reveal_frames = -1;
		reveal_limit_ms = now_ms();
		phone_screen = 1;	/* it never lost it: touches and MENU are CarPlay's again at once */
		say("CarPlay to the front: the phone kept the screen");
		return;
	}
	reveal_at_ms = now_ms() + 5000;	/* fallback if no status comes */
	reveal_frames = -1;
	say("CarPlay to the front: asking the phone for the screen");
}

/* Connect phone i and start CarPlay on it, with the driver watching; or
 * PICK_PAIR, PICK_CANCEL for pairing. */
static void switch_to(int i)
{
	if (i == PICK_PAIR) {
		pair_known = phones_reload();
		for (int k = 0; k < pair_known; k++)
			memcpy(pair_before[k], phones[k].addr, sizeof(pair_before[k]));
		pair_keys_len = read_file(rp("/etc/goc/sc_db.db"), pair_keys, sizeof(pair_keys));
		/* Visible and connectable; with Bluetooth off (CarPlay running) the
		 * mode is applied on IS1 - a CA right after P1 is lost (2026-11-20). */
		bt_power_on();
		bt_set_mode(BT_PAIR);
		pairing = 1;
		pair_until = now() + 180;
		watching = 1;
		if (ui_ok)
			ui_nav_wait(WAIT_PAIRING, unit_name);
		say("pairing mode on (%d phones known)", pair_known);
		return;
	}
	if (i == PICK_CANCEL) {
		pairing = 0;
		watching = 0;
		pairing_end();
		say("pairing cancelled");
		if (ui_ok)
			show_home(nphones);	/* the ring back on "Pair a phone" */
		return;
	}
	if (i < 0 || i >= nphones)
		return;
	if (i == target && (state == UP || state == STARTING)) {
		if (state == UP) {
			carplay_to_front();
		} else if (ui_ok) {	/* already on its way: the driver watches it now */
			watching = 1;
			ui_nav_wait(WAIT_CONNECTING, phones[i].name);
		}
		say("phone %d already %s", i + 1, state == UP ? "connected" : "starting");
		return;
	}
	say("switching to phone %d", i + 1);
	switch_started = 0;
	start_fails = 0;
	failed_until_ms = 0;
	target = i;
	/* The waiting screen first, drawn at once: stopping CarPlay and the
	 * access point reload below cut the video stream, and the decoder's
	 * broken frames (distorted, green) showed through the still transparent
	 * layer for up to 1 s until the next tick drew "Connecting" (owner,
	 * 2026-11-20). */
	watching = 1;
	if (ui_ok) {
		long t0 = now_ms();

		ui_nav_wait(WAIT_CONNECTING, phones[i].name);
		ui_display_draw_now();
		say("waiting screen drawn in %ld ms", now_ms() - t0);
	}
	picture_to_unit();
	/* Full stop (24: carplay_stop, the process exits) rather than 7: after
	 * "exit link" carplay still counted the old phone as linked and answered
	 * the new start with "4" for it (2026-11-20). The next call starts a fresh
	 * carplay through D-Bus activation. */
	if (state == UP || state == STARTING) {
		cp_link(24);
		stopped_at = now();
	}
	/* New access point passphrase, every client off: the phone left
	 * behind cannot rejoin a network without internet. */
	io_rotate_ap();
	state = SWITCHING;
	/* The other phone stays out (not connectable) while this one is paged. */
	seek(i, 3, 1, switch_grace_ms);
	switch_grace_ms = 0;
}

/* PROFILE_PHONES "11:1 00:2 aa:0": places in the list as it stands when the
 * unit starts, so it is read once the phones are known; PROFILE_SWITCH. */
static void profile_config(void)
{
	const char *m = getenv("PROFILE_PHONES"), *sw = getenv("PROFILE_SWITCH");

	memset(profile_addr, 0, sizeof(profile_addr));
	if (sw && sw[0] == '0')
		profile_switch = 0;
	while (m && *m) {
		unsigned p, n;
		int used = 0;

		if (sscanf(m, " %2x:%u%n", &p, &n, &used) != 2 || !used || p > 0xff || n > MAX_PHONES)
			break;
		if (!n) {
			strcpy(profile_addr[p], "-");
			say("car profile %02x -> no CarPlay", p);
		} else {
			if ((int)n <= nphones)
				memcpy(profile_addr[p], phones[n - 1].addr, sizeof(profile_addr[p]));
			say("car profile %02x -> phone %u", p, n);
		}
		m += used;
	}
	if (profile_switch == 0)
		say("car profile applies at start-up only");
}

/* A no-CarPlay profile (Guest): stop CarPlay if it runs, Bluetooth off so no
 * phone connects by itself, every Wi-Fi client off, Home. A phone chosen
 * there still starts (explicit choice), and is never kept. */
static void profile_no_carplay(int p)
{
	say("car profile %02x: no CarPlay, staying in the launcher", p);
	if (state == UP || state == STARTING || state == SWITCHING) {
		cp_link(24);	/* full stop; its down statuses find state IDLE */
		stopped_at = now();
		audio("releaseAudioSource", 4);
		io_rotate_ap();
	}
	state = IDLE;
	switch_started = 0;
	next_try = 0;
	seek_active = 0;
	seek_deadline_ms = 0;
	auto_phone = -1;
	auto_page_at = 0;
	watching = 0;
	failed_until_ms = 0;
	bt_release();
	target = -1;
	/* Home, wherever the picture is: the driver was in CarPlay and sees it
	 * at once; at ignition it waits behind the car's picture. */
	if (ui_ok)
		show_home(UI_FOCUS_DEFAULT);
}

/* The car has no phone for this driver (or nothing is to connect by itself):
 * nothing more connects unasked. A session that runs stays. */
static void profile_without_phone(void)
{
	auto_phone = -1;
	auto_page_at = 0;
	if (state == IDLE && !seek_active && !pairing && !watching)
		bt_release();
	ui_sync();
}

static void profile_report(int p, int fresh)
{
	if (!fresh)	/* the previous drive's profile: the car's own report follows ignition on */
		return;
	if (p != car_profile) {
		car_profile = p;
		say("car profile %02x", p);
	}
	if (p == profile_applied)
		return;
	int first = profile_applied < 0;
	profile_applied = p;
	decided = 1;
	int m = profile_phone(p);
	if (m == PROFILE_UNMAPPED) {
		say("car profile %02x: no phone for it; waiting for a choice", p);
		profile_without_phone();
		return;
	}
	if (!first && !profile_switch) {
		say("car profile %02x: not applied (start-up only)", p);
		ui_sync();
		return;
	}
	if (pairing) {
		say("car profile %02x: pairing in progress, not applied", p);
		return;
	}
	if (m == PROFILE_NONE) {
		profile_no_carplay(p);
		return;
	}
	if (m >= nphones) {
		say("car profile %02x: phone %d is not paired", p, m + 1);
		profile_without_phone();
		return;
	}
	if (!model.set[SET_CONNECT_AT_IGNITION]) {
		say("car profile %02x: phone %d, but nothing connects by itself", p, m + 1);
		profile_without_phone();
		return;
	}
	auto_phone = m;
	if (m == target && (state == UP || state == STARTING || state == SWITCHING ||
			    (seek_active && seek_phone == m))) {
		say("car profile %02x: phone %d already chosen", p, m + 1);
		ui_sync();
		return;
	}
	say("car profile %02x: phone %d", p, m + 1);
	if (state == IDLE) {
		/* In the background: Home, if anyone looks, shows it as it is. */
		target = m;
		seek(m, 2, 0, 0);
		ui_sync();
	} else {
		switch_to(m);	/* CarPlay runs on another phone: through "Connecting" */
	}
}

/* ---- what the driver asks for (launcher/ui/nav.h) ---- */

/* Back while connecting: stop, and Home. A manual stop stands: nothing
 * connects by itself afterwards. */
static void cancel_connect(void)
{
	int phone = target;

	say("connecting cancelled");
	if (state == STARTING || (state == SWITCHING && switch_started)) {
		cp_link(24);
		stopped_at = now();
	}
	state = IDLE;
	switch_started = 0;
	seek_active = 0;
	seek_deadline_ms = 0;
	next_try = 0;
	watching = 0;
	failed_until_ms = 0;
	auto_phone = -1;
	auto_page_at = 0;
	bt_release();
	show_home(phone);
}

static void disconnect(void)
{
	say("disconnect asked for");
	if (state == UP || state == STARTING || state == SWITCHING) {
		cp_link(24);	/* full stop; its down statuses find state IDLE */
		stopped_at = now();
		audio("releaseAudioSource", 4);
		io_rotate_ap();
	}
	state = IDLE;
	switch_started = 0;
	seek_active = 0;
	seek_deadline_ms = 0;
	next_try = 0;
	auto_phone = -1;
	auto_page_at = 0;
	bt_release();
	model_update();	/* the navigation draws the list again after this */
}

/* Which phone a driver profile brings up, kept by the phone's address once
 * the list has changed on the unit (PROFILE_PHONES counts places in the
 * list, and those move when a phone is forgotten). */
#define PROFILE_FILE "/data/autotop-profiles"

static void profile_store(void)
{
	char text[256];
	size_t n = 0;

	for (int c = 0; c < 256 && n + 24 < sizeof(text); c++)
		if (profile_addr[c][0])
			n += snprintf(text + n, sizeof(text) - n, "%s%02x %s", n ? "\n" : "", c, profile_addr[c]);
	text[n] = 0;
	io_put(PROFILE_FILE, text);
}

/* The kept mapping stands in place of PROFILE_PHONES. */
static void profile_restore(void)
{
	char text[256], *line, *save = NULL;

	if (read_file(rp(PROFILE_FILE), text, sizeof(text)) <= 0)
		return;
	memset(profile_addr, 0, sizeof(profile_addr));
	for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		unsigned code;
		char addr[24];

		if (sscanf(line, "%2x %23s", &code, addr) != 2 || code > 0xff)
			continue;
		if (!strcmp(addr, "-"))
			strcpy(profile_addr[code], "-");
		else
			phones_colon(addr, profile_addr[code]);	/* left empty if it is no address */
	}
	say("driver profiles from the kept mapping");
}

/* Forget phone i: off the screen at once; gocsdk is told once Bluetooth is
 * on and has listed its phones (forget_send); then the list is read again. */
static void forget_phone(int i)
{
	if (i < 0 || i >= nphones || forget_phase != FORGET_NONE)
		return;
	say("forget asked for (phone %d of %d)", i + 1, nphones);
	if (i == target && state != IDLE)
		disconnect();	/* it is in CarPlay, or on its way there */
	memcpy(forget_addr, phones[i].addr, sizeof(forget_addr));
	forget_phase = FORGET_WAITING;
	forget_at_ms = now_ms() + 9000;	/* Bluetooth takes about 5 s to come on */
	phones_drop(i);
	if (bt_released) {	/* gocsdk hears nothing while Bluetooth is off */
		forget_bt = 1;
		bt_power_on();
		bt_mode = BT_CLOSED;	/* nobody may connect meanwhile */
	}
	bt_apply();
	model_update();	/* the navigation draws the list again after this */
}

/* Some time after AT#CV: what gocsdk's list says now. */
static void forget_check(void)
{
	int still;

	phones_reload();
	still = phone_at(forget_addr);
	if (still >= 0) {
		say("forget: the phone is still paired (%d phones)", nphones);
		play(SND_DENY);
	} else {
		say("phone forgotten (%d phones left)", nphones);
		play(SND_OFF);
		for (int c = 0; c < 256; c++)	/* its profile is free */
			if (!strcmp(profile_addr[c], forget_addr))
				profile_addr[c][0] = 0;
		profile_store();
		save_soon("forget");
	}
	forget_phase = FORGET_NONE;
	if (forget_bt && !pairing && !seek_active && (state == IDLE || state == UP))
		bt_release();	/* as it was: off while nobody is being connected */
	forget_bt = 0;
	model_update();
	ui_sync();
	if (still >= 0 && ui_ok)	/* its row is back: say why */
		ui_nav_message("Couldn\xE2\x80\x99t forget ", phones[still].name);
}

/* carplay's process gone under a running session (it crashed, or was ended
 * from outside): it reports nothing, the video layer keeps its last frame
 * and nothing is behind it (bench, 2026-11-23: a frozen CarPlay picture
 * until the launcher was restarted). Looked for once a second; gone twice in
 * a row is taken as the session's own end. */
static void carplay_watch(void)
{
	static long next_ms;
	static int gone;

	if (state != UP) {
		gone = 0;
		return;
	}
	if (now_ms() < next_ms)
		return;
	next_ms = now_ms() + 1000;
	if (io_carplay_running()) {
		gone = 0;
	} else if (++gone >= 2) {
		gone = 0;
		say("carplay's process is gone under its session: the session has ended");
		link_status(7, 5);
	}
}

/* ---- the app's update, over the unit's own Wi-Fi ----
 *
 * "Start" on the Software update page starts the receiver (launcher/update.h)
 * and shows how the phone joins the unit's network and where to send the
 * file. The receiver hands the file to app-slot, which alone decides (the
 * signature, the version); its answer is the line on the screen. A bundle
 * that was taken runs from the next start: the unit restarts. Nothing is
 * listening before "Start" or after the screen is left. */
#define UPDATE_ADDR "192.168.43.1"	/* the unit on its own network (stock /etc/hostapd.sh) */
static int updating;		/* 1: the receiver listens; 2: a bundle is in, the restart is due */
static long update_poll_ms, update_restart_ms, update_join_ms;
static int update_clients;	/* phones on the access point when the screen came up */
static char update_line[160];

static void update_start(void)
{
	char ssid[40], psk[72];

	if (io_update_start()) {
		say("update: the receiver did not start");
		return;
	}
	updating = 1;
	update_line[0] = 0;
	update_poll_ms = 0;
	update_clients = io_ap_clients();
	update_join_ms = now_ms() + 2000;
	io_ap_info(ssid, sizeof(ssid), psk, sizeof(psk));
	ui_nav_receive(ssid, psk, UPDATE_ADDR, "Waiting for the update file");
	memset(psk, 0, sizeof(psk));	/* on the screen only; never in the log */
	say("update: waiting for a bundle over the unit's Wi-Fi");
}

static void update_stop(const char *why)
{
	if (!updating)
		return;
	io_update_stop();
	updating = 0;
	say("update: no longer waiting (%s)", why);
}

static void update_tick(void)
{
	struct ui_nav_state s;
	char line[160], words[120];
	long got, of;

	if (updating == 2) {
		if (now_ms() >= update_restart_ms) {
			updating = 0;
			say("update: restarting into the new app");
			io_update_stop();
			io_reboot();
		}
		return;
	}
	if (updating != 1)
		return;
	ui_nav_state(&s);
	if (s.page != PAGE_RECEIVE) {	/* CarPlay came up, or the car took the screen */
		update_stop("its screen is gone");
		return;
	}
	/* Step 1 shows how to join the Wi-Fi; when a phone has joined, the
	 * address follows by itself ("Next" is there for a phone that was on
	 * the Wi-Fi already, as the one in CarPlay is). Asked every 2 s: the
	 * question takes a moment and holds the screen while it does. */
	if (ui_nav_receive_step() == 1 && now_ms() >= update_join_ms) {
		int n = io_ap_clients();

		update_join_ms = now_ms() + 2000;
		if (n >= 0 && update_clients >= 0 && n > update_clients) {
			say("update: a phone has joined (%d -> %d): the address", update_clients, n);
			ui_nav_receive_next();
		} else if (n >= 0) {
			update_clients = n;	/* one that left may come back: it counts as joining */
		}
	}
	if (now_ms() < update_poll_ms)
		return;
	update_poll_ms = now_ms() + 400;
	if (!io_update_status(line, sizeof(line)) || !strcmp(line, update_line))
		return;
	if (strcmp(line, "waiting"))	/* a file is coming: whoever sends it has the address */
		ui_nav_receive_next();
	if (strncmp(line, "receiving ", 10) || strncmp(update_line, "receiving ", 10))
		say("update: %s", line);	/* each change but the count going up */
	snprintf(update_line, sizeof(update_line), "%s", line);
	if (!strncmp(line, "done ", 5)) {
		updating = 2;
		update_restart_ms = now_ms() + 3000;	/* the phone has its answer by then */
		ui_nav_wait(WAIT_UPDATING, "");
		return;
	}
	if (sscanf(line, "receiving %ld of %ld", &got, &of) == 2 && of > 0) {
		snprintf(words, sizeof(words), "Receiving the file: %ld%%", got * 100 / of);
	} else if (!strcmp(line, "checking")) {
		snprintf(words, sizeof(words), "Checking the file");
	} else if (!strncmp(line, "refused ", 8)) {
		const char *why = strstr(line, "refused: ") ? strstr(line, "refused: ") + 9 :
				  strstr(line, "failed: ") ? strstr(line, "failed: ") + 8 : line + 8;

		snprintf(words, sizeof(words), "Not taken: %.100s", why);
	} else {
		snprintf(words, sizeof(words), "Waiting for the update file");
	}
	ui_nav_receive("", "", "", words);
}

/* The app that runs has been up a minute with its screen: it is good. For
 * one that came in as an update this ends its trial (app-slot); for any
 * other it does nothing. */
static void app_confirm_tick(void)
{
	static long since;
	static int said;

	if (said)
		return;
	if (!since)
		since = now_ms();
	if (now_ms() - since >= 60000) {
		said = 1;
		io_app_confirm();
	}
}

/* The Bluetooth mark follows Bluetooth, whichever of its many paths moved it. */
static void bt_mark_tick(void)
{
	if (ui_ok && model.bt != bt_mark())
		ui_sync();
}

/* What a push cost, for the log: the wait before the move is what is felt. */
static void push_tick(void)
{
	static unsigned logged;
	struct ui_display_stats ds;

	if (!ui_ok)
		return;
	ui_display_stats(&ds);
	if (ds.moved != logged) {
		logged = ds.moved;
		say("push %u: drawn %u ms after the request, copied in %u ms, moved in %u ms", ds.moved, ds.draw_ms, ds.copy_ms, ds.move_ms);
	}
}

static void forget_tick(void)
{
	if (forget_phase == FORGET_NONE || now_ms() < forget_at_ms)
		return;
	if (forget_phase == FORGET_WAITING && bt_ready) {
		forget_send();	/* no listing came: by gocsdk's file alone */
		return;
	}
	if (forget_phase == FORGET_WAITING)
		say("forget: Bluetooth did not come on: nothing dropped");
	forget_check();
}

/* One name for Bluetooth, the access point and CarPlay's list
 * (design/screens.md, Settings). Pairings do not hang on it: a phone knows
 * the unit by its address and keys, and is handed the access point's name
 * and passphrase at every CarPlay start. The log never carries the name. */
static void rename_unit(const char *typed)
{
	char name[sizeof(unit_name)];
	size_t n;

	while (*typed == ' ')
		typed++;
	snprintf(name, sizeof(name), "%s", typed);
	for (n = strlen(name); n && name[n - 1] == ' '; n--)
		name[n - 1] = 0;
	if (!n || !strcmp(name, unit_name))
		return;
	snprintf(unit_name, sizeof(unit_name), "%s", name);
	/* Kept: the file the next start reads, and carplay's wrapper at each
	 * of its starts. */
	io_put(NAME_FILE, unit_name);
	save_soon("name");
	/* Bluetooth: now if it is on, else when it next is (a command sent
	 * while it is off is lost). */
	name_pending = 1;
	bt_apply();
	/* The access point: in its configuration now. hostapd reads it at the
	 * next passphrase change: at once with nobody in CarPlay, otherwise at
	 * the next switch, so a running session is not cut. */
	io_ap_name(unit_name);
	if (state == IDLE)
		io_rotate_ap();
	/* CarPlay: carplay reads its names once, when its process starts, and
	 * the process stays through stops and switches. So a running one is
	 * ended: now with nobody in CarPlay, otherwise just before the next
	 * start. D-Bus starts a fresh one at the next call. */
	if (io_carplay_running()) {
		if (state == IDLE)
			io_carplay_end();
		else
			names_stale = 1;
	}
	say("unit renamed (%d characters): Bluetooth %s, access point %s, CarPlay %s", (int)n,
	    name_pending ? "when it is next on" : "now", state == IDLE ? "now" : "at the next switch",
	    state == IDLE ? "at its next start" : "after this session");
}

static void on_event(enum ui_event ev, int arg, int value)
{
	switch (ev) {
	case UI_EV_PHONE:
	case UI_EV_SWITCH:
		auto_phone = -1;	/* the driver has chosen: that stands */
		auto_page_at = 0;
		switch_to(arg);
		break;
	case UI_EV_PAIR:
		switch_to(PICK_PAIR);
		break;
	case UI_EV_CANCEL:
		if (updating == 1) {
			update_stop("cancelled");
			break;
		}
		if (pairing)
			switch_to(PICK_CANCEL);
		else
			cancel_connect();
		break;
	case UI_EV_LEAVE:
		say("launcher left: the car's picture");
		if (state == UP)	/* a session is alive behind: see status 63 */
			ui_nav_cover();
		picture_to_car();
		break;
	case UI_EV_DISCONNECT:
		disconnect();
		break;
	case UI_EV_SETTING:
		if (arg == SET_BRIGHTNESS) {
			mcu1(0x61, value * 99 / 100);	/* the MCU's backlight: 0-99 */
			say("brightness %d", value);
		} else if (arg == SET_STEERING_RIGHT) {
			rhd = value;	/* CarPlay's layout follows at its next start */
			io_put("/etc/driverDirection", rhd ? "1" : "0");
			save_soon("setting");
			say("steering side: %s", rhd ? "right" : "left");
		} else if (arg == SET_EFFECTS) {
			say("visual effects: %s", value ? "on" : "off");
			settings_store();
		} else if (arg == SET_SOUNDS) {
			say("launcher sounds: level %d", value);
			settings_store();
		} else if (arg == SET_DEVELOPER) {	/* kept by the system's own script, not with our settings */
			say("developer access: %s", value ? "on" : "off");
			io_dev_access(value);
			/* On: the passphrase that will stay, in force and on the
			 * update page's first step before anyone types it. Off: a
			 * new one, so the one that stayed is dead. With CarPlay
			 * running, at the next switch: a session is not cut for it. */
			if (state == IDLE)
				io_rotate_ap();
			save_soon("setting");
		}
		break;
	case UI_EV_REBOOT:
		say("reboot asked for");
		io_reboot();
		break;
	case UI_EV_NAME:
		rename_unit(ui_nav_entered());
		model_update();	/* the navigation draws the list again after this */
		break;
	case UI_EV_FORGET:
		forget_phone(arg);
		break;
	case UI_EV_SOUND:
		play(arg);
		break;
	case UI_EV_UPDATE:
		update_start();
		break;
	case UI_EV_PROFILE:
		break;	/* not wired yet: its row is switched off */
	}
}

/* The iDrive controller, from the MCU (D0 4B 81 <t> <k> <s>; codes from the
 * car, 2026-11-21) or, at the bench, the control file: l, r one detent; p
 * push; b Back; m short MENU. */
void core_key(char c)
{
	if (!ui_ok)
		return;
	if (pic_source == 0 && ui_frame_layer_now() == LAYER_SCREEN) {
		long t0 = now_ms();

		if (c == 'm')	/* MENU is the car's: its picture */
			on_event(UI_EV_LEAVE, 0, 0);
		else if (c == 'l' || c == 'r' || c == 'p' || c == 'b')
			ui_nav_key(c == 'l' ? KEY_LEFT : c == 'r' ? KEY_RIGHT : c == 'p' ? KEY_PUSH : KEY_BACK);
		/* The answer is on the panel before anything else is done, and
		 * the log says how long it took. */
		ui_display_draw_now();
		say("key %c: answered in %ld ms", c, now_ms() - t0);
	} else if (c == 'm' && state == UP && phone_screen && pic_source == 0) {
		/* Short MENU in CarPlay: the BMW picture, like CarPlay's car icon.
		 * The session stays alive behind it (the phone is not told). */
		say("MENU in CarPlay: the car's picture");
		phone_screen = 0;
		ui_nav_cover();
		ui_display_draw_now();
		picture_to_car();
	}
	/* The knob inside CarPlay is CarPlay's: not passed on yet. */
}

/* ---- touch ---- */

static void send_touch(int type, int x, int y)
{
	static const int st[4] = { 0, 1, 2, 8 };
	char xml[256];

	snprintf(xml, sizeof(xml),
		 "<TouchEvent>\n <TouchPoint>\n  <Index:0>\n   <State>%d</State>\n"
		 "   <X>%d</X>\n   <Y>%d</Y>\n  </Index:0>\n </TouchPoint>\n</TouchEvent>\n",
		 st[type], x, y);
	struct dbm_arg a[3] = { { 'i', 0, 0 }, { 'i', type, 0 }, { 's', 0, xml } };
	io_call(CP_DEST, CP_PATH, CP_IFACE, "requestTouchStatus", a, 3);
}

/* The first finger, per gesture. The launcher's, if the unit's picture is
 * showing and the navigation says so (a launcher screen, the cover, the
 * banner and 24px around it); CarPlay's, if a session has the screen;
 * otherwise it is a touch on the car's picture, which brings Home. */
void core_touch(int type, int x, int y)
{
	static int owner;	/* per gesture: 1 the launcher, 2 CarPlay, 3 bring Home */

	if (type == 1)
		owner = ui_ok && pic_source == 0 && ui_nav_owns(x, y) ? 1 :
			state == UP && phone_screen ? 2 : 3;
	if (owner == 1)
		ui_display_touch(x, y, type != 3);
	else if (owner == 2)
		send_touch(type, x, y);
	else if (owner == 3 && type == 1) {
		if (ui_ok) {
			/* Drawn before the picture comes to the unit: behind the
			 * layer is CarPlay's last (stale) frame. */
			long t0 = now_ms();

			show_home(UI_FOCUS_DEFAULT);
			ui_display_draw_now();
			say("touch on the car's picture: Home, drawn in %ld ms", now_ms() - t0);
		}
		picture_to_unit();
	}
}

/* ---- what the unit calls (io.h) ---- */

/* A whole number from the configuration (PLANA.CFG keys reach us as
 * environment variables), within limits. */
static int config(const char *key, int fallback, int min, int max)
{
	const char *v = getenv(key);
	int n = v && *v ? atoi(v) : fallback;

	return n < min ? min : n > max ? max : n;
}

/* The settings. Until the unit has writable state they live in RAM: they
 * start from the configuration and hold until power-off. What is not wired
 * yet is shown switched off. */
static void settings_start(void)
{
	static const enum ui_setting off[] = {
		SET_CONNECT_AT_IGNITION,	/* needs writable state: CONNECT_AT_IGNITION in PLANA.CFG meanwhile */
		SET_VOLUME_MEDIA, SET_VOLUME_CALL, SET_VOLUME_NAVIGATION, SET_BASS, SET_MID, SET_TREBLE,	/* MCU 07, 0B */
	};
	char dir[8] = "0";

	if (read_file(rp("/etc/driverDirection"), dir, sizeof(dir)) > 0)
		rhd = dir[0] == '1';
	model.set[SET_CONNECT_AT_IGNITION] = config("CONNECT_AT_IGNITION", 1, 0, 1);
	model.set[SET_OFFER_OTHER_PHONE] = config("OFFER_OTHER_PHONE", 1, 0, 1);
	model.set[SET_EFFECTS] = config("VISUAL_EFFECTS", 1, 0, 1);	/* Full: the unit holds it (measured and seen, 2026-11-23) */
	model.set[SET_SOUNDS] = 1;	/* "Actions": everything but the per-detent tick */
	settings_restore();	/* what the driver chose stands */
	bench = config("BENCH", 0, 0, 1);
	model.set[SET_BRIGHTNESS] = 60;	/* the start frame's 61 3c */
	model.set[SET_VOLUME_MEDIA] = model.set[SET_VOLUME_CALL] = model.set[SET_VOLUME_NAVIGATION] = 50;
	for (size_t k = 0; k < sizeof(off) / sizeof(off[0]); k++)
		model.set_off[off[k]] = 1;
	model.name_off = model.profile_off = model.forget_off = 1;
	/* A name and a forgotten phone need the state to be kept; at the bench
	 * they can be tried without. */
	if (bench || io_state_kept())
		model.name_off = model.forget_off = 0;
	model.can_update = io_state_kept();	/* an update is kept on the state partition */
	int dev = io_dev_state();	/* the system's own word: it starts the server itself at boot */

	model.can_develop = dev & 1;
	model.set[SET_DEVELOPER] = (dev >> 1) & 1;
	model.kernel = "Linux 4.19";
	model.processor = "ARK1668";
	model.panel = "1280 \xC3\x97 480";
}

void core_begin(void)
{
	say("launcherd starting");
	start_ms = now_ms();
}

void core_start(int ui)
{
	if (io_carplay_running()) {
		say("carplay still running from before: full stop");
		cp_link(24);
		io_rotate_ap();
	}
	/* Stock's start frames: status, backlight 60, version, source fc. */
	mcu1(0x01, 0x00);
	mcu1(0x61, 0x3c);
	mcu1(0x02, 0x01);
	mcu1(0x04, 0xfc);
	if (access(rp("/tmp/bluetoothaddress"), F_OK))
		copy_file(rp("/data/bluetoothaddress"), rp("/tmp/bluetoothaddress"));
	bt_open();
	ui_ok = ui;
	say("UI %s", ui_ok ? "ready" : "unavailable (no /dev/fb0)");
	phones_reload();
	profile_config();
	profile_restore();
	/* Where the state is kept, the configured mapping is written down by
	 * address the first time, and the kept one stands from then on. */
	if (io_state_kept() && access(rp(PROFILE_FILE), F_OK)) {
		profile_store();
		save_soon("driver profiles");
	}
	settings_start();
	/* Start-up (owner, 2026-11-21): nothing connects by itself until the
	 * car has said which driver profile is active, and then only that
	 * profile's phone. Until then, and if the profile has none: no phone
	 * may connect, and Home is ready behind the car's picture, which is
	 * left alone. */
	if (ui_ok) {
		long t0 = now_ms();

		ui_nav_init(&model, on_event);
		show_home(UI_FOCUS_DEFAULT);
		ui_display_draw_now();	/* now, and timed: the first drawing also makes the glyphs */
		say("Home drawn in %ld ms (%d phones)", now_ms() - t0, nphones);
	}
	bt_set_mode(BT_CLOSED);
}

void core_flush(void)
{
	bt_flush();
}

int core_poll_ms(void)
{
	/* LVGL runs while anything of ours is drawn: a launcher screen (also
	 * behind the car's picture), the cover, the banner. */
	return (ui_ok && (ui_frame_layer_now() != LAYER_CLEAR || ui_banner_shown())) || reveal_at_ms ? 15 :
	       bt_qlen ? 50 : 1000;
}

void core_link_status(int type, int status)
{
	link_status(type, status);
}

/* The phone the pairing has just made, -1 none yet. Either its address was
 * not in the list when pairing began; or it was (its owner had forgotten the
 * car on it, so the list does not grow), it is on hands-free now, and
 * gocsdk's key file has changed. A known phone that only came back by itself
 * leaves the keys as they were, and is not it. */
static int paired_now(void)
{
	static char keys[sizeof(pair_keys)];
	int len;

	for (int i = 0; i < nphones; i++) {
		int known = 0;

		for (int j = 0; j < pair_known; j++)
			known |= !strcmp(phones[i].addr, pair_before[j]);
		if (!known)
			return i;
	}
	if (!phone_connected || bt_phone < 0)
		return -1;
	len = read_file(rp("/etc/goc/sc_db.db"), keys, sizeof(keys));
	return len > 0 && (len != pair_keys_len || memcmp(keys, pair_keys, len)) ? bt_phone : -1;
}

/* Once a second while the pairing screen is up. */
static void pairing_tick(void)
{
	int made;

	if (!pairing || now() == pair_check)
		return;
	pair_check = now();
	phones_reload();
	made = paired_now();
	if (made < 0 && now() <= pair_until)
		return;
	pairing = 0;
	if (made < 0) {
		say("pairing timed out (%d phones)", nphones);
		watching = 0;
		pairing_end();
		if (ui_ok)
			show_home(nphones);
		return;
	}
	say(nphones > pair_known ? "new phone paired (%d phones)" : "a phone the unit knows was paired again (%d phones)",
	    nphones);
	save_soon("pairing");
	save_after_up = 1;	/* carplay has the phone's keys once its first session is up */
	/* Straight into CarPlay on it, no second tap on its tile. Bluetooth
	 * stays on, with the pairing policy, for 6 s: the iPhone connects by
	 * itself right after pairing (switched off here, it reported
	 * "Connection Unsuccessful") and is paged only if it does not. */
	switch_grace_ms = 6000;
	switch_to(made);
}

void core_tick(void)
{
	if (save_at_ms && now_ms() >= save_at_ms) {
		save_at_ms = 0;
		io_state_save();
	}
	forget_tick();
	pairing_tick();
	push_tick();
	carplay_watch();
	bt_mark_tick();
	update_tick();
	app_confirm_tick();
	if (reveal_at_ms && now_ms() >= reveal_at_ms &&
	    (reveal_frames < 0 || io_vdec_frames() >= reveal_frames || now_ms() >= reveal_limit_ms)) {
		reveal_at_ms = 0;
		returning = 0;
		if (reveal_from_ms) {
			long f = io_vdec_frames();
			say("reveal: %ld frames in %ld ms%s", f >= 0 && reveal_from_frames >= 0 ? f - reveal_from_frames : -1,
			    now_ms() - reveal_from_ms, now_ms() >= reveal_limit_ms ? " (time limit)" : "");
			reveal_from_ms = 0;
		}
		long c0 = now_ms();
		/* Full: the cover fades away over the live picture; then the layer
		 * is drawn clear, unseen, and shows by its pixels again. */
		int fade = ui_ok && model.set[SET_EFFECTS];

		if (fade)
			ui_display_fade(1);
		clear_gui();
		if (fade) {
			ui_display_draw_now();
			ui_display_fade(0);
		}
		say("cover cleared in %ld ms", now_ms() - c0);
		if (ui_ok) {
			struct ui_display_stats ds;

			ui_display_stats(&ds);
			say("layer so far: %u areas copied (%u whole screens), %lu pixels", ds.copies, ds.full, ds.pixels);
		}
		if (banner_pending && ui_ok) {
			/* The navigation decides whether there is a banner: the
			 * setting, Guest, another phone to offer. */
			model_update();
			if (ui_nav_banner())
				say("banner up");
		}
		banner_pending = 0;
	}
	if (ui_ok)
		ui_display_tick();
	/* The car has not said who drives (no patched MCU, the bench): the
	 * wait is over, nothing connects by itself. */
	if (!decided && now_ms() >= start_ms + PROFILE_HOLD_MS) {
		decided = 1;
		say("no fresh car profile after %d ms: waiting for a choice", PROFILE_HOLD_MS);
		profile_without_phone();
	}
	/* "Couldn't connect" has been read: Home, the ring on that phone. */
	if (failed_until_ms && now_ms() >= failed_until_ms) {
		failed_until_ms = 0;
		watching = 0;
		if (ui_ok)
			show_home(failed_phone);
	}
	if (!io_bt_open() && now() % 5 == 0)
		bt_open();
	seek_tick();
	if ((state == STARTING || (state == SWITCHING && switch_started)) && now() - start_at > 40) {
		say("CarPlay start timed out");
		carplay_down();
	}
	/* A failed start with the phone still on hands-free: retry. */
	if (next_try && now() >= next_try && !pairing && (state == IDLE || state == SWITCHING)) {
		next_try = 0;
		if (phone_connected && bt_phone == target)
			start_carplay();
		else if (target >= 0)
			seek(target, 2, state == SWITCHING, 0);
	}
	/* The profile's phone was not there: page it again now and then, one
	 * quiet attempt each time (owner, 2026-11-22). Nobody else may connect. */
	if (auto_phone >= 0 && auto_phone < nphones && auto_page_at && now() >= auto_page_at && state == IDLE &&
	    !pairing && !seek_active && !watching) {
		auto_page_at = 0;
		target = auto_phone;
		seek(auto_phone, 1, 0, 0);
		ui_sync();
	}
	if (bench && now_ms() / 60000 != clock_minute) {	/* the stand-in clock runs */
		clock_minute = now_ms() / 60000;
		ui_sync();
	}
}
