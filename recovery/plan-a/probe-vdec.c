/* SPDX-License-Identifier: MIT
 * Plan A video decoder probe, stage 1 (vendor-consistent set): SDK hx170dec
 * and memalloc drivers on the 4.19 RAM kernel, SDK libmfc + libarkapi in
 * userspace, the SDK's H.264 test pieces as input.
 *
 *   probe-vdec DIR [show|showfirst] [rgb] [clear]
 *
 * "rgb" displays like the SDK's ARK1668 video path: the GPU 2D engine
 * (libarkapi arkapi_2d_process) converts the NV12 frame to RGB565, aspect
 * ratio kept and centred, and the video layer shows that buffer as R5G6B5.
 * Without "rgb" the NV12 frame goes to the layer directly as Y_UV420, which
 * the ARK1668 does not display correctly (kept for comparison). The mean
 * R, G, B of the converted picture are printed as "RGB mean r g b".
 *
 * "clear" zeroes the rest of the input buffer after each piece, so the
 * hardware cannot read stale bytes from an earlier, longer piece. Every
 * frame's luma and chroma FNV-1a checksums are printed as "F n y c" lines
 * for comparison with a host reference decode.
 *
 * Feeds DIR/000.h264, 001.h264, ... in order to mfc_decode(), counts output
 * frames and their size, and prints an FNV-1a checksum of the luma plane of
 * the first and last decoded frames (repeat runs must agree). With "show",
 * the last frame is put on the video layer (Y/UV 4:2:0) for a few seconds.
 * Exit 0 only if at least one frame decoded and all decoded frames agree on
 * their dimensions.
 */
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "ark_api.h"
#include "mfcapi.h"

#define IN_SIZE (1024 * 1024)

static uint32_t fnv(const unsigned char *p, size_t n)
{
	uint32_t h = 2166136261u;

	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}

/* Report where an illegal instruction or crash happened: address,
 * instruction word and the mapping (library + offset) it falls in. */
static void on_fault(int sig, siginfo_t *si, void *ctx)
{
	char line[256];
	unsigned long a = (unsigned long)si->si_addr, lo, hi, off;
	FILE *m = fopen("/proc/self/maps", "r");

	(void)ctx;
	printf("FAULT signal %d at 0x%08lx", sig, a);
	if (sig == SIGILL && a)
		printf(" insn 0x%08x / thumb 0x%04x", *(const uint32_t *)(a & ~3UL), *(const uint16_t *)(a & ~1UL));
	printf("\n");
	while (m && fgets(line, sizeof(line), m)) {
		char path[200] = "";

		if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %199s", &lo, &hi, &off, path) >= 3 && a >= lo && a < hi)
			printf("FAULT in %s offset 0x%lx\n", path, a - lo + off);
	}
	_exit(128 + sig);
}

/* Input buffer straight from the SDK allocator (the prebuilt libarkapi's
 * reqbuf crashed on hardware; this path is checked by diag-memalloc). */
typedef struct { unsigned busAddress; unsigned size; } AllocParams;
#define ALLOC_GET _IOWR('k', 1, AllocParams *)

int main(int argc, char **argv)
{
	AllocParams buf = { 0, IN_SIZE };
	unsigned char *in_virt;
	int afd, mfd;
	MFCHandle *mfc;
	OutFrameBuffer out;
	DWLLinearMem_t strm;
	char path[512];
	int pieces = 0, frames = 0, fails = 0, w = 0, h = 0, mismatch = 0, i, a;
	int show = 0, clear = 0, rgb = 0;
	size_t prev = IN_SIZE;
	uint32_t first = 0, last = 0;
	FrameBuffer keep = { 0 };

	for (a = 2; a < argc; a++) {
		if (!strcmp(argv[a], "show"))
			show = 1;
		else if (!strcmp(argv[a], "showfirst"))
			show = 2;
		else if (!strcmp(argv[a], "clear"))
			clear = 1;
		else if (!strcmp(argv[a], "rgb"))
			rgb = 1;
	}

	setvbuf(stdout, NULL, _IONBF, 0);
	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = on_fault;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGILL, &sa, NULL);
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
	}
	if (argc < 2)
		return fprintf(stderr, "usage: probe-vdec DIR [show]\n"), 2;
	afd = open("/dev/memalloc", O_RDWR | O_SYNC);
	mfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (afd < 0 || mfd < 0 || ioctl(afd, ALLOC_GET, &buf))
		return printf("RESULT FAIL input allocation (errno %d)\n", errno), 1;
	in_virt = mmap(0, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, buf.busAddress);
	if (in_virt == MAP_FAILED)
		return printf("RESULT FAIL input mapping (errno %d)\n", errno), 1;
	printf("input buffer phys 0x%08x size %u\n", buf.busAddress, buf.size);
	{
		/* Decoder clock (SDK/stock: 0x00210016 = CPU PLL / 2). */
		volatile const uint32_t *sys = mmap(0, 4096, PROT_READ, MAP_SHARED, mfd, 0xe4900000);

		if (sys != MAP_FAILED) {
			printf("mfcclk register 0x%08x\n", sys[0x64 / 4]);
			munmap((void *)sys, 4096);
		}
	}
	mfc = mfc_init(RAW_STRM_TYPE_H264);
	if (!mfc)
		return printf("RESULT FAIL mfc_init\n"), 1;

	for (;; pieces++) {
		FILE *f;
		size_t n;
		int ret;

		snprintf(path, sizeof(path), "%s/%03d.h264", argv[1], pieces);
		f = fopen(path, "rb");
		if (!f)
			break;
		n = fread(in_virt, 1, IN_SIZE, f);
		fclose(f);
		if (!n || n == IN_SIZE)
			return printf("RESULT FAIL piece %d size %zu\n", pieces, n), 1;
		if (clear && prev > n)
			memset(in_virt + n, 0, prev - n);
		prev = n;
		strm.virtualAddress = (u32 *)in_virt;
		strm.busAddress = buf.busAddress;
		strm.size = (u32)n;
		memset(&out, 0, sizeof(out));
		ret = mfc_decode(mfc, &strm, &out);
		/* The prebuilt SDK libmfc returns 0 on success, contrary to its header's
		 * MFCDEC_RET_OK (1); the SDK's own caller (libarkapi ark_video.c) checks
		 * `if (ret)` for failure. Output frames are in out.num either way. */
		if (ret != 0) {
			fails++;
			if (fails <= 5)
				printf("piece %d: mfc_decode ret %d\n", pieces, ret);
			continue;
		}
		for (i = 0; i < out.num; i++) {
			const FrameBuffer *fb = &out.buffer[i];
			size_t ysize = (size_t)out.frameWidth * out.frameHeight;
			const unsigned char *cvirt = fb->pcbcrVirAddress;
			uint32_t sum = fnv(fb->pyVirAddress, ysize);
			uint32_t csum;

			/* The SDK libmfc may leave the chroma virtual address unset; the
			 * chroma plane then follows the luma plane in the same buffer. */
			if (!frames)
				printf("addresses: y bus 0x%08x virt %p, cbcr bus 0x%08x virt %p\n",
				       fb->yBusAddress, fb->pyVirAddress, fb->cbcrBusAddress, fb->pcbcrVirAddress);
			/* SDK libmfc reports neither chroma address (both 0); like the SDK's
			 * own caller, take the NV12 chroma plane right after the luma. */
			if (!cvirt)
				cvirt = (const unsigned char *)fb->pyVirAddress + ysize;
			csum = cvirt ? fnv(cvirt, ysize / 2) : 0;

			printf("F %d %08x %08x\n", frames, sum, csum);
			if (!frames) {
				w = out.frameWidth;
				h = out.frameHeight;
				first = sum;
				printf("first frame: piece %d, %dx%d (coded %dx%d), Y 0x%08x, luma fnv 0x%08x\n",
				       pieces, w, h, out.codedWidth, out.codedHeight, fb->yBusAddress, sum);
			} else if (out.frameWidth != w || out.frameHeight != h) {
				mismatch++;
			}
			last = sum;
			if (show != 2 || !frames)
				keep = *fb;
			frames++;
		}
	}
	printf("pieces %d, decoded frames %d, decode failures %d, size mismatches %d\n",
	       pieces, frames, fails, mismatch);
	printf("luma fnv first 0x%08x last 0x%08x\n", first, last);

	if (frames && show && rgb) {
		struct ark_screen screen;
		AllocParams dst = { 0, 0 };
		ark2d_handle *g2d;
		ark2d_cfg cfg;
		void *layer;
		int ow, oh, x, y, ret;

		memset(&screen, 0, sizeof(screen));
		arkapi_display_get_screen_info(&screen);
		dst.size = screen.width * screen.height * 2;
		g2d = arkapi_2d_init();
		if (!g2d)
			return printf("RESULT FAIL arkapi_2d_init\n"), 1;
		if (ioctl(afd, ALLOC_GET, &dst))
			return printf("RESULT FAIL RGB buffer allocation (errno %d)\n", errno), 1;
		arkapi_2d_get_config(g2d, &cfg);
		cfg.src_width = w;
		cfg.src_height = h;
		cfg.win_src_x = cfg.win_src_y = 0;
		cfg.win_src_width = w;
		cfg.win_src_height = h;
		/* Fit the frame to the screen here: the prebuilt SDK libarkapi
		 * computes keep_aspect_ratio with UDIV, which the Cortex-A5 lacks
		 * (SIGILL). Width rounded up to 16 pixels for GPU/LCDC strides. */
		if ((unsigned long long)w * screen.height <= (unsigned long long)h * screen.width) {
			oh = screen.height;
			ow = (int)(((unsigned long long)w * screen.height / h + 15) & ~15ULL);
		} else {
			ow = screen.width & ~15;
			oh = (int)(((unsigned long long)h * ow / w) & ~1ULL);
		}
		cfg.dst_width = cfg.win_dst_width = ow;
		cfg.dst_height = cfg.win_dst_height = oh;
		cfg.direction = VERTICAL;
		cfg.keep_aspect_ratio = 0;
		arkapi_2d_set_config(g2d, &cfg);
		ret = arkapi_2d_process(g2d, keep.yBusAddress, dst.busAddress);
		ow = g2d->out_width;
		oh = g2d->out_height;
		printf("2D conversion ret %d: %dx%d NV12 at 0x%08x -> %dx%d RGB565 at 0x%08x\n",
		       ret, w, h, keep.yBusAddress, ow, oh, dst.busAddress);
		if (ret)
			return printf("RESULT FAIL 2D conversion\n"), 1;
		{
			const uint16_t *px = mmap(0, dst.size, PROT_READ, MAP_SHARED, mfd, dst.busAddress);
			unsigned long long r = 0, g = 0, b = 0;

			if (px != MAP_FAILED) {
				for (y = 0; y < oh; y++)
					for (x = 0; x < ow; x++) {
						uint16_t v = px[y * ow + x];

						r += ((v >> 11) & 0x1f) * 255 / 31;
						g += ((v >> 5) & 0x3f) * 255 / 63;
						b += (v & 0x1f) * 255 / 31;
					}
				printf("RGB mean %llu %llu %llu, fnv 0x%08x\n", r / (ow * oh), g / (ow * oh),
				       b / (ow * oh), fnv((const unsigned char *)px, (size_t)ow * oh * 2));
				munmap((void *)px, dst.size);
			}
		}
		layer = arkapi_display_open_layer(VIDEO_LAYER);
		if (!layer)
			return printf("RESULT FAIL video layer unavailable\n"), 1;
		arkapi_display_set_layer_format(layer, ARK_LCDC_FORMAT_R5G6B5);
		arkapi_display_set_layer_pos(layer, (screen.width - ow) / 2, (screen.height - oh) / 2);
		arkapi_display_set_layer_size(layer, ow, oh);
		arkapi_display_set_layer_addr(layer, dst.busAddress, 0, 0);
		arkapi_display_show_layer(layer);
		printf("showing %s frame as RGB565 on the video layer (%dx%d at %d,%d)\n",
		       show == 2 ? "first" : "last", ow, oh, (int)(screen.width - ow) / 2, (int)(screen.height - oh) / 2);
		sleep(20);
		arkapi_display_hide_layer(layer);
		arkapi_display_close_layer(layer);
		arkapi_2d_release(g2d);
	} else if (frames && show) {
		struct ark_screen screen;
		int sw, sh;
		void *layer;

		memset(&screen, 0, sizeof(screen));
		arkapi_display_get_screen_info(&screen);
		sw = w < (int)screen.width ? w : (int)screen.width;
		sh = h < (int)screen.height ? h : (int)screen.height;
		layer = arkapi_display_open_layer(VIDEO_LAYER);
		if (layer) {
			arkapi_display_set_layer_format(layer, ARK_LCDC_FORMAT_Y_UV420);
			arkapi_display_set_layer_pos(layer, 0, 0);
			arkapi_display_set_layer_size(layer, sw, sh);
			arkapi_display_set_layer_addr(layer, keep.yBusAddress,
				keep.cbcrBusAddress ? keep.cbcrBusAddress : keep.yBusAddress + (u32)(w * h), 0);
			arkapi_display_show_layer(layer);
			printf("showing %s frame on the video layer (%dx%d of %dx%d)\n",
			       show == 2 ? "first" : "last", sw, sh, w, h);
			sleep(10);
			arkapi_display_hide_layer(layer);
			arkapi_display_close_layer(layer);
		} else {
			printf("video layer unavailable\n");
		}
	}
	mfc_uninit(mfc);
	munmap(in_virt, buf.size);
	close(afd);
	close(mfd);
	if (!frames || mismatch)
		return printf("RESULT FAIL\n"), 1;
	printf("RESULT PASS %d frames %dx%d\n", frames, w, h);
	return 0;
}
