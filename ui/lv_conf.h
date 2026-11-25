#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 32
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_LOG 1
#define LV_LOG_PRINTF 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20
/* Inter is rendered from its outlines (launcher/ui/theme.c). */
#define LV_USE_TINY_TTF 1
/* The Wi-Fi code on the update's receiving screen (launcher/ui/receive.c). */
#define LV_USE_QRCODE 1
#define LV_USE_LINUX_FBDEV (!AUTOTOP_DESKTOP && !AUTOTOP_SNAP)
#define LV_USE_EVDEV (!AUTOTOP_DESKTOP && !AUTOTOP_SNAP)
#define LV_USE_SDL AUTOTOP_DESKTOP
#define LV_SDL_INCLUDE_PATH <SDL.h>
#endif
