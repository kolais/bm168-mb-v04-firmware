/* SPDX-License-Identifier: MIT
 * Host fixture for nand-restore, linked with
 * -Wl,--wrap=open,--wrap=close,--wrap=pread,--wrap=read,--wrap=write,--wrap=ioctl.
 *
 * Simulates the bounded writer kernel: a two-eraseblock "autotop-write-window"
 * at physical 0x160000, the autotop_writer sysfs control and NAND semantics
 * (program only clears bits, erase sets 0xff). MEMWRITE applies the kernel's
 * checks: armed, inside the window, raw mode, full page, erased target and an
 * exact stock-format OOB. FAULT selects an injected failure. At exit the final
 * flash (2,048+64 per page) is written to $FIXTURE_DUMP and counters to stderr.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../bm168_nand_format.h"

#ifndef WRITER_SYSFS
#error "build with the same -DWRITER_SYSFS as nand-restore"
#endif

#define MTD_PATH "/dev/mtd14"
#define NAME_PATH "/sys/class/mtd/mtd14/name"
#define FD_MTD_RW 200
#define FD_MTD_RAW 201
#define FD_SYSFS 202
#define FD_NAME 203
#define BLOCKS 2
#define PAGES 64
#define SIZE (BLOCKS * BM168_ERASE)
#define WSTART 0x160000ull

static unsigned char flash[BLOCKS * PAGES][BM168_PAGE + BM168_OOB];
static int mtd_opens, raw_mode, armed = 1, sysfs_write_mode, erases, programs, disarms;
static unsigned corrected, failed, verify_reads;
static int initialized;

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
ssize_t __real_pread(int fd, void *buf, size_t len, off_t at);
ssize_t __real_read(int fd, void *buf, size_t len);
ssize_t __real_write(int fd, const void *buf, size_t len);
int __real_ioctl(int fd, unsigned long request, ...);

static int fault(const char *s)
{
	const char *v = getenv("FAULT");
	return v && !strcmp(v, s);
}

/* Old content: a different, valid stock-format pattern, with some erased pages. */
static void init_flash(void)
{
	unsigned n, i;

	if (initialized)
		return;
	initialized = 1;
	armed = !fault("disarmed");
	for (n = 0; n < BLOCKS * PAGES; n++) {
		if (n % 7 == 3) {
			memset(flash[n], 0xff, sizeof(flash[n]));
			continue;
		}
		for (i = 0; i < BM168_PAGE; i++)
			flash[n][i] = (unsigned char)(0x5a ^ n ^ (i * 13));
		bm168_page_oob(flash[n], flash[n] + BM168_PAGE);
	}
	if (fault("marker"))
		flash[PAGES + 1][BM168_PAGE] = 0x00; /* block 1, page 1 marker byte */
}

__attribute__((destructor)) static void dump(void)
{
	const char *path = getenv("FIXTURE_DUMP");
	FILE *f;

	fprintf(stderr, "FIXTURE erases=%d programs=%d disarms=%d armed=%d\n",
		erases, programs, disarms, armed);
	if (path && (f = fopen(path, "wb"))) {
		fwrite(flash, 1, sizeof(flash), f);
		fclose(f);
	}
}

int __wrap_open(const char *path, int flags, ...)
{
	va_list ap;
	int mode;

	init_flash();
	if (!strcmp(path, MTD_PATH)) {
		assert(mtd_opens < 2);
		return mtd_opens++ ? FD_MTD_RAW : FD_MTD_RW;
	}
	if (!strcmp(path, WRITER_SYSFS)) {
		sysfs_write_mode = (flags & O_ACCMODE) == O_WRONLY;
		return FD_SYSFS;
	}
	if (!strcmp(path, NAME_PATH))
		return FD_NAME;
	va_start(ap, flags);
	mode = va_arg(ap, int);
	va_end(ap);
	return __real_open(path, flags, mode);
}

int __wrap_close(int fd)
{
	if (fd >= FD_MTD_RW && fd <= FD_NAME)
		return 0;
	return __real_close(fd);
}

ssize_t __wrap_read(int fd, void *buf, size_t len)
{
	char state[128];
	int n;

	if (fd == FD_NAME) {
		const char *name = fault("wrong-name") ? "kernel\n" : "autotop-write-window\n";
		n = (int)strlen(name);
		memcpy(buf, name, (size_t)n);
		return n;
	}
	if (fd == FD_SYSFS) {
		n = snprintf(state, sizeof(state), "window=0x%llx+0x%x armed=%d erases=%d programs=%d rejects=0\n",
			     fault("window-mismatch") ? WSTART + BM168_ERASE : WSTART, SIZE, armed,
			     erases, programs);
		memcpy(buf, state, (size_t)n < len ? (size_t)n : len);
		return n;
	}
	return __real_read(fd, buf, len);
}

ssize_t __wrap_write(int fd, const void *buf, size_t len)
{
	if (fd == FD_SYSFS) {
		assert(sysfs_write_mode);
		if (len >= 6 && !memcmp(buf, "disarm", 6)) {
			armed = 0;
			disarms++;
			return (ssize_t)len;
		}
		abort(); /* the tool must never arm the writer itself */
	}
	return __real_write(fd, buf, len);
}

ssize_t __wrap_pread(int fd, void *buf, size_t len, off_t at)
{
	unsigned char *b = buf;
	size_t done = 0;

	if (fd != FD_MTD_RW && fd != FD_MTD_RAW)
		return __real_pread(fd, buf, len, at);
	assert(at >= 0 && at % BM168_PAGE == 0 && (size_t)at + len <= SIZE && len % BM168_PAGE == 0);
	if (fd == FD_MTD_RW && len == BM168_ERASE && erases)
		verify_reads++;
	while (done < len) {
		unsigned n = (unsigned)((at + done) / BM168_PAGE);
		memcpy(b + done, flash[n], BM168_PAGE);
		if (fd == FD_MTD_RAW && fault("bitflip") && programs && n == 2)
			b[done + 17] ^= 0x04;
		done += BM168_PAGE;
	}
	if (fd == FD_MTD_RW && fault("ecc-corrected") && verify_reads)
		corrected++;
	return (ssize_t)len;
}

static int do_ioctl(int fd, unsigned long request, void *arg)
{
	if (request == MTDFILEMODE) {
		assert(fd == FD_MTD_RAW && (uintptr_t)arg == MTD_FILE_MODE_RAW);
		raw_mode = 1;
		return 0;
	}
	if (request == MEMGETINFO) {
		struct mtd_info_user *i = arg;
		memset(i, 0, sizeof(*i));
		i->type = MTD_NANDFLASH;
		i->flags = MTD_WRITEABLE;
		i->size = SIZE;
		i->erasesize = BM168_ERASE;
		i->writesize = BM168_PAGE;
		i->oobsize = BM168_OOB;
		return 0;
	}
	if (request == MEMGETBADBLOCK) {
		int64_t at = *(int64_t *)arg;
		assert(at == 0 || at == BM168_ERASE);
		return fault("bad-block") && at == BM168_ERASE;
	}
	if (request == MEMREADOOB64) {
		struct mtd_oob_buf64 *r = arg;
		assert(fd == FD_MTD_RAW && r->length == BM168_OOB && r->start % BM168_PAGE == 0);
		memcpy((void *)(uintptr_t)r->usr_ptr, flash[r->start / BM168_PAGE] + BM168_PAGE, BM168_OOB);
		return 0;
	}
	if (request == ECCGETSTATS) {
		struct mtd_ecc_stats *s = arg;
		memset(s, 0, sizeof(*s));
		s->corrected = corrected;
		s->failed = failed;
		return 0;
	}
	if (request == MEMERASE64) {
		struct erase_info_user64 *e = arg;
		unsigned n;
		assert(fd == FD_MTD_RW && armed && e->start % BM168_ERASE == 0 &&
		       e->length == BM168_ERASE && e->start + e->length <= SIZE);
		if (fault("erase-fail")) {
			errno = EIO;
			return -1;
		}
		for (n = 0; n < PAGES; n++)
			memset(flash[e->start / BM168_PAGE + n], 0xff, BM168_PAGE + BM168_OOB);
		if (fault("erase-incomplete"))
			flash[e->start / BM168_PAGE + 5][100] = 0xfe;
		erases++;
		return 0;
	}
	if (request == MEMWRITE) {
		struct mtd_write_req *w = arg;
		const unsigned char *d = (const void *)(uintptr_t)w->usr_data;
		const unsigned char *o = (const void *)(uintptr_t)w->usr_oob;
		unsigned n = (unsigned)(w->start / BM168_PAGE), i;
		/* Kernel-side rules from 0007-bm168-bounded-writer.patch. */
		assert(fd == FD_MTD_RW && armed && w->mode == MTD_OPS_RAW && w->len == BM168_PAGE &&
		       w->ooblen == BM168_OOB && d && o && w->start % BM168_PAGE == 0 &&
		       w->start + BM168_PAGE <= SIZE);
		assert(bm168_window_contains(WSTART, SIZE, WSTART + w->start, BM168_PAGE));
		assert(!bm168_check_page(d, o));
		for (i = 0; i < BM168_PAGE + BM168_OOB; i++)
			assert(flash[n][i] == 0xff); /* never program a page twice */
		if (fault("program-fail") && programs == 2) {
			errno = EIO;
			return -1;
		}
		for (i = 0; i < BM168_PAGE; i++)
			flash[n][i] &= d[i];
		for (i = 0; i < BM168_OOB; i++)
			flash[n][BM168_PAGE + i] &= o[i];
		programs++;
		return 0;
	}
	abort(); /* markbad, OOB-only writes, locks and everything else are forbidden */
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fd != FD_MTD_RW && fd != FD_MTD_RAW)
		return __real_ioctl(fd, request, arg);
	init_flash();
	return do_ioctl(fd, request, arg);
}
