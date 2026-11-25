/* SPDX-License-Identifier: MIT
 * Read-only: print the screen info /dev/ark_display returns (ioctl
 * 0xc004a01d, 5 x u32), to set ark_display_compat's parameters from stock.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
	uint32_t v[5] = { 0 };
	int fd = open("/dev/ark_display", O_RDONLY);

	if (fd < 0)
		return perror("/dev/ark_display"), 1;
	if (ioctl(fd, 0xc004a01d, v))
		return perror("ioctl 0xc004a01d"), 1;
	printf("ARKDISP screen_id=%u width=%u height=%u w2=%u h2=%u\n", v[0], v[1], v[2], v[3], v[4]);
	close(fd);
	return 0;
}
