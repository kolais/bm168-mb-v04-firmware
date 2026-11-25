/* SPDX-License-Identifier: MIT
 * What the rescue installer shows (recovery/plan-a/init-rescue). It runs
 * without the launcher and without its fonts, so it brings its own: capital
 * letters and digits on a 5 x 7 grid, drawn large. Plain, but it says what
 * is happening on the panel, where the driver is looking.
 *
 *   rescue-ui picture                     ask the MCU for the unit's picture
 *                                         (the panel shows the car's until then)
 *   rescue-ui show PERCENT LINE [LINE..]  the screen: a title, up to four
 *                                         lines, and a bar when PERCENT >= 0
 *   rescue-ui mem ADDRESS LENGTH          that much of physical memory to
 *                                         standard output: what U-Boot loaded
 *                                         above the RAM Linux was given can be
 *                                         mapped from /dev/mem but not read
 *
 * Lines are upper-cased; a character the font lacks is a blank.
 * RESCUE_UI_PPM=FILE draws into a picture file instead of /dev/fb0 (the host
 * check: tools/rescue_ui_check.sh).
 */
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#define W 1280
#define H 480
#define GROUND 0xff0b0d10u
#define INK 0xffe6e9eeu
#define MUTED 0xff959da8u
#define TRACK 0xff1a1f27u

static uint32_t px[W * H];

static const struct { char c; const char *rows[7]; } font[] = {
	{ 'A', { ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
	{ 'B', { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." } },
	{ 'C', { ".###.", "#...#", "#....", "#....", "#....", "#...#", ".###." } },
	{ 'D', { "####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####." } },
	{ 'E', { "#####", "#....", "#....", "####.", "#....", "#....", "#####" } },
	{ 'F', { "#####", "#....", "#....", "####.", "#....", "#....", "#...." } },
	{ 'G', { ".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###." } },
	{ 'H', { "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
	{ 'I', { ".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###." } },
	{ 'J', { "..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.." } },
	{ 'K', { "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#" } },
	{ 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
	{ 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#" } },
	{ 'N', { "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#" } },
	{ 'O', { ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
	{ 'P', { "####.", "#...#", "#...#", "####.", "#....", "#....", "#...." } },
	{ 'Q', { ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#" } },
	{ 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
	{ 'S', { ".####", "#....", "#....", ".###.", "....#", "....#", "####." } },
	{ 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
	{ 'U', { "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
	{ 'V', { "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.." } },
	{ 'W', { "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#" } },
	{ 'X', { "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#" } },
	{ 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.." } },
	{ 'Z', { "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####" } },
	{ '0', { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." } },
	{ '1', { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." } },
	{ '2', { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" } },
	{ '3', { "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###." } },
	{ '4', { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." } },
	{ '5', { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." } },
	{ '6', { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." } },
	{ '7', { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." } },
	{ '8', { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." } },
	{ '9', { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." } },
	{ '.', { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." } },
	{ ',', { ".....", ".....", ".....", ".....", ".##..", "..#..", ".#..." } },
	{ ':', { ".....", ".##..", ".##..", ".....", ".##..", ".##..", "....." } },
	{ '-', { ".....", ".....", ".....", "#####", ".....", ".....", "....." } },
	{ '%', { "##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##" } },
	{ '/', { "....#", "....#", "...#.", "..#..", ".#...", "#....", "#...." } },
	{ '!', { "..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.." } },
	{ '\'', { "..#..", "..#..", ".#...", ".....", ".....", ".....", "....." } },
};

static void rect(int x, int y, int w, int h, uint32_t c)
{
	for (int j = y; j < y + h && j < H; j++)
		for (int i = x; i < x + w && i < W; i++)
			if (i >= 0 && j >= 0)
				px[j * W + i] = c;
}

/* One line, each font dot a square of `s` pixels; returns the width used. */
static int text(int x, int y, int s, uint32_t c, const char *t)
{
	const int x0 = x;

	for (; *t && x + 5 * s <= W; t++, x += 6 * s) {
		const char ch = (char)toupper((unsigned char)*t);

		for (unsigned g = 0; g < sizeof(font) / sizeof(font[0]); g++) {
			if (font[g].c != ch)
				continue;
			for (int r = 0; r < 7; r++)
				for (int k = 0; k < 5; k++)
					if (font[g].rows[r][k] == '#')
						rect(x + k * s, y + r * s, s, s, c);
			break;
		}
	}
	return x - x0;
}

static int show(int percent, int n, char **lines)
{
	const char *ppm = getenv("RESCUE_UI_PPM");
	int fd;

	rect(0, 0, W, H, GROUND);
	text(56, 40, 8, INK, "SOFTWARE RESCUE");
	for (int i = 0; i < n && i < 4; i++)
		text(56, 150 + i * 60, 5, i ? MUTED : INK, lines[i]);
	if (percent >= 0) {
		if (percent > 100)
			percent = 100;
		rect(56, 410, W - 112, 20, TRACK);
		rect(56, 410, (W - 112) * percent / 100, 20, INK);
	}
	if (ppm) {	/* the host check: a picture file */
		FILE *f = fopen(ppm, "wb");

		if (!f)
			return 1;
		fprintf(f, "P6\n%d %d\n255\n", W, H);
		for (int i = 0; i < W * H; i++) {
			const unsigned char rgb[3] = { px[i] >> 16 & 255, px[i] >> 8 & 255, px[i] & 255 };

			fwrite(rgb, 1, 3, f);
		}
		fclose(f);
		return 0;
	}
	fd = open("/dev/fb0", O_WRONLY);	/* 1280 x 480, 32 bits: blue, green, red, alpha */
	if (fd < 0)
		return 1;
	for (size_t at = 0; at < sizeof(px);) {
		ssize_t w = pwrite(fd, (const char *)px + at, sizeof(px) - at, (off_t)at);

		if (w <= 0) {
			close(fd);
			return 1;
		}
		at += (size_t)w;
	}
	close(fd);
	return 0;
}

/* AA 55 00 04 4C 00 00 B0: "picture source: the unit", as the launcher sends
 * it (launcher/launcherd.c, picture_to); the MCU's line is ttyS2 at 115200. */
static int picture(void)
{
	static const unsigned char frame[] = { 0xAA, 0x55, 0x00, 0x04, 0x4C, 0x00, 0x00, 0xB0 };
	struct termios t;
	int fd = open("/dev/ttyS2", O_RDWR | O_NOCTTY);

	if (fd < 0 || tcgetattr(fd, &t))
		return 1;
	cfmakeraw(&t);
	cfsetispeed(&t, B115200);
	cfsetospeed(&t, B115200);
	t.c_cflag |= CLOCAL | CREAD;
	if (tcsetattr(fd, TCSANOW, &t) || write(fd, frame, sizeof(frame)) != (ssize_t)sizeof(frame))
		return 1;
	tcdrain(fd);
	close(fd);
	return 0;
}

static int mem(unsigned long addr, unsigned long len)
{
	const unsigned long page = (unsigned long)sysconf(_SC_PAGESIZE), base = addr & ~(page - 1), skip = addr - base;
	int fd = open(getenv("RESCUE_UI_MEM") ? getenv("RESCUE_UI_MEM") : "/dev/mem", O_RDONLY);
	const unsigned char *map;

	if (fd < 0 || !len)
		return 1;
	map = mmap(NULL, skip + len, PROT_READ, MAP_SHARED, fd, (off_t)base);
	if (map == MAP_FAILED)
		return 1;
	for (unsigned long at = 0; at < len;) {
		ssize_t w = write(1, map + skip + at, len - at > 65536 ? 65536 : len - at);

		if (w <= 0)
			return 1;
		at += (unsigned long)w;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "picture"))
		return picture();
	if (argc >= 4 && !strcmp(argv[1], "show"))
		return show(atoi(argv[2]), argc - 3, argv + 3);
	if (argc == 4 && !strcmp(argv[1], "mem"))
		return mem(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
	fprintf(stderr, "usage: rescue-ui picture | rescue-ui show PERCENT LINE [LINE..] | rescue-ui mem ADDRESS LENGTH\n");
	return 2;
}
