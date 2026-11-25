/* SPDX-License-Identifier: MIT
 * Poll a Goodix GT911 over /dev/i2c-0 without its interrupt, the way the
 * touch driver reads it: status register 0x814E (bit 7 = buffer ready,
 * bits 0-3 = points), point 1 at 0x8150 (x, y little-endian). After a
 * ready status the status register is reset to 0, as the driver does; no
 * other register is written. Prints each touch's first and last position.
 *   gt911-poll [SECONDS] [ADDR7]      default 30, 0x5d
 */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int rd(int fd, unsigned reg, unsigned char *buf, int n)
{
	unsigned char a[2] = { reg >> 8, reg & 0xff };

	return write(fd, a, 2) == 2 && read(fd, buf, n) == n ? 0 : -1;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 30;
	long addr = argc > 2 ? strtol(argv[2], NULL, 0) : 0x5d;
	int fd = open("/dev/i2c-0", O_RDWR), down = 0, taps = 0, fx = 0, fy = 0, lx = 0, ly = 0;
	unsigned char id[4] = { 0 }, cfg[4] = { 0 }, st, pt[4];
	time_t end;

	if (fd < 0 || ioctl(fd, I2C_SLAVE, addr) < 0)
		return perror("/dev/i2c-0"), 1;
	if (rd(fd, 0x8140, id, 4) || rd(fd, 0x8048, cfg, 4))
		return printf("RESULT FAIL no answer from 0x%02lx\n", addr), 1;
	printf("GT911 id %.4s, resolution %ux%u; touch now for %d s\n", id,
	       cfg[0] | cfg[1] << 8, cfg[2] | cfg[3] << 8, secs);
	end = time(NULL) + secs;
	while (time(NULL) < end) {
		usleep(10000);
		if (rd(fd, 0x814e, &st, 1) || !(st & 0x80))
			continue;
		if ((st & 0x0f) && !rd(fd, 0x8150, pt, 4)) {
			lx = pt[0] | pt[1] << 8;
			ly = pt[2] | pt[3] << 8;
			if (!down) {
				down = 1;
				fx = lx;
				fy = ly;
			}
		} else if (!(st & 0x0f) && down) {
			down = 0;
			printf("TAP %d first %d,%d last %d,%d\n", ++taps, fx, fy, lx, ly);
		}
		{
			unsigned char clr[3] = { 0x81, 0x4e, 0 };

			if (write(fd, clr, 3) != 3)
				printf("status reset failed\n");
		}
	}
	printf("RESULT %s %d taps\n", taps ? "PASS" : "FAIL", taps);
	return taps ? 0 : 1;
}
