#include "lvgl.h"
#include "screen.h"

static lv_obj_t *status_label;

static void on_carplay(lv_event_t *event)
{
    (void)event;
    /* Connect the verified vendor IPC here once runtime tracing is complete. */
    lv_label_set_text(status_label, "CarPlay control is not connected yet.");
}

static void on_settings(lv_event_t *event)
{
    (void)event;
    lv_label_set_text(status_label, "Edit ui/screen.c to build your interface and behaviour.");
}

static void button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 340, 150);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x203047), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_center(label);
}

void autotop_screen_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0c1420), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xeaf1fa), 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "AUTOTOP");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 48, 36);
    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "Interface development");
    lv_obj_align(subtitle, LV_ALIGN_TOP_RIGHT, -48, 45);

    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 720, 150);
    lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(row, "CarPlay", on_carplay);
    button(row, "Settings", on_settings);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "Vendor projection and vehicle controls await integration.");
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_LEFT, 48, -40);
}
