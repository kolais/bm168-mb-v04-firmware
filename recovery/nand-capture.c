/* SPDX-License-Identifier: MIT
 * BM168 read-only NAND acquisition. No erase, program or markbad operations.
 *
 * nand-capture /dev/mtdNro /mnt/usb/CAPTURE/<run>/<name>
 *   <name>.raw  every page: 2,048 raw main bytes, then 64 raw OOB bytes
 *   <name>.ecc  every page: 2,048 ECC-corrected main bytes
 *   <name>.log  geometry, bad-block ioctl results and per-page ECC events
 * Outputs are created exclusively (never overwritten) under /mnt/usb/CAPTURE/.
 * Uncorrectable pages are recorded and the capture continues; the exit status
 * is 2 if any were found, so the procedure can distinguish them from I/O errors.
 */
#define _FILE_OFFSET_BITS 64
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <mtd/mtd-user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PAGE 2048u
#define OOB 64u
#define ERASE 131072u
#define PAGES_PER_BLOCK (ERASE / PAGE)
#ifndef CAPTURE_ROOT
#define CAPTURE_ROOT "/mnt/usb/CAPTURE/"
#endif
#define ROOT CAPTURE_ROOT

static void fail(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

static void reject(const char *why)
{
    fprintf(stderr, "nand-capture: %s\n", why);
    exit(EXIT_FAILURE);
}

/* Stock U-Boot scans CARPLAY media for update inputs; never create them. */
static void check_name(const char *path)
{
    static const char *const vendor[] = {
        "update", "arkupdate", "uboot.bin", "uboot-env.bin", "zimage", "rootfs.img",
        "userdata.img", "arkdata.ini", "nboot.bin", "stepldr.bin", NULL,
    };
    char lower[PATH_MAX];
    size_t n = strlen(path);

    if (n >= sizeof(lower) - 8 || strncmp(path, ROOT, strlen(ROOT)) || strstr(path, ".."))
        reject("output prefix must be below " ROOT " without '..'");
    for (size_t i = 0; i <= n; i++)
        lower[i] = (char)tolower((unsigned char)path[i]);
    for (const char *component = lower + 1; component && *component;) {
        const char *end = strchr(component, '/');
        size_t len = end ? (size_t)(end - component) : strlen(component);
        for (int i = 0; vendor[i]; i++)
            if (len >= strlen(vendor[i]) && !strncmp(component, vendor[i], strlen(vendor[i])))
                reject("output path resembles a vendor update input");
        component = end ? end + 1 : NULL;
    }
}

static int create(const char *prefix, const char *suffix)
{
    char path[PATH_MAX];
    int fd;

    if (snprintf(path, sizeof(path), "%s%s", prefix, suffix) >= (int)sizeof(path))
        reject("output path too long");
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        fail(path);
    return fd;
}

static void write_all(int fd, const void *buf, size_t size)
{
    const unsigned char *p = buf;

    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            fail("write");
        p += n;
        size -= (size_t)n;
    }
}

static void read_exact(int fd, void *buf, size_t size, uint64_t at, const char *what)
{
    ssize_t n = pread(fd, buf, size, (off_t)at);

    /* mtdchar returns corrected/uncorrectable data with the full length;
     * ECC state is taken from ECCGETSTATS, not from this return value. */
    if (n < 0)
        fail(what);
    if ((size_t)n != size) {
        fprintf(stderr, "short %s at 0x%" PRIx64 "\n", what, at);
        exit(EXIT_FAILURE);
    }
}

static void finish(int fd)
{
    if (fsync(fd) || close(fd))
        fail("fsync/close");
}

int main(int argc, char **argv)
{
    static unsigned char raw_block[PAGES_PER_BLOCK * (PAGE + OOB)];
    static unsigned char ecc_block[ERASE];
    struct mtd_info_user info;
    struct mtd_ecc_stats before, after;
    unsigned long corrected_pages = 0, failed_pages = 0, bad_blocks = 0;
    int fd, rawfd, out_raw, out_ecc;
    FILE *log;

    if (argc != 3) {
        fprintf(stderr, "usage: nand-capture /dev/mtdNro " ROOT "<run>/<name>\n");
        return EXIT_FAILURE;
    }
    if (strncmp(argv[1], "/dev/mtd", 8) || strcmp(argv[1] + strlen(argv[1]) - 2, "ro"))
        reject("input must be a read-only /dev/mtdNro node");
    check_name(argv[2]);
    fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        fail("open normal");
    rawfd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (rawfd < 0)
        fail("open raw");
    if (ioctl(fd, MEMGETINFO, &info) < 0)
        fail("MEMGETINFO");
    if (info.type != MTD_NANDFLASH || info.writesize != PAGE || info.oobsize != OOB ||
        info.erasesize != ERASE || !info.size || info.size > 134217728u ||
        info.size % ERASE || (info.flags & MTD_WRITEABLE))
        reject("unsupported geometry or writable MTD device");
    if (ioctl(rawfd, MTDFILEMODE, MTD_FILE_MODE_RAW) < 0)
        fail("MTDFILEMODE RAW");

    out_raw = create(argv[2], ".raw");
    out_ecc = create(argv[2], ".ecc");
    log = fdopen(create(argv[2], ".log"), "w");
    if (!log)
        fail("fdopen log");
    fprintf(log, "BEGIN device=%s size=%u erase=%u page=%u oob=%u flags=0x%x\n",
            argv[1], info.size, info.erasesize, info.writesize, info.oobsize, info.flags);
    if (ioctl(fd, ECCGETSTATS, &before) < 0)
        fail("ECCGETSTATS");
    fprintf(log, "ECC_BEFORE corrected=%u failed=%u\n", before.corrected, before.failed);

    for (uint64_t block = 0; block < info.size; block += ERASE) {
        int64_t offset = (int64_t)block;
        int bad = ioctl(fd, MEMGETBADBLOCK, &offset);

        if (bad < 0)
            fail("MEMGETBADBLOCK");
        fprintf(log, "BLOCK offset=0x%08" PRIx64 " bad=%d\n", block, !!bad);
        bad_blocks += !!bad;
        for (unsigned page = 0; page < PAGES_PER_BLOCK; page++) {
            uint64_t at = block + (uint64_t)page * PAGE;
            unsigned char *raw = raw_block + page * (PAGE + OOB);
            struct mtd_oob_buf64 req = {
                .start = at, .length = OOB, .usr_ptr = (uintptr_t)(raw + PAGE),
            };
            struct mtd_ecc_stats s0, s1;

            /* RAW mode: no ECC correction and no bad-block substitution. */
            read_exact(rawfd, raw, PAGE, at, "raw read");
            if (ioctl(rawfd, MEMREADOOB64, &req) < 0)
                fail("MEMREADOOB64");
            if (req.length != OOB)
                reject("short OOB read");
            if (ioctl(fd, ECCGETSTATS, &s0) < 0)
                fail("ECCGETSTATS");
            read_exact(fd, ecc_block + page * PAGE, PAGE, at, "corrected read");
            if (ioctl(fd, ECCGETSTATS, &s1) < 0)
                fail("ECCGETSTATS");
            if (s1.failed != s0.failed) {
                failed_pages++;
                fprintf(log, "ECC_FAILED offset=0x%08" PRIx64 " count=%u\n", at, s1.failed - s0.failed);
            }
            if (s1.corrected != s0.corrected) {
                corrected_pages++;
                fprintf(log, "ECC_CORRECTED offset=0x%08" PRIx64 " bits=%u\n", at,
                        s1.corrected - s0.corrected);
            }
        }
        write_all(out_raw, raw_block, sizeof(raw_block));
        write_all(out_ecc, ecc_block, sizeof(ecc_block));
    }
    if (ioctl(fd, ECCGETSTATS, &after) < 0)
        fail("ECCGETSTATS");
    fprintf(log, "ECC_AFTER corrected=%u failed=%u\n", after.corrected, after.failed);
    fprintf(log, "END blocks=%u bad=%lu corrected_pages=%lu failed_pages=%lu\n",
            info.size / ERASE, bad_blocks, corrected_pages, failed_pages);
    finish(out_raw);
    finish(out_ecc);
    if (fflush(log) || fsync(fileno(log)) || fclose(log))
        fail("log close");
    if (close(fd) || close(rawfd))
        fail("close");
    printf("%s device=%s blocks=%u bad=%lu corrected_pages=%lu failed_pages=%lu\n",
           failed_pages ? "UNCORRECTABLE" : "PASS", argv[1], info.size / ERASE,
           bad_blocks, corrected_pages, failed_pages);
    return failed_pages ? 2 : EXIT_SUCCESS;
}
