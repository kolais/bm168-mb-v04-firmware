#include "lvgl.h"
#include "screen.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static unsigned char pixels[1280 * 480 * 4];

static void stop(int sig) { (void)sig; running = 0; }
static uint32_t ticks(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area; (void)data;
    lv_display_flush_ready(display);
}

static int snapshot(const char *path)
{
    lv_display_t *display = lv_display_create(1280, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    autotop_screen_create();
    lv_refr_now(display);
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); return 1; }
    fputs("P6\n1280 480\n255\n", file);
    for (size_t i = 0; i < 1280 * 480; ++i) {
        unsigned char rgb[] = {pixels[4*i+2], pixels[4*i+1], pixels[4*i]};
        if (fwrite(rgb, 1, 3, file) != 3) { fclose(file); return 1; }
    }
    return fclose(file) != 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("autotop-ui [--snapshot FILE.ppm | --device /dev/fb0 /dev/input/event0]\n"
             "Desktop build: run without arguments. Device mode requires exclusive display/input access.");
        return 0;
    }
    int snap = argc == 3 && !strcmp(argv[1], "--snapshot");
    int device = argc == 4 && !strcmp(argv[1], "--device");
    if (!snap && !device && !(AUTOTOP_DESKTOP && argc == 1)) {
        fputs("Use --help. Device access must be explicitly requested.\n", stderr);
        return 2;
    }
    lv_init();
    lv_tick_set_cb(ticks);
    if (snap) return snapshot(argv[2]);
#if AUTOTOP_DESKTOP
    if (device) { fputs("Build the ARM target for device mode.\n", stderr); return 2; }
    lv_display_t *display = lv_sdl_window_create(1280, 480);
    lv_sdl_window_set_title(display, "AUTOTOP UI development");
    lv_sdl_mouse_create();
#else
    if (access(argv[2], R_OK | W_OK) || access(argv[3], R_OK)) {
        perror("Display/input access"); return 1;
    }
    lv_display_t *display = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(display, argv[2]);
    lv_indev_t *input = lv_evdev_create(LV_INDEV_TYPE_POINTER, argv[3]);
    if (!input) { fputs("Cannot open touch input\n", stderr); return 1; }
    lv_indev_set_display(input, display);
#endif
    autotop_screen_create();
    signal(SIGINT, stop); signal(SIGTERM, stop);
    while (running) { lv_timer_handler(); usleep(5000); }
    return 0;
}
