/* SPDX-License-Identifier: MIT
 * launcherd on the unit: the devices, the session bus, the processes and
 * files that launcherd.c asks for through io.h, and the loop that feeds it.
 * No decisions are taken here.
 *
 * Runs as root outside the stock chroot (/mnt/stock). Logs codes only:
 * never Bluetooth names, addresses or PINs.
 */
#include "io.h"
#include "ap_words.h"
#include "sounds.h"
#include "display.h"
#include "update.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ROOT "/mnt/stock"
#define CP_IFACE "Local.DbusServer.Carplay"

#define CTL "/tmp/launcherd.ctl"

static struct dbm bus;
static int bus_ok, mcu = -1, goc = -1, touch_fd = -1, fb_fd = -1, ctl_fd = -1;

/* ---- clock, log, files ---- */

long io_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

void io_log(const char *line)
{
	puts(line);
	fflush(stdout);
}

const char *io_root(void)
{
	return ROOT;
}

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

/* Decoded video frames so far (decoder interrupts, /proc/interrupts). */
long io_vdec_frames(void)
{
	char buf[8192];
	int fd = open("/proc/interrupts", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		char *e = strchr(l, '\n');
		if (e)
			*e = 0;
		if (strstr(l, "vdec")) {
			char *c = strchr(l, ':');
			long v = c ? strtol(c + 1, NULL, 10) : -1;
			if (e)
				*e = '\n';
			return v;
		}
		if (e)
			*e = '\n';
	}
	return -1;
}

/* ---- MCU (via mcuproxy's pty; the proxy's allow-list still applies) ---- */

static void mcu_open(void)
{
	struct termios t;

	mcu = open("/tmp/mcu-pty", O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (mcu < 0) {
		say("no MCU pty (%s)", strerror(errno));
		return;
	}
	if (tcgetattr(mcu, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(mcu, TCSANOW, &t);
	}
}

int io_mcu_open(void)
{
	return mcu >= 0;
}

int io_mcu_write(const unsigned char *frame, int n)
{
	return write(mcu, frame, n) == n ? 0 : -1;
}

/* ---- Bluetooth (gocsdk's serial link) ---- */

int io_bt_open(void)
{
	return goc >= 0;
}

int io_bt_connect(void)
{
	/* gocsdk's /dev/goc_serial is an absolute link valid inside the chroot
	 * (e.g. /dev/pts/1): resolve it under ROOT. Outside, /dev/pts/N may be
	 * a different terminal (2026-11-19: it was the MCU proxy's). */
	char target[64], path[96];
	ssize_t n = readlink(ROOT "/dev/goc_serial", target, sizeof(target) - 1);

	if (n <= 0) {
		say("no goc_serial link yet");
		return -1;
	}
	target[n] = 0;
	snprintf(path, sizeof(path), "%s%s", target[0] == '/' ? ROOT : ROOT "/dev/", target);
	goc = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (goc < 0) {
		say("no goc_serial (%s)", strerror(errno));
		return -1;
	}
	return 0;
}

int io_bt_write(const char *line, int n)
{
	return write(goc, line, n) == n ? 0 : -1;
}

/* ---- D-Bus: carplay and audio ---- */

static int bus_connect(void)
{
	char a[256];

	if (read_file(ROOT "/tmp/dbus-address", a, sizeof(a)) < 0)
		return -1;
	a[strcspn(a, "\r\n")] = 0;
	if (dbm_connect(&bus, a))
		return -1;
	dbm_add_match(&bus, "type='signal',interface='" CP_IFACE "'");
	bus_ok = 1;
	say("bus connected");
	return 0;
}

static void bus_reset(void)
{
	dbm_close(&bus);
	bus_ok = 0;
	while (bus_connect())
		sleep(1);
}

void io_call(const char *dest, const char *path, const char *iface, const char *member,
		 const struct dbm_arg *a, int n)
{
	if (dbm_call(&bus, dest, path, iface, member, a, n)) {
		bus_reset();
		dbm_call(&bus, dest, path, iface, member, a, n);
	}
}

static void bus_drain(void)
{
	int type, n;
	int32_t v[8];
	char member[64];

	if (dbm_read(&bus, 0, &type, member, sizeof(member), v, &n)) {
		bus_reset();
		return;
	}
	if (type == 4 && !strcmp(member, "onLinkStatusChange") && n >= 2)
		core_link_status(v[0], v[1]);
}

/* ---- the GUI layer without a UI ---- */

void io_fb_clear(void)
{
	static unsigned char zero[1280 * 4];
	int fd = open("/dev/fb0", O_WRONLY);

	if (fd < 0)
		return;
	for (int y = 0; y < 480; y++)
		if (write(fd, zero, sizeof(zero)) != sizeof(zero))
			break;
	close(fd);
}

/* ---- processes and the access point ---- */

/* Process id by name (/proc/<pid>/comm), 0 if none runs. */
static int find_pid(const char *name)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int pid = 0;

	if (!d)
		return 0;
	while (!pid && (e = readdir(d))) {
		char path[300], comm[64] = "";
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		if (read_file(path, comm, sizeof(comm)) > 0) {
			comm[strcspn(comm, "\n")] = 0;
			if (!strcmp(comm, name))
				pid = atoi(e->d_name);
		}
	}
	closedir(d);
	return pid;
}

/* carplay is its own process: it outlives a launcherd restart, and its
 * session with the old phone then held the Wi-Fi and the video layer while
 * every new start failed (2026-11-20). */
int io_carplay_running(void)
{
	return find_pid("carplay") > 0;
}

/* End the carplay process (it takes its names only when it starts): asked
 * to go, given a second, then made to. Nobody is in CarPlay when this is
 * called. The next D-Bus call to it starts a fresh one. */
void io_carplay_end(void)
{
	int pid = find_pid("carplay");

	if (pid <= 0)
		return;
	kill(pid, SIGTERM);
	for (int k = 0; k < 20 && kill(pid, 0) == 0; k++)
		usleep(50000);
	if (kill(pid, 0) == 0) {
		kill(pid, SIGKILL);
		usleep(100000);
	}
}

/* Clients on the access point (count only; addresses never logged), -1 if
 * hostapd_cli gave nothing. */
static int ap_clients(void)
{
	FILE *f = popen("/bin/busybox chroot " ROOT " /usr/bin/hostapd_cli -p /var/run/hostapd all_sta 2>/dev/null"
			" | /bin/busybox grep -cE '^[0-9a-f]{2}(:[0-9a-f]{2}){5}$'", "r");
	int n = -1;

	if (f) {
		if (fscanf(f, "%d", &n) != 1)
			n = -1;
		pclose(f);
	}
	return n;
}

/* Every switch: a new random access point passphrase, so only the phone
 * CarPlay starts on next can join the unit's network (no internet there);
 * the phone left behind cannot rejoin and keeps its own Wi-Fi.
 *
 * carplay hands the phone ssid and wpa_passphrase read from
 * /etc/hostapd/hostapd.conf (libiap2link send_wifi_info_data, at every
 * start), the file hostapd runs from (hostapd -B /etc/hostapd/hostapd.conf).
 * hostapd is 2.3: hostapd_cli has no "reload" ("Unknown command", exit 0), so
 * until 2026-11-20 only the file changed - the old phone rejoined 7 s after
 * its deauthentication and the new one, given the file's passphrase, never
 * joined (3 starts of 30 s, then "Not reachable"). SIGHUP makes hostapd 2.3
 * read the file again (hostapd_reload_config), and it deauthenticates every
 * client itself (hostapd_clear_old); bench: 1 client -> 0, still ENABLED,
 * nobody back after 12 s. Passphrase never logged; RAM copy of /etc only. */
#define AP_ROTATE 1
#define AP_KEPT ROOT "/data/autotop-ap-kept"

/* Developer access is on (the system's own marks: recovery/plan-a/dev-access),
 * or was switched here a moment ago and its script has not got that far. */
static int dev_wanted = -1;

static int ap_keep(void)
{
	if (dev_wanted >= 0)
		return dev_wanted;
	return !access("/mnt/state/autotop/dev-access", F_OK) || !access("/tmp/dev-access.on", F_OK);
}

static void state_save_now(void);

static void rotate_ap_now(void)
{
	unsigned char r[AP_RANDOM];
	char psk[AP_PASSPHRASE_MAX] = "", cmd[256];
	int fd, made = 0, keep = ap_keep(), kept = 0;

	/* With developer access on, the passphrase is made once and stays
	 * (owner's decision, 2026-11-24): a developer's computer must find the
	 * unit again after every start without anyone typing. It is kept with
	 * the rest of /data. A phone left behind can rejoin for that time. */
	if (keep && read_file(AP_KEPT, psk, sizeof(psk)) > 0) {
		psk[strcspn(psk, "\r\n")] = 0;
		made = kept = strlen(psk) >= 8 && strspn(psk, "abcdefghijklmnopqrstuvwxyz0123456789") == strlen(psk);
	}
	/* Words and digits (ap_words.h): read off the screen and typed. */
	fd = made ? -1 : open("/dev/urandom", O_RDONLY);
	for (int k = 0; fd >= 0 && !made && k < 8; k++)
		made = read(fd, r, sizeof(r)) == (ssize_t)sizeof(r) && ap_passphrase(r, sizeof(r), psk, sizeof(psk));
	if (fd >= 0)
		close(fd);
	memset(r, 0, sizeof(r));
	if (!made) {
		say("no randomness: passphrase unchanged");
		return;
	}
	if (keep && !kept) {
		io_put("/data/autotop-ap-kept", psk);
		state_save_now();	/* this is a process of its own: it may wait */
	}
	snprintf(cmd, sizeof(cmd),
		 "/bin/busybox sed -i 's/^wpa_passphrase=.*/wpa_passphrase=%s/' " ROOT "/etc/hostapd/hostapd.conf", psk);
	memset(psk, 0, sizeof(psk));
	int written = system(cmd) == 0;
	memset(cmd, 0, sizeof(cmd));
	int before = ap_clients();
	int pid = find_pid("hostapd");
	int sent = written && pid > 0 && kill(pid, SIGHUP) == 0;
	int after = before;

	/* hostapd drops its clients within about 0.3 s, once 0.5 s or more
	 * (bench, 2026-11-22: "1 -> 1" in the log, 0 clients 20 s later). This
	 * runs in a detached process, so it may wait for the count. */
	for (int k = 0; sent && k < 8; k++) {
		usleep(k ? 200000 : 300000);
		after = ap_clients();
		if (after == 0)
			break;
	}
	say("access point passphrase %s%s, clients %d -> %d",
	    !written ? "NOT written" : !pid ? "written, hostapd NOT found" : sent ? "rotated (hostapd re-read)" : "written, SIGHUP FAILED",
	    kept ? ": the kept one (developer access)" : keep ? ": a new one, kept from now on (developer access)" : "",
	    before, after);
}

/* The access point's name: the "ssid=" line of the file hostapd runs from
 * (the RAM copy of /etc), rewritten here as data: no shell, whatever the
 * name holds. hostapd takes the rest of the line as the name. It reads the
 * file again at the next SIGHUP (the passphrase change). */
void io_ap_name(const char *name)
{
	static const char conf[] = ROOT "/etc/hostapd/hostapd.conf", tmp[] = ROOT "/etc/hostapd/hostapd.conf.new";
	FILE *in = fopen(conf, "r"), *out = in ? fopen(tmp, "w") : NULL;
	char line[512];
	int done = 0;

	if (!in || !out || strlen(name) > 32 || strpbrk(name, "\r\n")) {
		if (in)
			fclose(in);
		if (out)
			fclose(out);
		say("access point name NOT written");
		return;
	}
	while (fgets(line, sizeof(line), in)) {
		if (!strncmp(line, "ssid=", 5)) {
			fprintf(out, "ssid=%s\n", name);
			done = 1;
		} else {
			fputs(line, out);
		}
	}
	fclose(in);
	if (fclose(out) || !done || rename(tmp, conf)) {
		unlink(tmp);
		say("access point name NOT written");
		return;
	}
	say("access point name written");
}

/* ---- sounds ---- */

/* One aplay of the stock root, kept running, plays what is written to its
 * standard input: 16-bit mono, into the unit's dmix, so a sound mixes with
 * CarPlay's audio and with stock's own. It reads whole chunks (one period of
 * the mixer, 1024 frames, or a few), so a sound is sent with silence up to
 * SOUND_BLOCK frames behind it; a sound that would not fit into the pipe now
 * is dropped, as a late sound is worse than none. */
#define SOUND_BLOCK 4096

static int sound_fd = -1;

static void sound_open(void)
{
	char rate[16];
	int p[2];
	pid_t pid;

	snprintf(rate, sizeof(rate), "%d", sound_rate);
	if (pipe(p))
		return;
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_WRONLY);

		dup2(p[0], 0);
		dup2(null, 1);
		dup2(null, 2);
		for (int fd = 3; fd < 64; fd++)
			close(fd);
		execl("/bin/busybox", "busybox", "chroot", ROOT, "/usr/bin/aplay", "-q", "-D", "plug:dmix", "-t", "raw",
		      "-f", "S16_LE", "-c", "1", "-r", rate, "-", (char *)NULL);
		_exit(127);
	}
	close(p[0]);
	if (pid < 0) {
		close(p[1]);
		return;
	}
	fcntl(p[1], F_SETFL, O_NONBLOCK);
	fcntl(p[1], F_SETFD, FD_CLOEXEC);
	sound_fd = p[1];
	say("sounds: player started");
}

void io_sound(int which)
{
	static int16_t block[SOUND_BLOCK * 4];
	const struct sound *s;
	int frames;

	if (which < 0 || which >= SOUNDS)
		return;
	s = &sounds[which];
	frames = (s->frames + SOUND_BLOCK - 1) / SOUND_BLOCK * SOUND_BLOCK;
	if (frames > SOUND_BLOCK * 4)
		return;
	if (sound_fd < 0)
		sound_open();
	if (sound_fd < 0)
		return;
	memcpy(block, s->pcm, s->frames * sizeof(block[0]));
	memset(block + s->frames, 0, (frames - s->frames) * sizeof(block[0]));
	/* All of it or none: a pipe takes a write of this size whole when it has
	 * the room, and refuses it when it has not. */
	if (write(sound_fd, block, frames * sizeof(block[0])) < 0 && errno == EPIPE) {
		close(sound_fd);	/* the player is gone: a new one at the next sound */
		sound_fd = -1;
		waitpid(-1, NULL, WNOHANG);
	}
}

/* A small file under the stock root (its RAM copies of /data and /etc),
 * written beside itself and renamed into place. */
void io_put(const char *path, const char *text)
{
	char file[160], tmp[168];
	FILE *f;

	snprintf(file, sizeof(file), ROOT "%s", path);
	snprintf(tmp, sizeof(tmp), "%s.new", file);
	f = fopen(tmp, "w");
	if (f) {
		int written = fprintf(f, "%s\n", text) > 0;

		if (!fclose(f) && written && !rename(tmp, file))
			return;
		unlink(tmp);
	}
	say("%s NOT written", path);
}

/* init-b mounted the state partition (a kernel with the state window). */
int io_state_kept(void)
{
	char mounts[4096];

	return read_file("/proc/mounts", mounts, sizeof(mounts)) > 0 && strstr(mounts, " /mnt/state ubifs ") != NULL;
}

/* A job that takes a third of a second or more (the spinner froze for it),
 * in a process of its own that nobody waits for; here, if there is no
 * process to be had. */
static void detached(void (*job)(void))
{
	pid_t pid = fork();

	if (pid == 0) {
		if (fork() == 0)
			job();
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
	else
		job();
}

/* The state archive (recovery/plan-a/state-save, on the userdata partition).
 * Without the state partition the script says so and does nothing. */
static void state_save_now(void)
{
	int rc = system("/opt/autotop/state-save > /tmp/state-save.log 2>&1");

	say("state: save %s (exit %d)", rc == 0 ? "done" : "NOT done", WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
}

/* ---- app updates ---- */

static void app_confirm_now(void)
{
	int rc = system("/opt/autotop/app-slot confirm > /tmp/app-confirm.log 2>&1");

	if (!WIFEXITED(rc) || WEXITSTATUS(rc))
		say("app: the trial could NOT be confirmed (exit %d)", WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
}

void io_app_confirm(void)
{
	detached(app_confirm_now);
}

/* ---- developer access (recovery/plan-a/dev-access, part of the system) ---- */

int io_dev_state(void)
{
	char text[128] = "";

	if (system("/opt/autotop/dev-access status > /tmp/dev-access.status 2> /dev/null"))
		return 0;	/* a system without it */
	read_file("/tmp/dev-access.status", text, sizeof(text));
	return (strstr(text, "available yes") ? 1 : 0) | (strstr(text, "on yes") ? 2 : 0);
}

static void dev_access_now(void)
{
	int rc = system(dev_wanted ? "/opt/autotop/dev-access on > /tmp/dev-access.log 2>&1"
				   : "/opt/autotop/dev-access off > /tmp/dev-access.log 2>&1");

	say("developer access: %s %s (exit %d)", dev_wanted ? "on" : "off", rc == 0 ? "done" : "NOT done",
	    WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
}

void io_dev_access(int on)
{
	dev_wanted = on;
	if (!on)
		unlink(AP_KEPT);	/* the passphrase that stood while it was on is not to come back */
	detached(dev_access_now);
}

#define UPDATE_STATUS "/tmp/update.status"
static pid_t update_pid;

/* The receiver in a process of its own: it reads from the network, and it
 * lives only while the Software update page is open. */
int io_update_start(void)
{
	static const struct update_cfg cfg = { 80, "/tmp/update.bundle", "/opt/autotop/app-slot install",
					       UPDATE_STATUS, 16L << 20, 53, "192.168.43.1",
					       "/opt/autotop/bin/logs-pack" };

	io_update_stop();
	unlink(UPDATE_STATUS);
	update_pid = fork();
	if (update_pid == 0) {
		for (int fd = 3; fd < 256; fd++)	/* nothing of the launcher's in it */
			close(fd);
		_exit(update_serve(&cfg) ? 1 : 0);
	}
	if (update_pid < 0) {
		update_pid = 0;
		return -1;
	}
	return 0;
}

void io_update_stop(void)
{
	if (update_pid > 0) {
		kill(update_pid, SIGTERM);
		waitpid(update_pid, NULL, 0);
		update_pid = 0;
	}
	unlink("/tmp/update.bundle");
}

int io_update_status(char *line, int n)
{
	int got = read_file(UPDATE_STATUS, line, n);

	if (got <= 0)
		return 0;
	line[strcspn(line, "\r\n")] = 0;
	return 1;
}

int io_ap_clients(void)
{
	return ap_clients();
}

/* The name and the passphrase hostapd runs with (the RAM copy of /etc). */
void io_ap_info(char *ssid, int ssid_n, char *psk, int psk_n)
{
	char buf[4096], *p;
	int n = read_file(ROOT "/etc/hostapd/hostapd.conf", buf, sizeof(buf));

	ssid[0] = psk[0] = 0;
	if (n <= 0)
		return;
	for (p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
		int len = (int)strcspn(p, "\r\n");

		if (!strncmp(p, "ssid=", 5))
			snprintf(ssid, ssid_n, "%.*s", len - 5, p + 5);
		else if (!strncmp(p, "wpa_passphrase=", 15))
			snprintf(psk, psk_n, "%.*s", len - 15, p + 15);
	}
	memset(buf, 0, sizeof(buf));
}

void io_state_save(void)
{
	detached(state_save_now);
}

void io_rotate_ap(void)
{
	if (AP_ROTATE)
		detached(rotate_ap_now);
}

/* ---- touch: the first finger of the touchscreen ---- */

static void touch_drain(void)
{
	static int slot, id = -1, x, y, down;
	struct input_event e;

	while (read(touch_fd, &e, sizeof(e)) == sizeof(e)) {
		if (e.type == EV_ABS) {
			if (e.code == ABS_MT_SLOT)
				slot = e.value;
			else if (slot)
				continue;
			else if (e.code == ABS_MT_TRACKING_ID)
				id = e.value;
			else if (e.code == ABS_MT_POSITION_X)
				x = e.value;
			else if (e.code == ABS_MT_POSITION_Y)
				y = e.value;
		} else if (e.type == EV_SYN && e.code == SYN_REPORT) {
			int type = id >= 0 ? (down ? 2 : 1) : (down ? 3 : 0);

			down = id >= 0;
			if (type)
				core_touch(type, x, y);
		}
	}
}

/* ---- the GUI layer: /dev/fb0, 1280 x 480 ARGB8888, over CarPlay's video ---- */

/* The frame interrupt (the 4.19 lcdfb gives up after 100 ms). */
static void vsync(void)
{
	__u32 crtc = 0;

	ioctl(fb_fd, FBIO_WAITFORVSYNC, &crtc);
}

static uint32_t ticks(void)
{
	return (uint32_t)io_now_ms();
}

/* ---- a screen pushed out by the next, by the display controller ----
 *
 * /dev/fb0 and /dev/fb1 are the controller's first two GUI layers (OSD1 over
 * OSD2) on one memory of three screens. A layer has an address and a signed
 * position, and both can be set for the same frame; nothing is copied. The
 * lower layer shows the screen that leaves, shifted by its address (what
 * wraps into view at the far edge lies under the upper layer); the upper one
 * shows the screen that arrives and moves by its position. Proven with
 * recovery/plan-a/probe-push.c (docs/plan-a-gpu-display-spike.md). */
#define SCREEN_BYTES (1280 * 480 * 4)
#define PUSH_FRAMES 12	/* dur-screen, 200 ms, at the panel's 60 Hz */

struct ark_disp_addr { unsigned int yaddr, cbaddr, craddr, wait_vsync; };
struct ark_disp_reg { unsigned int addr, value; };
struct ark_fb_blend { int blending, by_pixel, mode; unsigned char alpha; };
struct ark_disp_atomic {
	int layer, what, pos_x, pos_y, width, height, format, yuyv_order, rgb_order;
	struct ark_disp_addr addr;
	int scaler[14];
};
#define ATOMIC_POS 1
#define ATOMIC_SIZE 2
#define ATOMIC_ADDR 8
#define ARKFB_SHOW_WINDOW _IO('O', 39)
#define ARKFB_HIDE_WINDOW _IO('O', 40)
#define ARKFB_SET_WINDOW_ADDR _IOW('O', 44, struct ark_disp_addr)
#define ARKFB_SET_WINDOW_ATOMIC _IOW('O', 46, struct ark_disp_atomic)
#define ARKFB_SET_REG_VALUE _IOW('O', 47, struct ark_disp_reg)
#define ARKFB_GET_REG_VALUE _IOR('O', 48, struct ark_disp_reg)
#define ARKFB_SET_BLEND _IOW('O', 41, struct ark_fb_blend)
#define LCDC_MODE0 0xe0500060u	/* layer order and blend modes */
#define LCDC_MODE1 0xe0500064u	/* per layer: blending, per-pixel alpha */
#define LCDC_OSD1_CTL 0xe0500074u
#define LCDC_OSD2_CTL 0xe0500088u

static int fb1_fd = -1;
static unsigned long fb_base;
static int fb_blend_mode;	/* the launcher's layer at rest: blends by its pixels, in this mode */

static int lcdc(unsigned int at, unsigned int *value, int set)
{
	struct ark_disp_reg r = { at, *value };

	if (ioctl(fb_fd, set ? ARKFB_SET_REG_VALUE : ARKFB_GET_REG_VALUE, &r))
		return -1;
	*value = r.value;
	return 0;
}

/* A layer's address, taken at the next frame interrupt; does not wait. */
static void layer_addr(int fd, unsigned long addr)
{
	struct ark_disp_addr a = { addr, 0, 0, 0 };

	ioctl(fd, ARKFB_SET_WINDOW_ADDR, &a);
}

/* Waits for the frame interrupt, then sets the layer. */
static void layer_set(int fd, int layer, int what, int x, unsigned long addr)
{
	struct ark_disp_atomic a = { .layer = layer, .what = what, .pos_x = x, .width = 1280, .height = 480,
				     .addr = { addr, 0, 0, 0 } };

	ioctl(fd, ARKFB_SET_WINDOW_ATOMIC, &a);
}

static void screen_move(enum ui_change how, int from, int to)
{
	const unsigned long a_from = fb_base + (unsigned long)from * SCREEN_BYTES, a_to = fb_base + (unsigned long)to * SCREEN_BYTES;
	const int dir = how == UI_CHANGE_BACK ? -1 : 1, n = PUSH_FRAMES;
	struct fb_var_screeninfo var;

	/* The lower layer comes on under the upper, which hides it until it
	 * moves; from the first frame it shows the screen that leaves. */
	ioctl(fb1_fd, ARKFB_SHOW_WINDOW);
	for (int f = 1; f <= n; f++) {
		int left = n - f, dx = (1280 - 1280 * left * left * left / (n * n * n)) & ~3;	/* ease-out */

		layer_addr(fb1_fd, a_from + dir * dx * 4);
		layer_set(fb_fd, 0, f == 1 ? ATOMIC_POS | ATOMIC_ADDR : ATOMIC_POS, dir * (1280 - dx), a_to);
	}
	ioctl(fb1_fd, ARKFB_HIDE_WINDOW);
	if (!ioctl(fb_fd, FBIOGET_VSCREENINFO, &var)) {	/* the driver's own idea of where fb0 rests */
		var.yoffset = to * 480;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
	}
}

/* The launcher's layer fades away over what is under it: for PUSH_FRAMES
 * frames it blends by one alpha for the whole layer, falling to nothing.
 * out = 0: by its pixels again (the caller has drawn it clear meanwhile). */
static void screen_fade(int out)
{
	struct ark_fb_blend b = { 1, !out, fb_blend_mode, 0xff };

	for (int f = 1; out && f <= PUSH_FRAMES; f++) {
		b.alpha = 255 - 255 * f / PUSH_FRAMES;
		vsync();
		ioctl(fb_fd, ARKFB_SET_BLEND, &b);
	}
	if (!out)
		ioctl(fb_fd, ARKFB_SET_BLEND, &b);
}

/* The second layer as the first is (the format, and how it blends), once. 0 = done. */
static int layers_start(void)
{
	struct fb_fix_screeninfo fix;
	unsigned int ctl = 0, mode0 = 0, mode1 = 0;

	if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) || fix.smem_len < 2 * SCREEN_BYTES)
		return -1;
	fb_base = fix.smem_start;
	fb1_fd = open("/dev/fb1", O_RDWR);
	if (fb1_fd < 0)
		return -1;
	if (lcdc(LCDC_OSD1_CTL, &ctl, 0) || lcdc(LCDC_MODE0, &mode0, 0) || lcdc(LCDC_MODE1, &mode1, 0)) {
		close(fb1_fd);
		fb1_fd = -1;
		return -1;
	}
	fb_blend_mode = mode0 >> 12 & 0xf;
	mode0 = (mode0 & ~(0xfu << 20)) | ((mode0 >> 12 & 0xf) << 20);
	mode1 = (mode1 & ~(3u << 14)) | ((mode1 >> 12 & 3) << 14);
	lcdc(LCDC_OSD2_CTL, &ctl, 1);
	lcdc(LCDC_MODE0, &mode0, 1);
	lcdc(LCDC_MODE1, &mode1, 1);
	/* In place and whole, on our memory, and off until a screen moves (the
	 * boot leaves it on, under ours, where the logo was). */
	layer_set(fb1_fd, 1, ATOMIC_POS | ATOMIC_SIZE | ATOMIC_ADDR, 0, fb_base);
	ioctl(fb1_fd, ARKFB_HIDE_WINDOW);
	return 0;
}

static int ui_start(void)
{
	struct fb_var_screeninfo var;
	uint8_t *pixels;
	int two = 1;

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0)
		return 0;
	if (!ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) && var.yoffset) {	/* a launcher before this one left it on screen 1 */
		var.yoffset = 0;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
	}
	pixels = mmap(NULL, 2 * SCREEN_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (pixels == MAP_FAILED) {
		two = 0;
		pixels = mmap(NULL, SCREEN_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	}
	if (pixels == MAP_FAILED) {
		close(fb_fd);
		fb_fd = -1;
		return 0;
	}
	if (ui_display_init(pixels, ticks, vsync))
		return 0;
	if (two && !layers_start()) {
		ui_display_moves(screen_move);
		ui_display_fades(screen_fade);
	}
	return 1;
}

void io_reboot(void)
{
	sync();
	reboot(RB_AUTOBOOT);
}

/* ---- the bench's stand-in for the iDrive controller ---- */

/* There is no iDrive at the bench. With LAUNCHER_CTL=1 in PLANA.CFG (the
 * image in NAND does not set it) the letters l, r, p, b, m written to
 * /tmp/launcherd.ctl act as its key frames would. */
static void ctl_open(void)
{
	const char *on = getenv("LAUNCHER_CTL");

	if (!on || on[0] != '1')
		return;
	mkfifo(CTL, 0600);
	ctl_fd = open(CTL, O_RDWR | O_NONBLOCK);	/* read-write: no end-of-file when a writer leaves */
	say("control file %s %s", CTL, ctl_fd >= 0 ? "open: keys from the bench" : "could not be opened");
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	core_begin();
	while (bus_connect()) {
		say("waiting for the session bus");
		sleep(1);
	}
	mcu_open();
	touch_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	ctl_open();
	core_start(ui_start());

	for (;;) {
		struct pollfd p[5] = {
			{ .fd = bus.fd, .events = POLLIN }, { .fd = goc, .events = POLLIN },
			{ .fd = mcu, .events = POLLIN }, { .fd = touch_fd, .events = POLLIN },
			{ .fd = ctl_fd, .events = POLLIN },
		};
		unsigned char b[256];
		ssize_t r;

		poll(p, 5, core_poll_ms());
		core_flush();
		if (p[0].revents)
			bus_drain();
		if (goc >= 0 && p[1].revents & POLLIN)
			while ((r = read(goc, b, sizeof(b))) > 0)
				core_bt_bytes((const char *)b, r);
		if (mcu >= 0 && p[2].revents & POLLIN)
			while ((r = read(mcu, b, sizeof(b))) > 0)
				core_mcu_bytes(b, r);
		if (touch_fd >= 0 && p[3].revents & POLLIN)
			touch_drain();
		if (ctl_fd >= 0 && p[4].revents & POLLIN)
			while ((r = read(ctl_fd, b, sizeof(b))) > 0)
				for (ssize_t k = 0; k < r; k++)
					core_key((char)b[k]);
		core_tick();
	}
}
