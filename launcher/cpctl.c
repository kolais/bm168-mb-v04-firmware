/* SPDX-License-Identifier: MIT
 * CarPlay control prototype for the new Launcher (docs/launcher-design.md).
 *
 *   cpctl start ADDR      wireless CarPlay with the phone's Bluetooth address
 *                         (requestSetPhoneBTAddr(7, ADDR, 1), requestLinkStatus(7, 3))
 *   cpctl status N        requestLinkStatus(7, N) (6 background, 7 exit, ...)
 *   cpctl touch [-c]      forward the touchscreen to carplay (requestTouchStatus
 *                         as stock Launcher sends it); -c first clears the GUI
 *                         framebuffer to transparent so the video layer shows
 *   cpctl watch SECS      print onLinkStatusChange signals
 *   cpctl monitor SECS    print arrival time and touch type of every
 *                         requestTouchStatus on the bus (latency tests)
 *
 * Bus address: $DBUS_SESSION_BUS_ADDRESS, else /mnt/stock/tmp/dbus-address.
 * Touch format (captured from stock Launcher, 2026-11-19): deviceType 0,
 * touchType 1 down / 2 move / 3 up, XML with Qt touch-point state 1/2/8.
 */
#include "dbusmini.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEST "com.arkmicro.carplay"
#define PATH "/com/arkmicro/carplay"
#define IFACE "Local.DbusServer.Carplay"

static int bus(struct dbm *c)
{
	const char *a = getenv("DBUS_SESSION_BUS_ADDRESS");
	char buf[256];

	if (!a || !*a) {
		FILE *f = fopen("/mnt/stock/tmp/dbus-address", "r");
		if (!f || !fgets(buf, sizeof(buf), f))
			return fprintf(stderr, "no bus address\n"), -1;
		fclose(f);
		buf[strcspn(buf, "\r\n")] = 0;
		a = buf;
	}
	if (dbm_connect(c, a))
		return fprintf(stderr, "cannot connect to %s\n", a), -1;
	return 0;
}

static int link_status(struct dbm *c, int status)
{
	struct dbm_arg a[2] = { { 'i', 7, 0 }, { 'i', status, 0 } };

	return dbm_call(c, DEST, PATH, IFACE, "requestLinkStatus", a, 2);
}

static int clear_fb(void)
{
	static unsigned char zero[1280 * 4];
	int fd = open("/dev/fb0", O_WRONLY);

	if (fd < 0)
		return -1;
	for (int y = 0; y < 480; y++)
		if (write(fd, zero, sizeof(zero)) != sizeof(zero))
			break;
	close(fd);
	return 0;
}

static int send_touch(struct dbm *c, int type, int x, int y)
{
	static const int state[4] = { 0, 1, 2, 8 };
	char xml[256];

	snprintf(xml, sizeof(xml),
		 "<TouchEvent>\n <TouchPoint>\n  <Index:0>\n   <State>%d</State>\n"
		 "   <X>%d</X>\n   <Y>%d</Y>\n  </Index:0>\n </TouchPoint>\n</TouchEvent>\n",
		 state[type], x, y);
	struct dbm_arg a[3] = { { 'i', 0, 0 }, { 'i', type, 0 }, { 's', 0, xml } };
	return dbm_call(c, DEST, PATH, IFACE, "requestTouchStatus", a, 3);
}

/* Forward touches. The bus connection is kept healthy: anything the bus
 * sends us is read and dropped (an unread connection can stall), and a
 * failed call reconnects and resends. A first version without this stopped
 * forwarding after some minutes (2026-11-19). */
static int touch(struct dbm *c)
{
	int fd = open("/dev/input/event0", O_RDONLY);
	struct input_event e;
	int slot = 0, id = -1, x = 0, y = 0, down = 0, sent = 0, reconnects = 0;

	if (fd < 0)
		return perror("event0"), 1;
	for (;;) {
		struct pollfd p[2] = { { .fd = fd, .events = POLLIN }, { .fd = c->fd, .events = POLLIN } };

		if (poll(p, 2, -1) < 0)
			continue;
		if (p[1].revents) {
			int type, n;
			int32_t v[8];

			if (dbm_read(c, 0, &type, NULL, 0, v, &n)) {
				dbm_close(c);
				while (bus(c))
					sleep(1);
				reconnects++;
				fprintf(stderr, "bus reconnected (%d)\n", reconnects);
			}
		}
		if (!(p[0].revents & POLLIN))
			continue;
		if (read(fd, &e, sizeof(e)) != sizeof(e))
			break;
		if (e.type == EV_ABS) {
			if (e.code == ABS_MT_SLOT)
				slot = e.value;
			else if (slot != 0)
				continue;	/* first finger only, for now */
			else if (e.code == ABS_MT_TRACKING_ID)
				id = e.value;
			else if (e.code == ABS_MT_POSITION_X)
				x = e.value;
			else if (e.code == ABS_MT_POSITION_Y)
				y = e.value;
		} else if (e.type == EV_SYN && e.code == SYN_REPORT) {
			int type = 0;

			if (id >= 0 && !down)
				type = 1, down = 1;
			else if (id >= 0 && down)
				type = 2;
			else if (id < 0 && down)
				type = 3, down = 0;
			if (!type)
				continue;
			if (send_touch(c, type, x, y)) {
				dbm_close(c);
				while (bus(c))
					sleep(1);
				reconnects++;
				fprintf(stderr, "bus reconnected after a failed call (%d)\n", reconnects);
				send_touch(c, type, x, y);
			}
			sent++;
		}
	}
	fprintf(stderr, "event0 closed after %d touch messages\n", sent);
	return 1;
}

int main(int argc, char **argv)
{
	struct dbm c;

	signal(SIGPIPE, SIG_IGN);
	if (argc < 2)
		goto usage;
	if (bus(&c))
		return 1;
	if (!strcmp(argv[1], "start") && argc == 3) {
		struct dbm_arg a[3] = { { 'i', 7, 0 }, { 's', 0, argv[2] }, { 'i', 1, 0 } };
		if (dbm_call(&c, DEST, PATH, IFACE, "requestSetPhoneBTAddr", a, 3) || link_status(&c, 3))
			return fprintf(stderr, "call failed\n"), 1;
		return 0;
	}
	if (!strcmp(argv[1], "status") && argc == 3)
		return link_status(&c, atoi(argv[2])) ? 1 : 0;
	if (!strcmp(argv[1], "touch")) {
		if (argc > 2 && !strcmp(argv[2], "-c"))
			clear_fb();
		return touch(&c);
	}
	if (!strcmp(argv[1], "watch") && argc == 3) {
		time_t end = time(NULL) + atoi(argv[2]);
		dbm_add_match(&c, "type='signal',interface='" IFACE "'");
		while (time(NULL) < end) {
			int type, n;
			int32_t v[8];
			char member[64];
			if (dbm_read(&c, 1000, &type, member, sizeof(member), v, &n))
				continue;
			if (type == 4 && n >= 2)
				printf("%ld %s %d %d\n", (long)time(NULL), member, v[0], v[1]), fflush(stdout);
		}
		return 0;
	}
	if (!strcmp(argv[1], "monitor") && argc == 3) {
		time_t end = time(NULL) + atoi(argv[2]);
		dbm_add_match(&c, "type='method_call',member='requestTouchStatus'");
		while (time(NULL) < end) {
			int type, n;
			int32_t v[8];
			char member[64];
			struct timeval tv;
			if (dbm_read(&c, 1000, &type, member, sizeof(member), v, &n))
				continue;
			gettimeofday(&tv, NULL);
			if (type == 1 && !strcmp(member, "requestTouchStatus") && n >= 2)
				printf("T %ld.%06ld %d\n", (long)tv.tv_sec, (long)tv.tv_usec, v[1]);
		}
		fflush(stdout);
		return 0;
	}
usage:
	fprintf(stderr, "usage: cpctl start ADDR | status N | touch [-c] | watch SECS | monitor SECS\n");
	return 2;
}
