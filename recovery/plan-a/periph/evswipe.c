/* SPDX-License-Identifier: MIT
 * Inject identical drags into the Goodix touchscreen's event device, for
 * repeatable CarPlay load (scrolling) during measurements.
 *
 *   evswipe X0 Y0 X1 Y1 MS COUNT [GAP_MS]
 *
 * Each stroke: touch down at (X0,Y0), move at constant speed to (X1,Y1) in
 * MS milliseconds with a report every 10 ms (the GT911's own rate), hold
 * still for 80 ms (no fling), lift. Strokes alternate direction, so a list
 * scrolls back and forth. GAP_MS (default 150) between strokes.
 *
 * Events are written to /dev/input/eventN (evdev write = input_inject_event),
 * so every reader sees them exactly like the driver's reports: the same
 * sequence the GT911 produces (MT protocol B, slot 0: tracking ID, position,
 * touch/width major, BTN_TOUCH, ABS_X/Y, SYN_REPORT; small tracking IDs).
 * A first version with IDs from 1000 left Plan A's CarPlay touch dead until
 * a power cycle (2026-11-19): every stroke after the first touched down at
 * the previous position, which the input core drops, so Launcher sent X=0. Injected events are
 * ignored while another client holds EVIOCGRAB; this is checked first.
 * Devices without ABS_MT_SLOT (stock 3.4) get protocol A frames instead.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int fd;
/* Protocol A (stock 3.4 GT911 driver, no slots) or B (4.19 goodix). */
static int proto_a;
/* Last position the kernel stored for slot 0 (evdev drops repeated values). */
static int last_x = -1, last_y = -1;

static void ev(int type, int code, int value)
{
	struct input_event e;

	memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.value = value;
	if (write(fd, &e, sizeof(e)) != sizeof(e)) {
		perror("write");
		exit(1);
	}
}

static void sleep_ms(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&t, NULL);
}

static void pos(int x, int y)
{
	last_x = x;
	last_y = y;
	ev(EV_ABS, ABS_MT_POSITION_X, x);
	ev(EV_ABS, ABS_MT_POSITION_Y, y);
	ev(EV_ABS, ABS_X, x);
	ev(EV_ABS, ABS_Y, y);
}

/* EVSWIPE_TS=1: print the wall-clock time of each touch-down report, to
 * measure latency against dbus-monitor --profile (same clock). */
static void stamp(const char *what)
{
	struct timeval tv;

	if (!getenv("EVSWIPE_TS"))
		return;
	gettimeofday(&tv, NULL);
	printf("%s %ld.%06ld\n", what, (long)tv.tv_sec, (long)tv.tv_usec);
	fflush(stdout);
}

/* Stock 3.4 format, as captured: every ~10.7 ms while touching
 * TRACKING_ID 0, TOUCH_MAJOR, X, Y, SYN_MT_REPORT, SYN_REPORT (also when
 * still); the lift is TRACKING_ID 0, TOUCH_MAJOR 0, SYN_MT_REPORT, SYN. */
static void frame_a(int x, int y, int major)
{
	ev(EV_ABS, ABS_MT_TRACKING_ID, 0);
	ev(EV_ABS, ABS_MT_TOUCH_MAJOR, major);
	if (major) {
		ev(EV_ABS, ABS_MT_POSITION_X, x);
		ev(EV_ABS, ABS_MT_POSITION_Y, y);
	}
	ev(EV_SYN, SYN_MT_REPORT, 0);
	ev(EV_SYN, SYN_REPORT, 0);
}

static void stroke_a(int x0, int y0, int x1, int y1, int ms)
{
	int steps = ms / 10 > 0 ? ms / 10 : 1;

	frame_a(x0, y0, 23);
	stamp("D");
	for (int i = 1; i <= steps; i++) {
		sleep_ms(10);
		frame_a(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, 23);
	}
	for (int i = 0; i < 8; i++) {	/* hold still 80 ms, no fling */
		sleep_ms(10);
		frame_a(x1, y1, 23);
	}
	frame_a(0, 0, 0);
}

static void stroke(int id, int x0, int y0, int x1, int y1, int ms)
{
	if (proto_a) {
		stroke_a(x0, y0, x1, y1, ms);
		return;
	}
	int steps = ms / 10 > 0 ? ms / 10 : 1;

	/* The input core drops an ABS value equal to the one it holds, so a
	 * touch-down at the previous stroke's position would reach readers
	 * without X/Y (Launcher then reports 0, the left edge: CarPlay's dock).
	 * Real fingers never repeat a pixel; nudge by one. */
	if (x0 == last_x)
		x0 += x0 < 1279 ? 1 : -1;
	if (y0 == last_y)
		y0 += y0 < 479 ? 1 : -1;
	/* Same order as the GT911 driver's first report of a contact. */
	ev(EV_ABS, ABS_MT_TRACKING_ID, id);
	ev(EV_ABS, ABS_MT_POSITION_X, x0);
	ev(EV_ABS, ABS_MT_POSITION_Y, y0);
	ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 40);
	ev(EV_ABS, ABS_MT_WIDTH_MAJOR, 40);
	ev(EV_KEY, BTN_TOUCH, 1);
	ev(EV_ABS, ABS_X, x0);
	ev(EV_ABS, ABS_Y, y0);
	ev(EV_SYN, SYN_REPORT, 0);
	stamp("D");
	for (int i = 1; i <= steps; i++) {
		sleep_ms(10);
		pos(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
		ev(EV_SYN, SYN_REPORT, 0);
	}
	sleep_ms(80);
	ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
	ev(EV_KEY, BTN_TOUCH, 0);
	ev(EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv)
{
	char path[32], name[128] = "";
	int v[7], gap = 150;

	if (argc < 7) {
		fprintf(stderr, "usage: evswipe X0 Y0 X1 Y1 MS COUNT [GAP_MS]\n");
		return 2;
	}
	for (int i = 0; i < 6; i++)
		v[i] = atoi(argv[i + 1]);
	if (argc > 7)
		gap = atoi(argv[7]);
	if (v[0] < 0 || v[0] > 1279 || v[2] < 0 || v[2] > 1279 || v[1] < 0 || v[1] > 479 ||
	    v[3] < 0 || v[3] > 479 || v[4] < 50 || v[4] > 5000 || v[5] < 1 || v[5] > 1000 ||
	    gap < 0 || gap > 5000) {
		fprintf(stderr, "out of range (x 0-1279, y 0-479, ms 50-5000, count 1-1000)\n");
		return 2;
	}
	fd = -1;
	for (int i = 0; i < 16 && fd < 0; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDWR);
		if (fd >= 0 && (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 || !strstr(name, "Goodix"))) {
			close(fd);
			fd = -1;
		}
	}
	if (fd < 0) {
		fprintf(stderr, "no Goodix input device\n");
		return 1;
	}
	if (ioctl(fd, EVIOCGRAB, 1) < 0) {
		fprintf(stderr, "%s is grabbed by another client (%s): injected events would be ignored\n",
			path, strerror(errno));
		return 1;
	}
	ioctl(fd, EVIOCGRAB, 0);
	{
		unsigned long abs[(ABS_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = { 0 };

		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
		proto_a = !(abs[ABS_MT_SLOT / (8 * sizeof(long))] >> (ABS_MT_SLOT % (8 * sizeof(long))) & 1);
		printf("multitouch protocol %c\n", proto_a ? 'A' : 'B');
	}
	{
		/* Slot 0's stored position, from real touches before us. */
		int32_t buf[1 + 16];

		int rx, ry;

		buf[0] = ABS_MT_POSITION_X;
		rx = ioctl(fd, EVIOCGMTSLOTS(sizeof(buf)), buf);
		if (rx >= 0)
			last_x = buf[1];
		buf[0] = ABS_MT_POSITION_Y;
		ry = ioctl(fd, EVIOCGMTSLOTS(sizeof(buf)), buf);
		if (ry >= 0)
			last_y = buf[1];
		printf("slot 0 stored position: %d,%d (ioctl %d/%d%s%s)\n", last_x, last_y, rx, ry,
		       rx < 0 || ry < 0 ? ": " : "", rx < 0 || ry < 0 ? strerror(errno) : "");
	}
	printf("%s (%s): %d strokes (%d,%d)<->(%d,%d) in %d ms, gap %d ms\n",
	       path, name, v[5], v[0], v[1], v[2], v[3], v[4], gap);
	fflush(stdout);
	for (int n = 0; n < v[5]; n++) {
		/* Small consecutive IDs like the kernel's input_mt_new_trkid(). */
		int id = 1 + n % 200;

		if (n & 1)
			stroke(id, v[2], v[3], v[0], v[1], v[4]);
		else
			stroke(id, v[0], v[1], v[2], v[3], v[4]);
		sleep_ms(gap);
	}
	printf("done\n");
	return 0;
}
