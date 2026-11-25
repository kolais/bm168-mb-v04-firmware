/* SPDX-License-Identifier: MIT
 * Display-layer spike (docs/plan-a-gpu-display-spike.md): can the display
 * controller itself move and fade whole screens? It has three GUI layers
 * (fb0..fb2 = OSD1..OSD3) over one 8 MiB memory that holds three screens;
 * each layer has an address, a position (signed) and a layer-wide alpha.
 * Nothing is copied per frame: one layer shows the old screen, the other
 * the new one, and only registers change, at the frame interrupt.
 *
 *   probe-push regs                     the layers' registers
 *   probe-push push [FRAMES] [REPS]     the old screen leaves to the left as
 *                                       the new comes from the right, a hold,
 *                                       and back; the screen ends as it was
 *   probe-push back0 [FRAMES] [REPS]    a push back from screen 0, and forward again
 *   probe-push fade [FRAMES] [REPS]     a cross-fade by the layer's alpha
 *   ... up=1                            fb1 is the upper layer (default: fb0)
 *
 * The launcher must not be running. Screen 0 of the memory is what is shown
 * (the launcher's last picture); screens 1 and 2 are painted here.
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

#define W 1280
#define H 480
#define STRIDE (W * 4)
#define SCREEN (STRIDE * H)

/* The driver's interface (drivers/video/fbdev/arkmicro/ark_lcdc_common.h). */
struct ark_disp_scaler { int src_w, src_h, win_x, win_y, win_w, win_h, out_x, out_y, out_w, out_h, cut_top, cut_bottom, cut_left, cut_right; };
struct ark_disp_addr { unsigned int yaddr, cbaddr, craddr, wait_vsync; };
struct ark_disp_reg { unsigned int addr, value; };
struct ark_fb_blend { int alpha_blend_en, per_pix_alpha_blend_en, blend_mode; unsigned char alpha; };
struct ark_disp_atomic {
	int layer, atomic_stat, pos_x, pos_y, width, height, format, yuyv_order, rgb_order;
	struct ark_disp_addr addr;
	struct ark_disp_scaler scaler;
};
#define ATOMIC_POS 1
#define ATOMIC_SIZE 2
#define ATOMIC_ADDR 8
#define ARKFB_SHOW_WINDOW _IO('O', 39)
#define ARKFB_HIDE_WINDOW _IO('O', 40)
#define ARKFB_SET_WINDOW_ADDR _IOW('O', 44, struct ark_disp_addr)
#define ARKFB_SET_WINDOW_ATOMIC _IOW('O', 46, struct ark_disp_atomic)
#define ARKFB_SET_REG_VALUE _IOW('O', 47, struct ark_disp_reg)
#define ARKFB_GET_REG_VALUE _IOR('O', 48, struct ark_disp_reg)
#define ARKFB_SET_BLEND _IOW('O', 41, struct ark_fb_blend)

#define LCDC 0xe0500000u
#define R_CONTROL 0x04
#define R_MODE0 0x60
#define R_MODE1 0x64
#define R_OSD1_CTL 0x74
#define R_OSD2_CTL 0x88

static int fb0, fb1;
static uint32_t *mem;
static unsigned long base;

static unsigned int reg(unsigned int at)
{
	struct ark_disp_reg r = { LCDC + at, 0 };

	if (ioctl(fb0, ARKFB_GET_REG_VALUE, &r))
		perror("get reg");
	return r.value;
}

static void set_reg(unsigned int at, unsigned int value)
{
	struct ark_disp_reg r = { LCDC + at, value };

	if (ioctl(fb0, ARKFB_SET_REG_VALUE, &r))
		perror("set reg");
}

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void vsync(void)
{
	uint32_t crtc = 0;

	ioctl(fb0, FBIO_WAITFORVSYNC, &crtc);
}

/* Which layer is the upper one: 0 = fb0 (OSD1), 1 = fb1 (OSD2). fb0 is the
 * one that shows a screen at rest. */
static int up;
static int fd[2];

/* A layer's address: taken at the next frame interrupt, without waiting. */
static void addr_at_frame(int l, unsigned long addr)
{
	struct ark_disp_addr a = { addr, 0, 0, 0 };

	if (ioctl(fd[l], ARKFB_SET_WINDOW_ADDR, &a))
		perror("addr");
}

/* A layer's place and address: waits for the frame interrupt, then sets. */
static void layer_set(int l, int what, int x, unsigned long addr)
{
	struct ark_disp_atomic a;

	memset(&a, 0, sizeof(a));
	a.layer = l;
	a.atomic_stat = what;
	a.pos_x = x;
	a.width = W;
	a.height = H;
	a.addr.yaddr = addr;
	if (ioctl(fd[l], ARKFB_SET_WINDOW_ATOMIC, &a))
		perror("atomic");
}

/* A screen as a list: a title and rows, in its own colour; opaque. */
static void paint(int screen, uint32_t ground, uint32_t ink)
{
	uint32_t *p = mem + (size_t)screen * W * H;

	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint32_t c = ground;

			if (y >= 16 && y < 44 && x >= 32 && x < 32 + 300 + screen * 60)
				c = ink;	/* the title */
			if (y >= 60 && (y % 60) == 59 && x >= 32 && x < W - 32)
				c = 0xff2a2f38;	/* hairlines */
			if (y >= 60 && (y % 60) >= 20 && (y % 60) < 40 && x >= 56 && x < 56 + 180 + ((y / 60 * 97 + screen * 131) % 260))
				c = ink;	/* a row's words */
			if (y >= 60 && (y % 60) >= 22 && (y % 60) < 38 && x >= W - 96 && x < W - 64)
				c = 0xff8a93a3;	/* its chevron */
			if (x < 4 || x >= W - 4)
				c = screen == 1 ? 0xffff5a00 : 0xff00a0ff;	/* the screen's edges, to see where they meet */
			p[(size_t)y * W + x] = c;
		}
}

static int ease(int f, int n)	/* 0..W, ease-out, in steps of 4 pixels */
{
	double t = (double)f / n, e = 1 - (1 - t) * (1 - t) * (1 - t);
	int x = (int)(e * W + 0.5);

	return f >= n ? W : x & ~3;
}

static void report(const char *name, const double *at, int n)
{
	double worst = 0, sum = 0;
	int late = 0;

	for (int i = 1; i < n; i++) {
		double d = at[i] - at[i - 1];

		sum += d;
		if (d > worst)
			worst = d;
		late += d > 20;
	}
	printf("%-14s %3d frames  %6.2f ms/frame on average, longest %6.2f ms, %d over 20 ms\n", name, n - 1,
	       n > 1 ? sum / (n - 1) : 0, worst, late);
}

/* The screen at `from` leaves, the one at `to` arrives; dir 1: to the left
 * (forward), -1: to the right (back). The lower layer shows `from`, shifted
 * by its address (what wraps into view lies under the upper layer); the
 * upper layer shows `to` and moves by its position. At the end fb0 shows
 * `to` in place and fb1 is off. */
static void push(int from, int to, int dir, int frames)
{
	double at[400];
	unsigned long a_from = base + (unsigned long)from * SCREEN, a_to = base + (unsigned long)to * SCREEN;

	if (up == 0) {	/* fb1 under fb0, with the same picture */
		layer_set(1, ATOMIC_POS | ATOMIC_SIZE | ATOMIC_ADDR, 0, a_from);
		ioctl(fb1, ARKFB_SHOW_WINDOW);
	}
	at[0] = now_ms();
	for (int f = 1; f <= frames; f++) {
		int dx = ease(f, frames);

		addr_at_frame(1 - up, a_from + dir * dx * 4);
		layer_set(up, f == 1 ? ATOMIC_POS | ATOMIC_SIZE | ATOMIC_ADDR : ATOMIC_POS, dir * (W - dx), a_to);
		if (f == 1 && up == 1)
			ioctl(fb1, ARKFB_SHOW_WINDOW);
		at[f] = now_ms();
	}
	if (up == 1) {	/* the same picture under it, then it goes */
		addr_at_frame(0, a_to);
		vsync();
		vsync();
	}
	ioctl(fb1, ARKFB_HIDE_WINDOW);
	report(dir > 0 ? "push forward" : "push back", at, frames + 1);
}

/* A cross-fade by the upper layer's own alpha. */
static void fade(int from, int to, int frames)
{
	double at[400];
	struct ark_fb_blend b = { 1, 0, 0, 0 };
	unsigned long a_from = base + (unsigned long)from * SCREEN, a_to = base + (unsigned long)to * SCREEN;
	unsigned int ctl = reg(up ? R_OSD2_CTL : R_OSD1_CTL), mode0 = reg(R_MODE0), mode1 = reg(R_MODE1);

	if (up == 0) {
		layer_set(1, ATOMIC_POS | ATOMIC_SIZE | ATOMIC_ADDR, 0, a_from);
		ioctl(fb1, ARKFB_SHOW_WINDOW);
		vsync();
		ioctl(fd[0], ARKFB_SET_BLEND, &b);	/* fb0 clear, over the same picture */
		addr_at_frame(0, a_to);
	} else {
		ioctl(fd[1], ARKFB_SET_BLEND, &b);
		layer_set(1, ATOMIC_POS | ATOMIC_SIZE | ATOMIC_ADDR, 0, a_to);
		ioctl(fb1, ARKFB_SHOW_WINDOW);
	}
	at[0] = now_ms();
	for (int f = 1; f <= frames; f++) {
		b.alpha = (unsigned char)(255 * f / frames);
		vsync();
		ioctl(fd[up], ARKFB_SET_BLEND, &b);
		at[f] = now_ms();
	}
	if (up == 1)
		addr_at_frame(0, a_to);
	vsync();
	vsync();
	ioctl(fb1, ARKFB_HIDE_WINDOW);
	set_reg(up ? R_OSD2_CTL : R_OSD1_CTL, ctl);	/* blending as it was */
	set_reg(R_MODE0, mode0);
	set_reg(R_MODE1, mode1);
	report("fade", at, frames + 1);
}

int main(int argc, char **argv)
{
	static const struct { const char *name; unsigned int at; } regs[] = {
		{ "control", 0x04 }, { "alpha video/osd1", 0x24 }, { "alpha osd2/osd3", 0x28 }, { "mode 0", 0x60 }, { "mode 1", 0x64 },
		{ "osd1 ctl", 0x74 }, { "osd1 size", 0x78 }, { "osd1 pos", 0x7c }, { "osd1 addr", 0x80 },
		{ "osd2 ctl", 0x88 }, { "osd2 size", 0x8c }, { "osd2 pos", 0x90 }, { "osd2 addr", 0x94 },
		{ "osd1 window point", 0x160 }, { "osd2 window point", 0x164 }, { "osd1 source size", 0x170 }, { "osd2 source size", 0x174 },
	};
	const char *what = argc > 1 ? argv[1] : "regs";
	int frames = argc > 2 ? atoi(argv[2]) : 12, reps = argc > 3 ? atoi(argv[3]) : 3;
	struct fb_fix_screeninfo fix;
	unsigned int osd2[4], mode0, mode1, control;

	fb0 = open("/dev/fb0", O_RDWR);
	fb1 = open("/dev/fb1", O_RDWR);
	if (fb0 < 0 || fb1 < 0 || ioctl(fb0, FBIOGET_FSCREENINFO, &fix)) {
		printf("RESULT FAIL no /dev/fb0 and /dev/fb1\n");
		return 1;
	}
	base = fix.smem_start;
	fd[0] = fb0;
	fd[1] = fb1;
	for (int i = 2; i < argc; i++)
		if (!strcmp(argv[i], "up=1"))
			up = 1;
	if (!strcmp(what, "regs")) {
		printf("memory at 0x%08lx, %u bytes, line %u\n", base, fix.smem_len, fix.line_length);
		for (unsigned int i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
			printf("%-18s 0x%03x = %08x\n", regs[i].name, regs[i].at, reg(regs[i].at));
		return 0;
	}
	if (frames < 2 || frames > 300 || fix.smem_len < 3 * SCREEN) {
		printf("RESULT FAIL frames 2..300, three screens of memory\n");
		return 1;
	}
	mem = mmap(0, 3 * SCREEN, PROT_READ | PROT_WRITE, MAP_SHARED, fb0, 0);
	if (mem == MAP_FAILED) {
		printf("RESULT FAIL the layers' memory cannot be mapped\n");
		return 1;
	}
	paint(1, 0xff0b0d10, 0xffe6e9ee);
	paint(2, 0xff101820, 0xffffc890);

	/* The second layer as the first is: the format, and how it blends. */
	control = reg(R_CONTROL);
	mode0 = reg(R_MODE0);
	mode1 = reg(R_MODE1);
	for (int i = 0; i < 4; i++)
		osd2[i] = reg(R_OSD2_CTL + 4 * i);
	set_reg(R_OSD2_CTL, reg(R_OSD1_CTL));
	set_reg(R_MODE1, (mode1 & ~(3u << 14)) | ((mode1 >> 12 & 3) << 14));
	set_reg(R_MODE0, (mode0 & ~(0xfu << 20)) | ((mode0 >> 12 & 0xf) << 20));

	for (int r = 0; r < reps; r++) {
		if (!strcmp(what, "fade")) {
			fade(0, 1, frames);
			usleep(700000);
			fade(1, 0, frames);
		} else if (!strcmp(what, "back0")) {	/* back from the first screen: its address runs below the memory */
			push(0, 1, -1, frames);
			usleep(700000);
			push(1, 0, 1, frames);
		} else {
			push(0, 1, 1, frames);	/* into a list */
			usleep(700000);
			push(1, 2, 1, frames);	/* one level deeper */
			usleep(700000);
			push(2, 1, -1, frames);	/* back */
			usleep(700000);
			push(1, 0, -1, frames);	/* and out */
		}
		usleep(700000);
	}

	/* As it was. */
	vsync();
	set_reg(R_CONTROL, control);
	set_reg(R_MODE0, mode0);
	set_reg(R_MODE1, mode1);
	for (int i = 0; i < 4; i++)
		set_reg(R_OSD2_CTL + 4 * i, osd2[i]);
	printf("RESULT PASS\n");
	return 0;
}
