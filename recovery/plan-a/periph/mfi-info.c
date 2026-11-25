/* SPDX-License-Identifier: MIT
 * Read-only identity check of the Apple MFi coprocessor on /dev/i2c-0.
 *   mfi-info [BUS] [ADDR7]        default /dev/i2c-0, 0x11 (carplay's 0x22)
 * Reads registers 0x00-0x07 (device version, firmware version, protocol
 * major/minor, device ID). Each read writes only the one-byte register
 * address and then reads one byte, retrying while the chip NACKs (it does
 * while busy or asleep). No register is ever written with data.
 */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int rd(int fd, unsigned char reg, unsigned char *val)
{
	for (int t = 0; t < 200; t++) {
		if (write(fd, &reg, 1) == 1) {
			usleep(500);
			for (int u = 0; u < 20; u++) {
				if (read(fd, val, 1) == 1)
					return t;
				usleep(500);
			}
		}
		usleep(1000);
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *bus = argc > 1 ? argv[1] : "/dev/i2c-0";
	long addr = argc > 2 ? strtol(argv[2], NULL, 0) : 0x11;
	static const char *names[8] = { "device version", "firmware version", "protocol major",
					"protocol minor", "device id[0]", "device id[1]", "device id[2]",
					"device id[3]" };
	int fd = open(bus, O_RDWR), ok = 0;

	if (fd < 0 || ioctl(fd, I2C_SLAVE, addr) < 0)
		return perror(bus), 1;
	for (unsigned char r = 0; r < 8; r++) {
		unsigned char v;
		int tries = rd(fd, r, &v);

		if (tries < 0)
			printf("MFI reg 0x%02x %-16s no answer\n", r, names[r]);
		else
			printf("MFI reg 0x%02x %-16s 0x%02x (after %d retries)\n", r, names[r], v, tries), ok++;
	}
	printf("RESULT %s %d/8 registers from 0x%02lx on %s\n", ok ? "PASS" : "FAIL", ok, addr, bus);
	return ok ? 0 : 1;
}
