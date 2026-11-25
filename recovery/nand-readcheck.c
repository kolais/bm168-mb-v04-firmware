/* SPDX-License-Identifier: MIT
 * BM168 read-only marker probe. No erase, program or markbad operations.
 * Offsets in the output are partition-relative main-area byte offsets.
 */
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <mtd/mtd-user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void fail(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

static void oob_read(int fd, uint64_t offset, unsigned char *buf)
{
    struct mtd_oob_buf64 req = {
        .start = offset, .length = 64, .usr_ptr = (uintptr_t)buf,
    };
    if (ioctl(fd, MEMREADOOB64, &req) < 0)
        fail("MEMREADOOB64");
    if (req.length != 64) {
        fprintf(stderr, "short OOB read at 0x%" PRIx64 "\n", offset);
        exit(EXIT_FAILURE);
    }
}

static void hex(const unsigned char *buf, size_t size)
{
    for (size_t i = 0; i < size; i++)
        printf("%02x", buf[i]);
}

int main(int argc, char **argv)
{
    struct mtd_info_user info;
    unsigned char normal[64], raw[64], repeat[64], data[2048];
    unsigned bad_count = 0;
    int fd, rawfd;

    if (argc != 2) {
        fprintf(stderr, "usage: nand-readcheck /dev/mtdNro\n");
        return EXIT_FAILURE;
    }
    /* Two descriptors keep normal and RAW file modes independent. */
    fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        fail("open normal");
    rawfd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (rawfd < 0)
        fail("open raw");
    if (ioctl(fd, MEMGETINFO, &info) < 0)
        fail("MEMGETINFO");
    if (info.type != MTD_NANDFLASH || info.writesize != 2048 ||
        info.oobsize != 64 || info.erasesize != 131072 ||
        !info.size || info.size > 134217728 || info.size % info.erasesize) {
        fprintf(stderr, "unsupported geometry\n");
        return EXIT_FAILURE;
    }
    if (ioctl(rawfd, MTDFILEMODE, MTD_FILE_MODE_RAW) < 0)
        fail("MTDFILEMODE RAW");
    printf("BEGIN device=%s size=%u erase=%u page=%u oob=%u flags=0x%x\n",
           argv[1], info.size, info.erasesize, info.writesize,
           info.oobsize, info.flags);

    for (uint64_t block = 0; block < info.size; block += info.erasesize) {
        /* MEMGETBADBLOCK takes a signed 64-bit offset on both target ABIs. */
        int64_t offset = (int64_t)block;
        int bad = ioctl(fd, MEMGETBADBLOCK, &offset);
        int marker_bad = 0;
        if (bad < 0)
            fail("MEMGETBADBLOCK");
        for (unsigned page = 0; page < 2; page++) {
            uint64_t at = block + page * info.writesize;
            /* RAW read does not skip bad blocks or apply ECC correction. */
            ssize_t count = pread(rawfd, data, sizeof(data), (off_t)at);
            if (count < 0)
                fail("raw pread");
            if (count != (ssize_t)sizeof(data)) {
                fprintf(stderr, "short raw read at 0x%" PRIx64 "\n", at);
                return EXIT_FAILURE;
            }
            oob_read(fd, at, normal);
            oob_read(rawfd, at, raw);
            oob_read(rawfd, at, repeat);
            if (memcmp(normal, raw, sizeof(raw)) ||
                memcmp(raw, repeat, sizeof(raw))) {
                fprintf(stderr, "OOB mode/repeat mismatch at 0x%" PRIx64 "\n", at);
                return EXIT_FAILURE;
            }
            marker_bad |= raw[0] != 0xff;
            printf("PAGE offset=0x%08" PRIx64 " main16=", at);
            hex(data, 16);
            printf(" oob=");
            hex(raw, sizeof(raw));
            putchar('\n');
        }
        if (!!bad != marker_bad) {
            fprintf(stderr, "marker/ioctl mismatch at 0x%" PRIx64
                    " ioctl=%d marker=%d\n", block, bad, marker_bad);
            return EXIT_FAILURE;
        }
        printf("BLOCK offset=0x%08" PRIx64 " bad=%d\n", block, !!bad);
        bad_count += !!bad;
    }
    if (close(fd) || close(rawfd))
        fail("close");
    printf("PASS device=%s blocks=%u bad=%u oob_modes_match=1 repeats_match=1\n",
           argv[1], info.size / info.erasesize, bad_count);
    return fflush(stdout) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
