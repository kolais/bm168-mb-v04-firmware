/* SPDX-License-Identifier: MIT
 * Host fixture for nand-capture, linked with
 * -Wl,--wrap=open,--wrap=pread,--wrap=ioctl,--wrap=close.
 * Simulates a two-eraseblock read-only MTD device at /dev/mtd99ro. FAULT selects
 * an injected failure. Any ioctl other than the read-only set aborts the test.
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
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FAKE "/dev/mtd99ro"
#define FD_NORMAL 100
#define FD_RAW 101
static int opens, raw_mode;
static unsigned corrected, failed;

int __real_open(const char *path, int flags, ...);
ssize_t __real_pread(int fd, void *buf, size_t len, off_t at);
int __real_ioctl(int fd, unsigned long request, ...);
int __real_close(int fd);

static int fault(const char *s)
{
    const char *v = getenv("FAULT");
    return v && !strcmp(v, s);
}

int __wrap_open(const char *path, int flags, ...)
{
    va_list ap;
    int mode;

    if (!strcmp(path, FAKE)) {
        assert((flags & O_ACCMODE) == O_RDONLY);
        assert(opens < 2);
        return FD_NORMAL + opens++;
    }
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    return __real_open(path, flags, mode);
}

int __wrap_close(int fd)
{
    if (fd == FD_NORMAL || fd == FD_RAW) {
        assert(opens == 2);
        return 0;
    }
    return __real_close(fd);
}

ssize_t __wrap_pread(int fd, void *buf, size_t len, off_t at)
{
    unsigned char *b = buf;

    if (fd != FD_NORMAL && fd != FD_RAW)
        return __real_pread(fd, buf, len, at);
    assert(len == 2048 && at % 2048 == 0 && at < 262144);
    if (fault("read-error") && at == 133120) {
        errno = EIO;
        return -1;
    }
    for (size_t i = 0; i < len; i++)
        b[i] = (unsigned char)((at / 2048) + i);
    if (fd == FD_RAW) {
        assert(raw_mode);
        if (at == 4096)
            b[7] ^= 0x10; /* a raw bit error that ECC corrects */
    } else {
        if (at == 4096)
            corrected += 1;
        if (fault("uncorrectable") && at == 8192)
            failed += 1;
    }
    return fault("short-data") && at == 2048 ? 2047 : (ssize_t)len;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, request);
    if (fd != FD_NORMAL && fd != FD_RAW) {
        arg = va_arg(ap, void *);
        va_end(ap);
        return __real_ioctl(fd, request, arg);
    }
    if (request == MTDFILEMODE) {
        assert(fd == FD_RAW && va_arg(ap, int) == MTD_FILE_MODE_RAW);
        va_end(ap);
        raw_mode = 1;
        return 0;
    }
    arg = va_arg(ap, void *);
    va_end(ap);
    if (request == MEMGETINFO) {
        struct mtd_info_user *i = arg;
        memset(i, 0, sizeof(*i));
        i->type = MTD_NANDFLASH;
        i->size = 262144;
        i->erasesize = 131072;
        i->writesize = 2048;
        i->oobsize = fault("geometry") ? 128 : 64;
        i->flags = fault("writable") ? MTD_WRITEABLE : 0;
        return 0;
    }
    if (request == MEMGETBADBLOCK) {
        int64_t at = *(int64_t *)arg;
        assert(at == 0 || at == 131072);
        if (fault("bad-ioctl")) {
            errno = EIO;
            return -1;
        }
        return at == 131072;
    }
    if (request == MEMREADOOB64) {
        struct mtd_oob_buf64 *r = arg;
        unsigned char *b = (void *)(uintptr_t)r->usr_ptr;
        assert(fd == FD_RAW && r->length == 64);
        memset(b, 0xff, 64);
        b[3] = (unsigned char)(r->start / 2048);
        if (fault("short-oob"))
            r->length = 63;
        return 0;
    }
    if (request == ECCGETSTATS) {
        struct mtd_ecc_stats *s = arg;
        assert(fd == FD_NORMAL);
        memset(s, 0, sizeof(*s));
        s->corrected = corrected;
        s->failed = failed;
        return 0;
    }
    abort(); /* No other ioctl, especially an erase/write/markbad, is allowed. */
}
