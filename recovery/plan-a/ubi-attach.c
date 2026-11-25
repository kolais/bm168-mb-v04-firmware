/* SPDX-License-Identifier: MIT
 * Attach an MTD partition to UBI with a chosen bad-block reserve:
 *
 *   ubi-attach MTD_NUM UBI_NUM MAX_BAD_PER_1024
 *
 * For the one moment recovery/plan-a/state-mount makes the state partition
 * anew. The unit's busybox (1.25) cannot say how many blocks UBI is to hold
 * back for blocks that go bad, and the kernel's own figure is 20 in every
 * 1,024 of the whole chip: 20 of this partition's 48. A volume made then is
 * half the size of stock's (which holds back 2), too small for an app
 * update (seen on the unit, 2026-11-24: 1,032 KB where stock's has 2,980).
 * The figure only counts while the volume is made; later attaches by
 * busybox find the volume as large as it is.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct attach_req {	/* the kernel's struct ubi_attach_req (include/uapi/mtd/ubi-user.h) */
	int32_t ubi_num;
	int32_t mtd_num;
	int32_t vid_hdr_offset;
	int16_t max_beb_per1024;
	int8_t padding[10];
};
#define UBI_IOCATT _IOW('o', 64, struct attach_req)

int main(int argc, char **argv)
{
	struct attach_req req = { 0 };
	int fd;

	if (argc != 4) {
		fprintf(stderr, "usage: ubi-attach MTD_NUM UBI_NUM MAX_BAD_PER_1024\n");
		return 2;
	}
	req.mtd_num = atoi(argv[1]);
	req.ubi_num = atoi(argv[2]);
	req.max_beb_per1024 = (int16_t)atoi(argv[3]);
	fd = open("/dev/ubi_ctrl", O_RDONLY);
	if (fd < 0 || ioctl(fd, UBI_IOCATT, &req)) {
		perror("ubi-attach");
		return 1;
	}
	close(fd);
	return 0;
}
