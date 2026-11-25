/* SPDX-License-Identifier: MIT
 * Plan A video decoder probe, stage 2: STOCK libmfc.so (the library CarPlay's
 * libScreenStream and the Android Auto sink use) on the 4.19 RAM kernel.
 *
 *   probe-mfc DIR [MEMALLOC_NODE] [show]   (default /tmp/dev/memalloc)
 *
 * With "show", frames go to the screen through STOCK libarkcmn.so the way
 * libScreenStream does: open("/dev/fb4") (VIDEO2), arkapi_init_fb_display(fd,
 * w, h, x, y, screen_w, screen_h, 0, 0, 0, 0, 0x11 (Y_UV420), 0, 0), then per
 * frame arkapi_set_fb_addr(fd, y, y + align16(w) * align16(h), 0, 0) and
 * arkapi_show_fb(fd, 1). The test stream is 720x1280 portrait, so a 720x480
 * window of the top of each frame is shown centred on the 1280x480 panel at
 * about 30 frames per second; the last frame stays 20 seconds.
 *
 * Calls libmfc the way libScreenStream's PushFrame does (disassembly):
 *   MFCLock(); MFCDecode(&stream, &out, first_call, 104) (H.264) per
 *   access unit; ret 1 = consumed, frames in out; MFCRelease(104); MFCUnlock().
 * The stream buffer is 1 MiB from the allocator node (ioctl 0xc0046b01,
 * {busAddress, size}), mapped through /dev/mem with O_RDWR|O_SYNC like
 * libScreenStream. Stock libmfc itself needs /tmp/dev/hx170 and
 * /tmp/dev/memalloc.
 *
 * For every output frame it prints "F n y c v": FNV-1a of luma and NV12
 * chroma read through the probe's own uncached /dev/mem (O_SYNC) mapping of
 * the frame's bus address (what the hardware wrote to RAM), and the luma
 * checksum through libmfc's own virtual pointer (what a CPU reader of
 * libmfc's mapping sees). Compare with reference-fnv.txt.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define IN_SIZE (1024 * 1024)
#define MFC_H264 104
#define ALLOC_GET 0xc0046b01
#define ALLOC_FREE 0x40046b02

/* Layouts recovered from stock libmfc MFCH264Decode and its callers. */
typedef struct { uint32_t *virt; uint32_t bus; uint32_t size; } StockLinear;
typedef struct { uint32_t yBus; uint32_t w1; uint32_t w2; uint8_t *virt; uint32_t w4; } StockFrame;
typedef struct { int num; uint32_t width; uint32_t height; StockFrame buf[4]; uint32_t spare[32]; } StockOut;

int MFCLock(void);
int MFCUnlock(void);
int MFCDecode(StockLinear *in, StockOut *out, int init, int type);
int MFCRelease(int type);
int arkapi_init_fb_display(int fd, unsigned int w, unsigned int h, unsigned int x, unsigned int y,
			   unsigned int screen_w, unsigned int screen_h, unsigned int a, unsigned int b,
			   unsigned int c, unsigned int d, unsigned int format, unsigned int e, unsigned int f);
int arkapi_set_fb_addr(int fd, unsigned int y, unsigned int uv, unsigned int cr, unsigned int wait);
int arkapi_show_fb(int fd, int show);

#define SCREEN_W 1280
#define SCREEN_H 480

static uint32_t fnv(const volatile uint8_t *p, size_t n)
{
	uint32_t h = 2166136261u;

	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
	(void)ctx;
	printf("FAULT signal %d at %p\n", sig, si->si_addr);
	_exit(128 + sig);
}

int main(int argc, char **argv)
{
	const char *node = argc > 2 && strcmp(argv[2], "show") ? argv[2] : "/tmp/dev/memalloc";
	int show = !strcmp(argv[argc - 1], "show"), fb = -1, shown = 0;
	uint32_t req[2] = { 0, IN_SIZE };
	int afd, mfd, pieces = 0, frames = 0, fails = 0, ret;
	uint8_t *in_virt;
	StockLinear in;
	StockOut out;
	char path[512];
	struct sigaction sa;

	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	if (argc < 2)
		return fprintf(stderr, "usage: probe-mfc DIR [MEMALLOC_NODE]\n"), 2;
	afd = open(node, O_RDWR | O_SYNC);
	mfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (afd < 0 || mfd < 0 || ioctl(afd, ALLOC_GET, req) || !req[0])
		return printf("RESULT FAIL stream buffer from %s\n", node), 1;
	in_virt = mmap(0, IN_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, req[0]);
	if (in_virt == MAP_FAILED)
		return printf("RESULT FAIL stream mapping\n"), 1;
	printf("allocator %s, stream buffer phys 0x%08x\n", node, req[0]);
	{
		volatile const uint32_t *sys = mmap(0, 4096, PROT_READ, MAP_SHARED, mfd, 0xe4900000);

		if (sys != MAP_FAILED)
			printf("mfcclk register 0x%08x\n", sys[0x64 / 4]);
	}
	if (show) {
		fb = open("/dev/fb4", O_RDWR | O_SYNC);
		if (fb < 0)
			return printf("RESULT FAIL open /dev/fb4\n"), 1;
	}
	MFCLock();
	for (;; pieces++) {
		FILE *f;
		size_t n;
		int i;

		snprintf(path, sizeof(path), "%s/%03d.h264", argv[1], pieces);
		f = fopen(path, "rb");
		if (!f)
			break;
		n = fread(in_virt, 1, IN_SIZE, f);
		fclose(f);
		if (!n || n == IN_SIZE)
			return printf("RESULT FAIL piece %d size %zu\n", pieces, n), 1;
		in.virt = (uint32_t *)in_virt;
		in.bus = req[0];
		in.size = (uint32_t)n;
		memset(&out, 0, sizeof(out));
		ret = MFCDecode(&in, &out, pieces == 0, MFC_H264);
		if (ret != 1) {
			if (++fails <= 5)
				printf("piece %d: MFCDecode ret %d, frames %d\n", pieces, ret, out.num);
		}
		for (i = 0; i < out.num && i < 4; i++) {
			uint32_t w = out.width, h = out.height;
			size_t ys = (size_t)((w + 15) & ~15u) * ((h + 15) & ~15u);
			size_t len = (ys * 3 / 2 + 4095) & ~4095ul;
			volatile uint8_t *phys;
			uint32_t y = 0, c = 0, v = 0;

			if (!frames)
				printf("first frame: piece %d, %ux%u, Y bus 0x%08x, libmfc virt %p\n",
				       pieces, w, h, out.buf[i].yBus, (void *)out.buf[i].virt);
			phys = mmap(0, len, PROT_READ, MAP_SHARED, mfd, out.buf[i].yBus);
			if (phys != MAP_FAILED) {
				y = fnv(phys, (size_t)w * h);
				c = fnv(phys + ys, (size_t)w * h / 2);
				munmap((void *)phys, len);
			}
			if (out.buf[i].virt)
				v = fnv(out.buf[i].virt, (size_t)w * h);
			printf("F %d %08x %08x %08x\n", frames, y, c, v);
			if (show) {
				unsigned int ww = w < SCREEN_W ? w : SCREEN_W, wh = h < SCREEN_H ? h : SCREEN_H;

				if (!shown) {
					ret = arkapi_init_fb_display(fb, ww, wh, (SCREEN_W - ww) / 2, (SCREEN_H - wh) / 2,
								     SCREEN_W, SCREEN_H, 0, 0, 0, 0, 0x11, 0, 0);
					printf("arkapi_init_fb_display(%u x %u at %u,%u) ret %d\n", ww, wh,
					       (SCREEN_W - ww) / 2, (SCREEN_H - wh) / 2, ret);
				}
				ret = arkapi_set_fb_addr(fb, out.buf[i].yBus, out.buf[i].yBus + (uint32_t)ys, 0, 0);
				if (!shown || ret == -1)
					printf("arkapi_set_fb_addr ret %d\n", ret);
				if (!shown) {
					printf("arkapi_show_fb ret %d\n", arkapi_show_fb(fb, 1));
					shown = 1;
				}
				usleep(33000);
			}
			frames++;
		}
	}
	if (shown) {
		printf("holding last frame 20 s\n");
		sleep(20);
		arkapi_show_fb(fb, 0);
	}
	if (fb >= 0)
		close(fb);
	MFCRelease(MFC_H264);
	MFCUnlock();
	munmap(in_virt, IN_SIZE);
	ioctl(afd, ALLOC_FREE, &req[0]);
	printf("pieces %d, frames %d, MFCDecode non-1 returns %d\n", pieces, frames, fails);
	if (!frames)
		return printf("RESULT FAIL no frames\n"), 1;
	printf("RESULT PASS %d frames\n", frames);
	return 0;
}
