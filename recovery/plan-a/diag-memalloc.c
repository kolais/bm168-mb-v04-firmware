/* SPDX-License-Identifier: MIT
 * Step-by-step check of the SDK allocator path used by libarkapi:
 * MEMALLOC_IOCXGETBUFFER on /dev/memalloc, then mmap of the returned bus
 * address through /dev/mem, a write/readback, and the free. Prints every
 * result, so a failure is located without a debugger.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct { unsigned busAddress; unsigned size; } MemallocParams;
#define MEMALLOC_IOCXGETBUFFER _IOWR('k', 1, MemallocParams *)
#define MEMALLOC_IOCSFREEBUFFER _IOW('k', 2, unsigned long)

int main(void)
{
	MemallocParams p = { 0, 1024 * 1024 };
	int fd, mem, ret;
	volatile unsigned char *v;

	setvbuf(stdout, NULL, _IONBF, 0);
	fd = open("/dev/memalloc", O_RDWR | O_SYNC);
	printf("open /dev/memalloc fd=%d errno=%d\n", fd, fd < 0 ? errno : 0);
	if (fd < 0)
		return 1;
	ret = ioctl(fd, MEMALLOC_IOCXGETBUFFER, &p);
	printf("GETBUFFER ret=%d errno=%d bus=0x%08x size=%u\n", ret, ret ? errno : 0, p.busAddress, p.size);
	if (ret)
		return 1;
	mem = open("/dev/mem", O_RDWR | O_SYNC);
	printf("open /dev/mem fd=%d errno=%d\n", mem, mem < 0 ? errno : 0);
	if (mem < 0)
		return 1;
	v = mmap(0, p.size, PROT_READ | PROT_WRITE, MAP_SHARED, mem, p.busAddress);
	printf("mmap -> %p errno=%d\n", (void *)v, v == MAP_FAILED ? errno : 0);
	if (v == MAP_FAILED)
		return 1;
	v[0] = 0x5a;
	v[p.size - 1] = 0xa5;
	printf("readback first=0x%02x last=0x%02x\n", v[0], v[p.size - 1]);
	munmap((void *)v, p.size);
	ret = ioctl(fd, MEMALLOC_IOCSFREEBUFFER, &p.busAddress);
	printf("FREEBUFFER ret=%d\nDIAG DONE\n", ret);
	return 0;
}
