/* SPDX-License-Identifier: MIT
 * See display.h.
 */
#include "display.h"
#include "screens.h"
#include <stdlib.h>
#include <string.h>

#define STRIDE (T_SCREEN_W * 4)

static uint8_t *target;	/* the screen that is shown */
static uint8_t *screens;	/* the layer memory: screen 0, and screen 1 after it if the display moves screens */
static int shown;
static void (*move)(enum ui_change how, int from, int to);
static enum ui_change arriving;	/* how the next whole-screen picture arrives */
static void (*fade)(int out);
static uint32_t asked_at;	/* when the arriving screen was asked for */
static lv_display_t *disp;
static void (*wait_vsync)(void);
static int finger_x, finger_y, finger_down;
static struct ui_display_stats stats;

/* LVGL has finished an area in its own buffer: copy it into the layer, row
 * by row, top down. A small area is over in a fraction of a frame. A whole
 * screen (2.4 MB) starts at the top of a frame and stays ahead of the scan. */
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	const int w = lv_area_get_width(a), h = lv_area_get_height(a);
	const int whole = w == T_SCREEN_W && h == T_SCREEN_H;
	enum ui_change how = whole ? arriving : UI_CHANGE_CUT;

	if (whole)
		arriving = UI_CHANGE_CUT;
	if (how != UI_CHANGE_CUT && move) {
		const uint32_t t0 = lv_tick_get();

		/* Into the screen that is not shown, then the layers bring it in. */
		memcpy(screens + (size_t)(1 - shown) * STRIDE * T_SCREEN_H, px, (size_t)STRIDE * T_SCREEN_H);
		stats.draw_ms = t0 - asked_at;
		stats.copy_ms = lv_tick_get() - t0;
		move(how, shown, 1 - shown);
		stats.move_ms = lv_tick_get() - t0 - stats.copy_ms;
		shown = 1 - shown;
		target = screens + (size_t)shown * STRIDE * T_SCREEN_H;
		stats.copies++;
		stats.full++;
		stats.moved++;
		stats.pixels += (unsigned long)w * h;
		lv_display_flush_ready(d);
		return;
	}
	if (wait_vsync && w * h >= T_SCREEN_W * T_SCREEN_H / 2)
		wait_vsync();
	for (int y = 0; y < h; y++)
		memcpy(target + (size_t)(a->y1 + y) * STRIDE + (size_t)a->x1 * 4, px + (size_t)y * w * 4, (size_t)w * 4);
	stats.copies++;
	stats.full += whole;
	stats.pixels += (unsigned long)w * h;
	lv_display_flush_ready(d);
}

static void finger(lv_indev_t *i, lv_indev_data_t *data)
{
	(void)i;
	data->point.x = finger_x;
	data->point.y = finger_y;
	data->state = finger_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int ui_display_init(uint8_t *layer, uint32_t (*now_ms)(void), void (*vsync)(void))
{
	uint8_t *draw = malloc((size_t)STRIDE * T_SCREEN_H);
	lv_indev_t *touch;

	if (!draw)
		return -1;
	target = screens = layer;
	shown = 0;
	wait_vsync = vsync;
	memset(target, 0, (size_t)STRIDE * T_SCREEN_H);	/* transparent until something is drawn */
	lv_init();
	lv_tick_set_cb(now_ms);
	disp = lv_display_create(T_SCREEN_W, T_SCREEN_H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
	/* A buffer the size of the screen: every changed area is drawn whole
	 * and copied once, whatever its size. */
	lv_display_set_buffers(disp, draw, NULL, STRIDE * T_SCREEN_H, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, flush);
	touch = lv_indev_create();
	lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(touch, finger);
	lv_indev_set_display(touch, disp);
	if (theme_init())
		return -1;
	ui_frame_init();
	return 0;
}

int ui_display_tick(void)
{
	return (int)lv_timer_handler();
}

void ui_display_draw_now(void)
{
	lv_refr_now(disp);
}

void ui_display_touch(int x, int y, int down)
{
	finger_x = x;
	finger_y = y;
	finger_down = down;
}

void ui_display_moves(void (*m)(enum ui_change how, int from, int to))
{
	move = m;
}

void ui_display_change(enum ui_change how)
{
	if (how == UI_CHANGE_CUT)
		return;
	stats.forward += how == UI_CHANGE_FORWARD;
	stats.back += how == UI_CHANGE_BACK;
	arriving = how;
	asked_at = lv_tick_get();
	lv_obj_invalidate(lv_screen_active());	/* one whole picture, not its changed parts */
}

void ui_display_fades(void (*f)(int out))
{
	fade = f;
}

void ui_display_fade(int out)
{
	stats.fades += out != 0;
	if (fade)
		fade(out);
}

void ui_display_stats(struct ui_display_stats *s)
{
	*s = stats;
}
