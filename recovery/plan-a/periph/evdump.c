/* SPDX-License-Identifier: MIT
 * Print touch events from the first input device whose name contains NAME
 * for SECONDS, then a summary per tap (BTN_TOUCH down..up): first, last,
 * min and max ABS_MT_POSITION_X/Y. Read-only.
 *   evdump [SECONDS] [NAME]        default 30, "Goodix"
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 30, fd = -1, taps = 0, down = 0;
	const char *want = argc > 2 ? argv[2] : "Goodix";
	char path[64], name[128];
	int x = -1, y = -1, fx = 0, fy = 0, minx = 0, maxx = 0, miny = 0, maxy = 0;
	struct input_absinfo ax, ay;
	time_t end;

	for (int i = 0; i < 16 && fd < 0; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY);
		if (fd >= 0 && (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 || !strstr(name, want))) {
			close(fd);
			fd = -1;
		}
	}
	if (fd < 0)
		return printf("RESULT FAIL no input device named *%s*\n", want), 1;
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
	printf("device %s \"%s\" X %d..%d Y %d..%d; touch now for %d s\n", path, name,
	       ax.minimum, ax.maximum, ay.minimum, ay.maximum, secs);
	end = time(NULL) + secs;
	while (time(NULL) < end) {
		struct pollfd p = { fd, POLLIN, 0 };
		struct input_event ev;

		if (poll(&p, 1, 200) <= 0 || read(fd, &ev, sizeof(ev)) != sizeof(ev))
			continue;
		if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X)
			x = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y)
			y = ev.value;
		else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value) {
			down = 1;
			fx = fy = -1;
		} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && !ev.value && down) {
			down = 0;
			printf("TAP %d first %d,%d last %d,%d range x %d..%d y %d..%d\n",
			       ++taps, fx, fy, x, y, minx, maxx, miny, maxy);
		} else if (ev.type == EV_SYN && down && x >= 0 && y >= 0) {
			if (fx < 0) {
				fx = minx = maxx = x;
				fy = miny = maxy = y;
			}
			minx = x < minx ? x : minx;
			maxx = x > maxx ? x : maxx;
			miny = y < miny ? y : miny;
			maxy = y > maxy ? y : maxy;
		}
	}
	printf("RESULT %s %d taps\n", taps ? "PASS" : "FAIL", taps);
	return taps ? 0 : 1;
}
