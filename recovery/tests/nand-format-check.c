/* SPDX-License-Identifier: MIT
 * Host checks for recovery/bm168_nand_format.h.
 *   nand-format-check encode IMAGE   print "<offset> <oob hex>" per 2,048-byte page,
 *                                    the same format as tools/nand_ecc.py encode
 *   nand-format-check capture RAW    every non-erased page of a 2,048+64 raw capture:
 *                                    count exact stock-format matches
 *   nand-format-check window         window/containment rules
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bm168_nand_format.h"

static unsigned char *slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long n;

	if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
		exit(2);
	buf = malloc((size_t)n + 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
		exit(2);
	fclose(f);
	*size = (size_t)n;
	return buf;
}

static int window_checks(void)
{
	struct { uint64_t s, z, a, l; int valid, contains; } t[] = {
		{ 0x160000, 0x20000, 0x160000, 0x20000, 1, 1 },
		{ 0x160000, 0x20000, 0x160000, 0x800, 1, 1 },
		{ 0x160000, 0x20000, 0x17f800, 0x800, 1, 1 },
		{ 0x160000, 0x20000, 0x180000, 0x800, 1, 0 },   /* just past the end */
		{ 0x160000, 0x20000, 0x15f800, 0x800, 1, 0 },   /* just before */
		{ 0x160000, 0x20000, 0x17f800, 0x1000, 1, 0 },  /* straddles the end */
		{ 0x160000, 0x20000, 0x160000, 0, 1, 0 },       /* empty request */
		{ 0x140000, 0x20000, 0x140000, 0x800, 0, 0 },   /* arkdata */
		{ 0x000000, 0x20000, 0x000000, 0x800, 0, 0 },   /* S-Loader */
		{ 0x160000, 0x10000, 0x160000, 0x800, 0, 0 },   /* not erase-aligned size */
		{ 0x161000, 0x20000, 0x161000, 0x800, 0, 0 },   /* not erase-aligned start */
		{ 0x160000, 0x0, 0x160000, 0x800, 0, 0 },       /* empty window */
		{ 0x560000, 0x6d00000, 0x7240000, 0x20000, 1, 1 }, /* whole rootfs, last block */
		{ 0x7240000, 0x40000, 0x7240000, 0x800, 0, 0 },    /* into userdata */
		{ 0x7260000, 0x20000, 0x7260000, 0x800, 0, 0 },    /* userdata */
#ifdef BM168_BOOTLOGO_WINDOW	/* only a writer built for the boot picture takes its partition */
		{ 0x7860000, 0x80000, 0x7860000, 0x800, 1, 1 },    /* bootlogo, whole */
		{ 0x7860000, 0x80000, 0x78df800, 0x800, 1, 1 },    /* its last page */
		{ 0x7860000, 0x80000, 0x78e0000, 0x800, 1, 0 },    /* just past it */
		{ 0x7880000, 0x20000, 0x7880000, 0x800, 1, 1 },    /* a block inside it */
		{ 0x7860000, 0xa0000, 0x7860000, 0x800, 0, 0 },    /* into bootanimation */
		{ 0x7840000, 0x40000, 0x7860000, 0x800, 0, 0 },    /* from inside userdata */
		{ 0x78e0000, 0x20000, 0x78e0000, 0x800, 0, 0 },    /* bootanimation */
#else
		{ 0x7860000, 0x80000, 0x7860000, 0x800, 0, 0 },    /* bootlogo: not for this build */
#endif
		{ 0x160000, 0xffffffffffe00000ull, 0x160000, 0x800, 0, 0 }, /* overflow */
		{ 0x160000, 0x20000, 0xffffffffffffff00ull, 0x200, 1, 0 },  /* address overflow */
	};
	unsigned int i, bad = 0;

	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		int v = bm168_window_valid(t[i].s, t[i].z);
		int c = bm168_window_contains(t[i].s, t[i].z, t[i].a, t[i].l);
		if (v != t[i].valid || c != t[i].contains) {
			printf("window case %u failed: valid=%d contains=%d\n", i, v, c);
			bad++;
		}
	}
	printf("window: %u cases, %u failures\n", i, bad);
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	unsigned char oob[BM168_OOB], ff[BM168_PAGE + BM168_OOB];
	size_t size, n;
	unsigned char *buf;

	if (argc == 2 && !strcmp(argv[1], "window"))
		return window_checks();
	if (argc != 3)
		return 2;
	buf = slurp(argv[2], &size);
	if (!strcmp(argv[1], "encode")) {
		if (size % BM168_PAGE)
			return 2;
		for (n = 0; n < size / BM168_PAGE; n++) {
			bm168_page_oob(buf + n * BM168_PAGE, oob);
			printf("%08zx ", n * BM168_PAGE);
			for (unsigned int i = 0; i < BM168_OOB; i++)
				printf("%02x", oob[i]);
			putchar('\n');
		}
		return 0;
	}
	if (!strcmp(argv[1], "capture")) {
		size_t stride = BM168_PAGE + BM168_OOB, erased = 0, match = 0, other = 0;

		if (size % stride)
			return 2;
		memset(ff, 0xff, sizeof(ff));
		for (n = 0; n < size / stride; n++) {
			unsigned char *p = buf + n * stride;
			if (!memcmp(p, ff, stride))
				erased++;
			else if (!bm168_check_page(p, p + BM168_PAGE))
				match++;
			else
				other++;
		}
		printf("%s pages=%zu erased=%zu exact=%zu other=%zu\n", argv[2], size / stride,
		       erased, match, other);
		return 0;
	}
	return 2;
}
