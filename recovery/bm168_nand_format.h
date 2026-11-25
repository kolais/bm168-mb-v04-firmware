/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * BM168 stock normal-partition NAND page format (docs/nand-ecc-format.md).
 *
 * Shared, dependency-free code used by the bounded writer kernel patch and by
 * the user-space nand-restore tool, and tested natively against
 * tools/nand_ecc.py and the saved captures. Keep this file byte-identical to
 * the copy added by recovery/patches-writer/0007-bm168-bounded-writer.patch;
 * the recovery build checks that.
 *
 * BCH over GF(2^14), primitive polynomial 0x4443, t = 7 (98 parity bits).
 * Data bits enter least-significant bit first, byte 0 first. The remainder
 * coefficient of x^(97-k) is stored in byte k/8, bit k%8; the final six bits
 * are padding. Parity is XORed with a mask that makes all-0xff data produce
 * all-0xff parity. OOB = ff ff ff | parity(step 0) | parity(step 1) | ff x 35.
 */
#ifndef BM168_NAND_FORMAT_H
#define BM168_NAND_FORMAT_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <string.h>
#endif

#define BM168_PAGE 2048u
#define BM168_OOB 64u
#define BM168_STEP 1024u
#define BM168_PARITY 13u
#define BM168_ERASE 131072u
#define BM168_CHIP 134217728u
/* Only kernel and rootfs may ever be inside a write window. */
#define BM168_WINDOW_MIN 0x00160000u
#define BM168_WINDOW_MAX 0x07260000u

/* Generator polynomial without its x^98 term, 32-bit words, least significant first. */
static const uint32_t bm168_bch_gen[4] = {
	0x68459229u, 0x810480d0u, 0x4ec48cf8u, 0x00000003u,
};
static const uint8_t bm168_bch_mask[BM168_PARITY] = {
	0xcc, 0x8a, 0x7a, 0xe8, 0xf6, 0x2c, 0x31, 0x52, 0x9a, 0xa2, 0xd2, 0x83, 0xff,
};

static inline void bm168_bch_parity(const uint8_t *data, uint8_t *out)
{
	uint32_t r[4] = { 0, 0, 0, 0 };
	unsigned int i, j, k;

	for (i = 0; i < BM168_STEP; i++) {
		for (j = 0; j < 8; j++) {
			uint32_t fb = ((data[i] >> j) & 1u) ^ ((r[3] >> 1) & 1u);

			r[3] = ((r[3] << 1) | (r[2] >> 31)) & 3u;
			r[2] = (r[2] << 1) | (r[1] >> 31);
			r[1] = (r[1] << 1) | (r[0] >> 31);
			r[0] <<= 1;
			if (fb) {
				r[0] ^= bm168_bch_gen[0];
				r[1] ^= bm168_bch_gen[1];
				r[2] ^= bm168_bch_gen[2];
				r[3] ^= bm168_bch_gen[3];
			}
		}
	}
	memset(out, 0, BM168_PARITY);
	for (k = 0; k < 98; k++) {
		unsigned int bit = 97 - k;

		if ((r[bit >> 5] >> (bit & 31)) & 1u)
			out[k >> 3] |= (uint8_t)(1u << (k & 7));
	}
	for (k = 0; k < BM168_PARITY; k++)
		out[k] ^= bm168_bch_mask[k];
}

/* Complete stock OOB for one 2,048-byte page. */
static inline void bm168_page_oob(const uint8_t *page, uint8_t *oob)
{
	memset(oob, 0xff, BM168_OOB);
	bm168_bch_parity(page, oob + 3);
	bm168_bch_parity(page + BM168_STEP, oob + 3 + BM168_PARITY);
}

/* 0 when `oob` is exactly the stock OOB for `page`; never touches hardware. */
static inline int bm168_check_page(const uint8_t *page, const uint8_t *oob)
{
	uint8_t expect[BM168_OOB];

	bm168_page_oob(page, expect);
	return memcmp(expect, oob, BM168_OOB) ? -1 : 0;
}

/* The boot picture's partition (U-Boot reads a JPEG from it): a window only
 * for a writer built for it, with BM168_BOOTLOGO_WINDOW. Every other build
 * refuses it as it refuses everything outside kernel/rootfs. */
#define BM168_BOOTLOGO_START 0x07860000u
#define BM168_BOOTLOGO_SIZE 0x00080000u

/* A window is erase-aligned, non-empty and confined to kernel/rootfs. */
static inline int bm168_window_valid(uint64_t start, uint64_t size)
{
#ifdef BM168_BOOTLOGO_WINDOW
	/* Added before the rule and not woven into it: without the macro the
	 * function is, token for token, the one the installed kernel was
	 * built from, so that kernel still builds to the same bytes. */
	if (size && !(start & (BM168_ERASE - 1)) && !(size & (BM168_ERASE - 1)) &&
	    start >= BM168_BOOTLOGO_START && start - BM168_BOOTLOGO_START < BM168_BOOTLOGO_SIZE &&
	    size <= BM168_BOOTLOGO_SIZE - (start - BM168_BOOTLOGO_START))
		return 1;
#endif
	/* Masks, not '%': 64-bit division is unavailable in 32-bit ARM kernels. */
	return size && !(start & (BM168_ERASE - 1)) && !(size & (BM168_ERASE - 1)) &&
	       start >= BM168_WINDOW_MIN && start <= BM168_WINDOW_MAX &&
	       size <= BM168_WINDOW_MAX - start;
}

/* [addr, addr + len) lies entirely inside a valid window. */
static inline int bm168_window_contains(uint64_t start, uint64_t size,
					uint64_t addr, uint64_t len)
{
	return bm168_window_valid(start, size) && len && addr >= start &&
	       addr - start <= size && len <= size - (addr - start);
}

#endif
