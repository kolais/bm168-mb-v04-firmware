/* SPDX-License-Identifier: MIT
 * GPU compositing spike: can the GC300 2D engine blend and move whole
 * screens into the GUI layer fast enough for the launcher's transitions?
 *
 * Linked against the stock libGAL.so (Vivante 5.0.11) like probe-gal, run in
 * the stock chroot with galcore loaded and the launcher stopped (it draws
 * into the same layer). Two full-screen pictures are painted once by the CPU
 * into GPU surfaces; each test then composes them into /dev/fb0 (the GUI
 * layer, 1280 x 480 ARGB8888) once per frame, the way a transition would,
 * waits for the GPU each time, and reports frames per second. At the middle
 * frame one pixel is read back, so the blend arithmetic (and what it leaves
 * in the layer's alpha, which the display controller uses against the video)
 * is known and not assumed.
 *
 *   probe-blend [FRAMES] [vsync] [keep]
 *
 * FRAMES per test (default 60); "vsync" waits for the panel's vertical sync
 * after each frame, as the launcher would; "keep" leaves the last picture on
 * the layer instead of putting back what was there.
 *
 * Nothing is written but the GUI layer's memory. Exit 0 with RESULT PASS when
 * every call succeeded; the numbers are the result.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "HAL/gc_hal.h"
#include "HAL/gc_hal_raster.h"

#define W 1280
#define H 480
#define GROUND 0xFF0B0B0Cu

static gcoOS os;
static gcoHAL hal;
static gco2D e2d;
static int fb_fd = -1, frames = 60, vsync, failed;
static uint32_t *fb, fb_addr, fb_stride;

struct pic {
	gcoSURF surf;
	uint32_t addr, *px;
	int stride;
};
static struct pic a, b;

#define TRY(what, call) do { \
	gceSTATUS s_ = (call); \
	if (gcmIS_ERROR(s_)) { \
		printf("  %-34s status=%d\n", what, (int)s_); \
		failed = 1; \
	} \
} while (0)

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void fill(struct pic *p, int x, int y, int w, int h, uint32_t argb)
{
	for (int r = y; r < y + h; r++)
		for (int c = x; c < x + w; c++)
			*(uint32_t *)((uint8_t *)p->px + r * p->stride + c * 4) = argb;
}

static int make(struct pic *p)
{
	gctUINT32 addr[3] = { 0 };
	gctPOINTER mem[3] = { 0 };
	gctUINT aw, ah;
	gctINT stride;

	if (gcmIS_ERROR(gcoSURF_Construct(hal, W, H, 1, gcvSURF_BITMAP, gcvSURF_A8R8G8B8, gcvPOOL_DEFAULT, &p->surf)) ||
	    gcmIS_ERROR(gcoSURF_Lock(p->surf, addr, mem)) ||
	    gcmIS_ERROR(gcoSURF_GetAlignedSize(p->surf, &aw, &ah, &stride)))
		return -1;
	p->addr = addr[0];
	p->px = mem[0];
	p->stride = stride;
	printf("surface %ux%u stride %d at GPU address 0x%08x\n", aw, ah, stride, (unsigned)p->addr);
	return 0;
}

/* Picture A: four tiles, as Home. Picture B: six rows, as a list. Opaque. */
static void paint(void)
{
	fill(&a, 0, 0, W, H, GROUND);
	for (int i = 0; i < 4; i++) {
		fill(&a, 144 + i * 280, 64, 264, 320, 0xFF1C1C1Eu);
		fill(&a, 144 + i * 280, 64, 264, 2, 0xFF3A3A3Cu);
		fill(&a, 144 + i * 280 + 104, 150, 56, 56, 0xFFE8E8EAu);
	}
	fill(&b, 0, 0, W, H, GROUND);
	for (int i = 0; i < 6; i++) {
		fill(&b, 32, 64 + i * 56, 1216, 54, i == 2 ? 0xFF2C2C2Eu : 0xFF161618u);
		fill(&b, 56, 64 + i * 56 + 20, 240 + 40 * i, 14, 0xFFE8E8EAu);
	}
	fill(&b, 32, 64 + 2 * 56, 1216, 3, 0xFFF5A623u);	/* the ring's colour, on the focused row */
}

/* One rectangle of a picture onto the layer at (dx, dy). */
static void blit(const struct pic *p, int sx, int sy, int w, int h, int dx, int dy)
{
	gcsRECT src = { sx, sy, sx + w, sy + h }, dst = { dx, dy, dx + w, dy + h }, all = { 0, 0, W, H };

	TRY("SetColorSourceEx", gco2D_SetColorSourceEx(e2d, p->addr, p->stride, gcvSURF_A8R8G8B8, gcvSURF_0_DEGREE,
							W, H, gcvFALSE, gcvSURF_OPAQUE, 0));
	TRY("SetTargetEx", gco2D_SetTargetEx(e2d, fb_addr, fb_stride, gcvSURF_0_DEGREE, W, H));
	TRY("SetClipping", gco2D_SetClipping(e2d, &all));
	TRY("BatchBlit", gco2D_BatchBlit(e2d, 1, &src, &dst, 0xCC, 0xCC, gcvSURF_A8R8G8B8));
}

/* Source over destination with one alpha for the whole source. Method 1:
 * the global alpha stands in for the source's own. Method 2 (PE 2.0): the
 * source is multiplied by the global alpha first, so opaque over opaque
 * should stay opaque. Which of them does what is what the read-back shows. */
static void blend_on(int method, int alpha)
{
	if (method == 1) {
		TRY("EnableAlphaBlend", gco2D_EnableAlphaBlend(e2d, (gctUINT8)alpha, 0xFF,
				gcvSURF_PIXEL_ALPHA_STRAIGHT, gcvSURF_PIXEL_ALPHA_STRAIGHT,
				gcvSURF_GLOBAL_ALPHA_ON, gcvSURF_GLOBAL_ALPHA_OFF,
				gcvSURF_BLEND_STRAIGHT, gcvSURF_BLEND_INVERSED,
				gcvSURF_COLOR_STRAIGHT, gcvSURF_COLOR_STRAIGHT));
	} else {
		TRY("SetSourceGlobalColorAdvanced", gco2D_SetSourceGlobalColorAdvanced(e2d, (uint32_t)alpha << 24));
		TRY("SetPixelMultiplyModeAdvanced", gco2D_SetPixelMultiplyModeAdvanced(e2d,
				gcv2D_COLOR_MULTIPLY_DISABLE, gcv2D_COLOR_MULTIPLY_DISABLE,
				gcv2D_GLOBAL_COLOR_MULTIPLY_ALPHA, gcv2D_COLOR_MULTIPLY_DISABLE));
		TRY("EnableAlphaBlendAdvanced", gco2D_EnableAlphaBlendAdvanced(e2d,
				gcvSURF_PIXEL_ALPHA_STRAIGHT, gcvSURF_PIXEL_ALPHA_STRAIGHT,
				gcvSURF_GLOBAL_ALPHA_SCALE, gcvSURF_GLOBAL_ALPHA_OFF,
				gcvSURF_BLEND_ONE, gcvSURF_BLEND_INVERSED));
	}
}

static void blend_off(void)
{
	TRY("DisableAlphaBlend", gco2D_DisableAlphaBlend(e2d));
	TRY("SetPixelMultiplyModeAdvanced(off)", gco2D_SetPixelMultiplyModeAdvanced(e2d,
			gcv2D_COLOR_MULTIPLY_DISABLE, gcv2D_COLOR_MULTIPLY_DISABLE,
			gcv2D_GLOBAL_COLOR_MULTIPLY_DISABLE, gcv2D_COLOR_MULTIPLY_DISABLE));
}

static void show(void)
{
	TRY("Flush", gco2D_Flush(e2d));
	TRY("Commit", gcoHAL_Commit(hal, gcvTRUE));	/* waits for the GPU */
	if (vsync) {
		int crtc = 0;

		ioctl(fb_fd, FBIO_WAITFORVSYNC, &crtc);
	}
}

/* The frames of each test; t runs 0..255 over a test. */
static void frame_cpu(int t)
{
	(void)t;
	for (int r = 0; r < H; r++)
		memcpy((uint8_t *)fb + r * fb_stride, (uint8_t *)a.px + r * a.stride, W * 4);
}

static void frame_copy(int t)
{
	blit(t & 1 ? &b : &a, 0, 0, W, H, 0, 0);
	show();
}

static void frame_fade1(int t)
{
	blit(&a, 0, 0, W, H, 0, 0);
	blend_on(1, t);
	blit(&b, 0, 0, W, H, 0, 0);
	blend_off();
	show();
}

static void frame_fade2(int t)
{
	blit(&a, 0, 0, W, H, 0, 0);
	blend_on(2, t);
	blit(&b, 0, 0, W, H, 0, 0);
	blend_off();
	show();
}

/* A leaves to the left as B comes in from the right: two copies, no blend. */
static void frame_slide(int t)
{
	int x = t * W / 256;

	if (x < W)
		blit(&a, x, 0, W - x, H, 0, 0);
	if (x > 0)
		blit(&b, 0, 0, x, H, W - x, 0);
	show();
}

/* The design's screen change: a cross-fade with a 24 px slide. */
static void frame_fade_slide(int t)
{
	int dx = 24 - t * 24 / 255;

	blit(&a, 0, 0, W, H, 0, 0);
	blend_on(1, t);
	blit(&b, 0, 0, W - dx, H, dx, 0);
	blend_off();
	show();
}

/* Into CarPlay: the content fades to the ground colour. */
static void frame_to_ground(int t)
{
	gcsRECT all = { 0, 0, W, H };

	TRY("SetTargetEx", gco2D_SetTargetEx(e2d, fb_addr, fb_stride, gcvSURF_0_DEGREE, W, H));
	TRY("SetClipping", gco2D_SetClipping(e2d, &all));
	TRY("Clear", gco2D_Clear(e2d, 1, &all, GROUND, 0xCC, 0xCC, gcvSURF_A8R8G8B8));
	blend_on(1, 255 - t);
	blit(&a, 0, 0, W, H, 0, 0);
	blend_off();
	show();
}

/* Where the two pictures differ: a tile's bright mark in A (E8E8EA), a dark
 * row in B (161618). Half-way, a true blend is about 7F7F81 with alpha FF. */
static uint32_t sample(void)
{
	return *(uint32_t *)((uint8_t *)fb + 256 * fb_stride + (144 + 104 + 28) * 4);
}

static void run(const char *name, void (*frame)(int))
{
	double t0 = now_ms(), t1;
	uint32_t mid = 0;

	for (int f = 0; f < frames; f++) {
		frame(f * 255 / (frames - 1));
		if (f == frames / 2)
			mid = sample();
	}
	t1 = now_ms();
	printf("%-12s %3d frames %7.1f ms  %6.2f ms/frame  %5.1f fps   middle pixel %08x, last %08x\n", name, frames,
	       t1 - t0, (t1 - t0) / frames, frames * 1000.0 / (t1 - t0), (unsigned)mid, (unsigned)sample());
}

int main(int argc, char **argv)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint32_t *saved;
	int keep = 0, pe20, same = 1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "vsync"))
			vsync = 1;
		else if (!strcmp(argv[i], "keep"))
			keep = 1;
		else if (atoi(argv[i]) >= 2)
			frames = atoi(argv[i]);
	}
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix)) {
		printf("RESULT FAIL no /dev/fb0\n");
		return 1;
	}
	printf("fb0 %ux%u %u bpp, line %u bytes, memory at 0x%08lx (%u bytes)\n", var.xres, var.yres,
	       var.bits_per_pixel, fix.line_length, fix.smem_start, fix.smem_len);
	if (var.xres != W || var.yres != H || var.bits_per_pixel != 32) {
		printf("RESULT FAIL the layer is not 1280x480 at 32 bpp\n");
		return 1;
	}
	fb = mmap(0, fix.line_length * H, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	saved = malloc(fix.line_length * H);
	if (fb == MAP_FAILED || !saved) {
		printf("RESULT FAIL the layer cannot be mapped\n");
		return 1;
	}
	fb_addr = fix.smem_start;
	fb_stride = fix.line_length;
	memcpy(saved, fb, fix.line_length * H);

	if (gcmIS_ERROR(gcoOS_Construct(gcvNULL, &os)) || gcmIS_ERROR(gcoHAL_ConstructEx(gcvNULL, os, &hal)) ||
	    gcmIS_ERROR(gcoHAL_Get2DEngine(hal, &e2d))) {
		printf("RESULT FAIL the GPU library did not start\n");
		return 1;
	}
	pe20 = gcoHAL_IsFeatureAvailable(hal, gcvFEATURE_2DPE20) == gcvSTATUS_TRUE;
	printf("features: PE2.0 %d, multi-source blit %d, full DirectFB %d\n", pe20,
	       gcoHAL_IsFeatureAvailable(hal, gcvFEATURE_2D_MULTI_SOURCE_BLT) == gcvSTATUS_TRUE,
	       gcoHAL_IsFeatureAvailable(hal, gcvFEATURE_FULL_DIRECTFB) == gcvSTATUS_TRUE);
	if (make(&a) || make(&b)) {
		printf("RESULT FAIL no memory for two full-screen surfaces\n");
		return 1;
	}
	paint();
	printf("%d frames per test%s; the GPU is waited for after each frame\n", frames, vsync ? ", then the vertical sync" : "");

	run("cpu-copy", frame_cpu);
	run("gpu-copy", frame_copy);
	/* The copy is exact: the layer now holds the picture of the last frame. */
	blit(&a, 0, 0, W, H, 0, 0);
	show();
	for (int r = 0; r < H && same; r++)
		same = !memcmp((uint8_t *)fb + r * fb_stride, (uint8_t *)a.px + r * a.stride, W * 4);
	printf("gpu-copy reads back identical to the picture: %s\n", same ? "yes" : "NO");
	run("fade-1", frame_fade1);
	if (pe20)
		run("fade-2", frame_fade2);
	run("slide", frame_slide);
	run("fade+slide", frame_fade_slide);
	run("to-ground", frame_to_ground);

	if (!keep) {
		memcpy(fb, saved, fix.line_length * H);
		printf("the layer is as it was\n");
	}
	gcoSURF_Unlock(a.surf, a.px);
	gcoSURF_Unlock(b.surf, b.px);
	gcoSURF_Destroy(a.surf);
	gcoSURF_Destroy(b.surf);
	gcoHAL_DestroyEx(hal);
	gcoOS_Destroy(os);
	printf(failed || !same ? "RESULT FAIL a call failed or the copy was not exact\n" : "RESULT PASS\n");
	return failed || !same;
}
