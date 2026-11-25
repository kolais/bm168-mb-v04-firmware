/* SPDX-License-Identifier: MIT
 * BM168 bounded NAND restore: fixed physical replay of original ECC-corrected
 * main data into the kernel's armed write window, as stock-format pages.
 *
 *   nand-restore plan    /dev/mtdN IMAGE MANIFEST   report only, never writes
 *   nand-restore execute /dev/mtdN IMAGE MANIFEST   erase/program/verify
 *
 * /dev/mtdN must be the "autotop-write-window" partition. IMAGE is exactly the
 * window's corrected main bytes; or PATH@OFFSET, the same bytes from OFFSET
 * (a multiple of 4096) on inside a larger file: a rescue container on a
 * stick, or one U-Boot left in RAM, reached through /dev/mem
 * (recovery/plan-a/init-rescue). That form is mapped, not read: memory
 * above what Linux was given can be mapped from /dev/mem but not read. MANIFEST (tools/restore_manifest.py) gives the
 * window, the image SHA-256 and one SHA-256 per eraseblock. Every block must
 * pass preflight before the first erase. There is no skipping, retrying,
 * relocation or markbad: any unexpected state stops the run and disarms.
 */
#define _FILE_OFFSET_BITS 64
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <mtd/mtd-user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "bm168_nand_format.h"

#ifndef WRITER_SYSFS
#define WRITER_SYSFS "/sys/bus/platform/devices/ec000000.nand/autotop_writer"
#endif
#define PAGES (BM168_ERASE / BM168_PAGE)
#define MAX_BLOCKS 904u	/* kernel and rootfs together: 0x160000 + 0x7100000 */

/* ---- SHA-256 (FIPS 180-4) ---- */
struct sha256 { uint32_t h[8]; uint64_t n; unsigned char b[64]; size_t used; };
static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(struct sha256 *s, const unsigned char *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		       (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (; i < 64; i++)
		w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
		       w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (i = 0; i < 64; i++) {
		uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(struct sha256 *s)
{
	static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
					0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	memcpy(s->h, iv, sizeof(iv));
	s->n = 0;
	s->used = 0;
}
static void sha_update(struct sha256 *s, const unsigned char *p, size_t len)
{
	s->n += len;
	while (len) {
		size_t take = 64 - s->used < len ? 64 - s->used : len;
		memcpy(s->b + s->used, p, take);
		s->used += take; p += take; len -= take;
		if (s->used == 64) {
			sha_block(s, s->b);
			s->used = 0;
		}
	}
}
static void sha_hex(struct sha256 *s, char out[65])
{
	uint64_t bits = s->n * 8;
	unsigned char pad = 0x80, len[8];
	int i;

	sha_update(s, &pad, 1);
	pad = 0;
	while (s->used != 56)
		sha_update(s, &pad, 1);
	for (i = 0; i < 8; i++)
		len[i] = (unsigned char)(bits >> (56 - 8 * i));
	sha_update(s, len, 8);
	for (i = 0; i < 8; i++)
		sprintf(out + 8 * i, "%08x", s->h[i]);
}
static void sha_of(const unsigned char *p, size_t len, char out[65])
{
	struct sha256 s;

	sha_init(&s);
	sha_update(&s, p, len);
	sha_hex(&s, out);
}

/* ---- run state ---- */
static int execute, mtd_fd = -1, raw_fd = -1;

static void writer_ctl(const char *cmd)
{
	int fd = open(WRITER_SYSFS, O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, cmd, strlen(cmd)) < 0)
			perror("writer control");
		close(fd);
	}
}

static void stop(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void stop(const char *fmt, ...)
{
	va_list ap;

	printf("STOP ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	if (execute)
		writer_ctl("disarm");
	fflush(stdout);
	exit(EXIT_FAILURE);
}

static void read_all(int fd, void *buf, size_t len, off_t at, const char *what)
{
	ssize_t n = pread(fd, buf, len, at);

	if (n < 0)
		stop("%s read failed at 0x%llx: %s", what, (unsigned long long)at, strerror(errno));
	if ((size_t)n != len)
		stop("%s short read at 0x%llx", what, (unsigned long long)at);
}

/* The image: a file of exactly the window, or the window's bytes mapped from
 * inside a larger one. */
static int img_fd = -1;
static const unsigned char *img_map;

static void image_read(void *buf, uint64_t at)
{
	if (img_map)
		memcpy(buf, img_map + at, BM168_ERASE);
	else
		read_all(img_fd, buf, BM168_ERASE, (off_t)at, "image");
}

static void read_oob(int fd, uint64_t at, unsigned char *oob)
{
	struct mtd_oob_buf64 req = { .start = at, .length = BM168_OOB, .usr_ptr = (uintptr_t)oob };

	if (ioctl(fd, MEMREADOOB64, &req) < 0 || req.length != BM168_OOB)
		stop("OOB read failed at 0x%" PRIx64, at);
}

static void ecc_stats(struct mtd_ecc_stats *s)
{
	if (ioctl(mtd_fd, ECCGETSTATS, s) < 0)
		stop("ECCGETSTATS failed: %s", strerror(errno));
}

/* Writer state from the kernel: "window=0x..+0x.. armed=N ..." */
static void writer_state(uint64_t *start, uint64_t *size, int *armed)
{
	char buf[256];
	unsigned long long s, z;
	int fd = open(WRITER_SYSFS, O_RDONLY | O_CLOEXEC), a;
	ssize_t n;

	if (fd < 0)
		stop("writer control %s missing: not a writer kernel", WRITER_SYSFS);
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		stop("writer state unreadable");
	buf[n] = 0;
	if (sscanf(buf, "window=0x%llx+0x%llx armed=%d", &s, &z, &a) != 3)
		stop("writer state unparsable: %s", buf);
	*start = s;
	*size = z;
	*armed = a;
}

struct manifest {
	uint64_t start, size;
	char image[65];
	char block[MAX_BLOCKS][65];
	unsigned blocks;
	unsigned max_fresh;	/* corrected bits tolerated per freshly written block */
	unsigned resume;	/* first block to write; earlier blocks are only checked */
};

static void parse_manifest(const char *path, struct manifest *m)
{
	FILE *f = fopen(path, "r");
	char line[256], hex[80];
	unsigned long long a, b;
	unsigned idx, seen_header = 0;

	if (!f)
		stop("manifest %s: %s", path, strerror(errno));
	memset(m, 0, sizeof(*m));
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (!strcmp(line, "AUTOTOP-RESTORE v1\n"))
			seen_header = 1;
		else if (sscanf(line, "window 0x%llx 0x%llx", &a, &b) == 2) {
			m->start = a;
			m->size = b;
		} else if (sscanf(line, "max_fresh_corrections %u", &idx) == 1 && idx <= 7)
			m->max_fresh = idx;
		else if (sscanf(line, "resume_from_block %u", &idx) == 1 && idx < MAX_BLOCKS)
			m->resume = idx;
		else if (sscanf(line, "image_sha256 %64[0-9a-f]", hex) == 1 && strlen(hex) == 64)
			strcpy(m->image, hex);
		else if (sscanf(line, "block %u %64[0-9a-f]", &idx, hex) == 2 && strlen(hex) == 64 &&
			 idx == m->blocks && idx < MAX_BLOCKS)
			strcpy(m->block[m->blocks++], hex);
		else
			stop("manifest line not understood: %s", line);
	}
	fclose(f);
	if (!seen_header || !m->image[0] || !bm168_window_valid(m->start, m->size) ||
	    m->blocks != m->size / BM168_ERASE || m->resume >= m->blocks)
		stop("manifest incomplete or inconsistent");
}

static int all_ff(const unsigned char *p, size_t n)
{
	while (n--)
		if (*p++ != 0xff)
			return 0;
	return 1;
}

int main(int argc, char **argv)
{
	static unsigned char image_block[BM168_ERASE], flash_block[BM168_ERASE];
	static struct manifest m;
	unsigned char page[BM168_PAGE], oob[BM168_OOB], expect[BM168_OOB];
	char hex[65], name[64];
	struct mtd_info_user info;
	struct mtd_ecc_stats e0, e1;
	struct sha256 whole;
	uint64_t wstart, wsize;
	int armed, fd;
	off_t img_base = 0;	/* where the window's bytes start in IMAGE */
	char *at_sign;
	unsigned b, p, programmed = 0, raw_flips;
	ssize_t n;

	if (argc != 5 || (strcmp(argv[1], "plan") && strcmp(argv[1], "execute"))) {
		fprintf(stderr, "usage: nand-restore plan|execute /dev/mtdN IMAGE MANIFEST\n");
		return EXIT_FAILURE;
	}
	execute = !strcmp(argv[1], "execute");
	setvbuf(stdout, NULL, _IOLBF, 0);
	parse_manifest(argv[4], &m);
	writer_state(&wstart, &wsize, &armed);
	if (wstart != m.start || wsize != m.size)
		stop("kernel window 0x%llx+0x%llx differs from manifest 0x%llx+0x%llx",
		     (unsigned long long)wstart, (unsigned long long)wsize,
		     (unsigned long long)m.start, (unsigned long long)m.size);
	if (execute && !armed)
		stop("writer is not armed");

	/* The node must be the window partition, by name and geometry. */
	if (strncmp(argv[2], "/dev/mtd", 8) || strlen(argv[2]) > 12 || strstr(argv[2], "ro"))
		stop("device must be a /dev/mtdN node");
	snprintf(name, sizeof(name), "/sys/class/mtd/%s/name", argv[2] + 5);
	fd = open(name, O_RDONLY | O_CLOEXEC);
	n = fd >= 0 ? read(fd, hex, sizeof(hex) - 1) : -1;
	if (fd >= 0)
		close(fd);
	if (n <= 0 || (hex[n] = 0, strcmp(hex, "autotop-write-window\n")))
		stop("%s is not the autotop-write-window partition", argv[2]);
	mtd_fd = open(argv[2], (execute ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	raw_fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	if (mtd_fd < 0 || raw_fd < 0)
		stop("open %s: %s", argv[2], strerror(errno));
	if (ioctl(mtd_fd, MEMGETINFO, &info) < 0 || info.type != MTD_NANDFLASH ||
	    info.writesize != BM168_PAGE || info.oobsize != BM168_OOB ||
	    info.erasesize != BM168_ERASE || info.size != m.size ||
	    (execute && !(info.flags & MTD_WRITEABLE)))
		stop("unexpected window geometry or flags");
	if (ioctl(raw_fd, MTDFILEMODE, MTD_FILE_MODE_RAW) < 0)
		stop("MTDFILEMODE RAW failed");

	/* Image: exact size, whole hash and per-block hashes. */
	at_sign = strrchr(argv[3], '@');
	if (at_sign) {
		char *end;

		errno = 0;
		img_base = (off_t)strtoull(at_sign + 1, &end, 0);
		if (errno || end == at_sign + 1 || *end)
			stop("image offset in %s is not a number", argv[3]);
		*at_sign = 0;
	}
	img_fd = open(argv[3], O_RDONLY | O_CLOEXEC);
	if (img_fd < 0)
		stop("image %s: %s", argv[3], strerror(errno));
	/* A plain file is the window and nothing else; inside a container the
	 * hashes below are the whole check (a memory device has no size). */
	if (at_sign) {
		void *map;

		if (img_base % 4096)
			stop("image offset must be a multiple of 4096");
		map = mmap(NULL, (size_t)m.size, PROT_READ, MAP_SHARED, img_fd, img_base);
		if (map == MAP_FAILED)
			stop("image %s cannot be mapped at 0x%llx: %s", argv[3], (unsigned long long)img_base,
			     strerror(errno));
		img_map = map;
	} else if (lseek(img_fd, 0, SEEK_END) != (off_t)m.size) {
		stop("image size differs from the window");
	}
	sha_init(&whole);
	for (b = 0; b < m.blocks; b++) {
		image_read(image_block, (uint64_t)b * BM168_ERASE);
		sha_update(&whole, image_block, BM168_ERASE);
		sha_of(image_block, BM168_ERASE, hex);
		if (strcmp(hex, m.block[b]))
			stop("image block %u hash differs from manifest", b);
	}
	sha_hex(&whole, hex);
	if (strcmp(hex, m.image))
		stop("image hash differs from manifest");
	printf("BEGIN mode=%s window=0x%" PRIx64 "+0x%" PRIx64 " blocks=%u image_sha256=%s "
	       "max_fresh_corrections=%u resume_from_block=%u armed=%d\n",
	       argv[1], m.start, m.size, m.blocks, m.image, m.max_fresh, m.resume, armed);

	/* Preflight every block before any erase. */
	for (b = 0; b < m.blocks; b++) {
		uint64_t at = (uint64_t)b * BM168_ERASE;
		int64_t ofs = (int64_t)at;
		int bad = ioctl(mtd_fd, MEMGETBADBLOCK, &ofs);
		unsigned char m0[BM168_OOB], m1[BM168_OOB];
		unsigned pages = 0;

		if (bad < 0)
			stop("MEMGETBADBLOCK failed at block %u", b);
		read_oob(raw_fd, at, m0);
		read_oob(raw_fd, at + BM168_PAGE, m1);
		image_read(image_block, at);
		for (p = 0; p < PAGES; p++)
			pages += !all_ff(image_block + p * BM168_PAGE, BM168_PAGE);
		ecc_stats(&e0);
		read_all(mtd_fd, flash_block, BM168_ERASE, (off_t)at, "flash");
		ecc_stats(&e1);
		sha_of(flash_block, BM168_ERASE, hex);
		printf("PLAN block=%u phys=0x%08" PRIx64 " bad=%d marker0=%02x marker1=%02x "
		       "current_sha256=%s image_sha256=%s same=%d program_pages=%u "
		       "ecc_corrected=%u ecc_failed=%u\n",
		       b, m.start + at, !!bad, m0[0], m1[0], hex, m.block[b], !strcmp(hex, m.block[b]),
		       pages, e1.corrected - e0.corrected, e1.failed - e0.failed);
		if (bad || m0[0] != 0xff || m1[0] != 0xff)
			stop("block %u is bad or has a bad-block marker", b);
	}
	if (!execute) {
		printf("PLAN_OK blocks=%u (no erase or program issued)\n", m.blocks);
		return EXIT_SUCCESS;
	}

	for (b = m.resume; b < m.blocks; b++) {
		uint64_t at = (uint64_t)b * BM168_ERASE;
		int64_t ofs = (int64_t)at;
		struct erase_info_user64 er = { .start = at, .length = BM168_ERASE };
		unsigned char m0[BM168_OOB], m1[BM168_OOB];

		image_read(image_block, at);
		sha_of(image_block, BM168_ERASE, hex);
		if (strcmp(hex, m.block[b]))
			stop("image block %u changed since preflight", b);
		read_oob(raw_fd, at, m0);
		read_oob(raw_fd, at + BM168_PAGE, m1);
		if (ioctl(mtd_fd, MEMGETBADBLOCK, &ofs) != 0 || m0[0] != 0xff || m1[0] != 0xff)
			stop("block %u bad-block state changed since preflight", b);

		if (ioctl(mtd_fd, MEMERASE64, &er) < 0)
			stop("erase failed at block %u: %s", b, strerror(errno));
		for (p = 0; p < PAGES; p++) {
			uint64_t pa = at + (uint64_t)p * BM168_PAGE;
			read_all(raw_fd, page, BM168_PAGE, (off_t)pa, "erased raw");
			read_oob(raw_fd, pa, oob);
			if (!all_ff(page, BM168_PAGE) || !all_ff(oob, BM168_OOB))
				stop("block %u page %u not erased after erase", b, p);
		}
		printf("ERASED block=%u phys=0x%08" PRIx64 "\n", b, m.start + at);

		for (p = 0; p < PAGES; p++) {
			unsigned char *src = image_block + p * BM168_PAGE;
			struct mtd_write_req req = {
				.start = at + (uint64_t)p * BM168_PAGE, .len = BM168_PAGE,
				.ooblen = BM168_OOB, .usr_data = (uintptr_t)src,
				.usr_oob = (uintptr_t)expect, .mode = MTD_OPS_RAW,
			};

			if (all_ff(src, BM168_PAGE))
				continue;	/* identical to the erased page in this format */
			bm168_page_oob(src, expect);
			if (ioctl(mtd_fd, MEMWRITE, &req) < 0)
				stop("program failed at block %u page %u: %s", b, p, strerror(errno));
			programmed++;
		}

		raw_flips = 0;
		for (p = 0; p < PAGES; p++) {
			uint64_t pa = at + (uint64_t)p * BM168_PAGE;
			unsigned char *src = image_block + p * BM168_PAGE;
			unsigned i, flips = 0;

			read_all(raw_fd, page, BM168_PAGE, (off_t)pa, "verify raw");
			read_oob(raw_fd, pa, oob);
			if (all_ff(src, BM168_PAGE))
				memset(expect, 0xff, BM168_OOB);
			else
				bm168_page_oob(src, expect);
			/*
			 * Count raw bit flips in main + OOB. A few are tolerated only when
			 * the reviewed manifest allows them; the corrected hash below must
			 * still match exactly.
			 */
			for (i = 0; i < BM168_PAGE; i++)
				flips += (unsigned)__builtin_popcount(page[i] ^ src[i]);
			for (i = 0; i < BM168_OOB; i++)
				flips += (unsigned)__builtin_popcount(oob[i] ^ expect[i]);
			raw_flips += flips;
			if (raw_flips > m.max_fresh)
				stop("block %u page %u raw readback differs (%u bits in block)", b, p, raw_flips);
			if (flips)
				printf("TOLERATED_RAW block=%u page=%u bits=%u\n", b, p, flips);
		}
		ecc_stats(&e0);
		read_all(mtd_fd, flash_block, BM168_ERASE, (off_t)at, "verify corrected");
		ecc_stats(&e1);
		sha_of(flash_block, BM168_ERASE, hex);
		printf("VERIFIED block=%u phys=0x%08" PRIx64 " sha256=%s ecc_corrected=%u ecc_failed=%u\n",
		       b, m.start + at, hex, e1.corrected - e0.corrected, e1.failed - e0.failed);
		if (strcmp(hex, m.block[b]) || e1.failed != e0.failed)
			stop("block %u corrected readback differs or failed ECC", b);
		/*
		 * Freshly programmed data must read back without corrections, unless
		 * the reviewed manifest tolerates a few (data is still verified exact).
		 */
		if (e1.corrected - e0.corrected > m.max_fresh)
			stop("block %u needed ECC corrections after programming; investigate", b);
		if (e1.corrected != e0.corrected)
			printf("TOLERATED block=%u ecc_corrected=%u\n", b, e1.corrected - e0.corrected);
	}
	writer_ctl("disarm");
	printf("RESULT PASS blocks=%u written_from=%u programmed_pages=%u (writer disarmed)\n",
	       m.blocks, m.resume, programmed);
	return EXIT_SUCCESS;
}
