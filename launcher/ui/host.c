/* SPDX-License-Identifier: MIT
 * See host.h.
 */
#include "host.h"
#include "display.h"
#include "screens.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t *pixels;
static uint32_t now_ms;		/* set by the caller, so every run is the same */

static uint32_t ticks(void)
{
	return now_ms;
}

int host_init(void)
{
	pixels = calloc(T_SCREEN_W * T_SCREEN_H, 4);
	if (!pixels)
		return -1;
	return ui_display_init(pixels, ticks, NULL);
}

void host_run(int ms)
{
	lv_timer_handler();
	for (uint32_t end = now_ms + ms; now_ms < end;) {
		now_ms += end - now_ms < 10 ? end - now_ms : 10;
		lv_timer_handler();
	}
}

void host_tap(int x, int y)
{
	host_draw();	/* a finger touches what is on the screen */
	ui_display_touch(x, y, 1);
	host_run(60);
	ui_display_touch(x, y, 0);
	host_run(60);
}

void host_draw(void)
{
	ui_display_draw_now();
}

int host_save_ppm(const char *path, int over_carplay)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", T_SCREEN_W, T_SCREEN_H);
	for (int i = 0; i < T_SCREEN_W * T_SCREEN_H; i++) {
		const uint8_t *p = pixels + 4 * i;	/* B G R A */
		int x = i % T_SCREEN_W, y = i / T_SCREEN_W, a = p[3];
		int under[3] = { over_carplay ? 24 + x * 40 / T_SCREEN_W : 0, over_carplay ? 60 + y * 60 / T_SCREEN_H : 0,
				 over_carplay ? 110 + x * 60 / T_SCREEN_W : 0 };
		unsigned char rgb[3] = {
			(unsigned char)((p[2] * a + under[0] * (255 - a)) / 255),
			(unsigned char)((p[1] * a + under[1] * (255 - a)) / 255),
			(unsigned char)((p[0] * a + under[2] * (255 - a)) / 255),
		};

		fwrite(rgb, 1, 3, f);
	}
	return fclose(f) ? -1 : 0;
}

int host_drawn_pixels(void)
{
	int n = 0;

	host_draw();
	for (int i = 0; i < T_SCREEN_W * T_SCREEN_H; i++)
		n += pixels[4 * i + 3] != 0;
	return n;
}
