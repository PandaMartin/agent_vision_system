#include "lvgl.h"
#include "sw/app/ext_app/loader.h"
extern const lv_font_t font_cn_16;

#define SCREEN_W 800
#define SCREEN_H 480

static volatile int s_tick;
static lv_obj_t* s_time;
static lv_obj_t* s_arc;

static void on_exit(lv_event_t* e) { (void)e; fw_app_exit(); }

static void draw_grid(lv_obj_t* scr)
{
    int i;
    for (i = 0; i < 20; i++) {
        lv_obj_t* v = lv_obj_create(scr);
        lv_obj_set_pos(v, i * 40, 0);
        lv_obj_set_size(v, 1, SCREEN_H);
        lv_obj_set_style_bg_color(v, lv_color_hex(0x123A4A), 0);
        lv_obj_set_style_border_width(v, 0, 0);
        lv_obj_set_style_pad_all(v, 0, 0);
    }
    for (i = 0; i < 12; i++) {
        lv_obj_t* h = lv_obj_create(scr);
        lv_obj_set_pos(h, 0, i * 40);
        lv_obj_set_size(h, SCREEN_W, 1);
        lv_obj_set_style_bg_color(h, lv_color_hex(0x123A4A), 0);
        lv_obj_set_style_border_width(h, 0, 0);
        lv_obj_set_style_pad_all(h, 0, 0);
    }
}

static void draw_corners(lv_obj_t* scr)
{
    static const int cx[4] = { 16, SCREEN_W - 56, 16, SCREEN_W - 56 };
    static const int cy[4] = { 16, 16, SCREEN_H - 56, SCREEN_H - 56 };
    int i;
    for (i = 0; i < 4; i++) {
        lv_obj_t* a = lv_obj_create(scr);
        lv_obj_set_pos(a, cx[i], cy[i]);
        lv_obj_set_size(a, 40, 3);
        lv_obj_set_style_bg_color(a, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_border_width(a, 0, 0);
        lv_obj_set_style_pad_all(a, 0, 0);

        lv_obj_t* b = lv_obj_create(scr);
        lv_obj_set_pos(b, cx[i], cy[i]);
        lv_obj_set_size(b, 3, 40);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xFF2FD0), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
    }
}

static void draw_glow(lv_obj_t* scr)
{
    int i;
    static const unsigned int ring[3] = { 0x00303A, 0x00565F, 0x009AA8 };
    for (i = 0; i < 3; i++) {
        lv_obj_t* g = lv_obj_create(scr);
        lv_obj_set_pos(g, 130 - i * 10, 150 - i * 10);
        lv_obj_set_size(g, 540 + i * 20, 140 + i * 20);
        lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g, 2, 0);
        lv_obj_set_style_border_color(g, lv_color_hex(ring[i]), 0);
        lv_obj_set_style_radius(g, 12, 0);
        lv_obj_set_style_pad_all(g, 0, 0);
    }
}

static void on_tick(lv_timer_t* t)
{
    (void)t;
    s_tick++;
    if (s_time != 0) {
        lv_label_set_text(s_time, "12:00");
    }
    if (s_arc != 0) {
        lv_arc_set_value(s_arc, s_tick % 60);
    }
}

void app_main(void)
{
    lv_obj_t* btn;
    lv_obj_t* ex;
    lv_obj_t* scr = lv_obj_create(0);
    lv_obj_set_size(scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x060A14), 0);

    draw_grid(scr);
    draw_glow(scr);
    draw_corners(scr);

    s_time = lv_label_create(scr);
    lv_obj_set_pos(s_time, 330, 200);
    lv_obj_set_style_text_font(s_time, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(0x00E5FF), 0);
    lv_label_set_text(s_time, "12:00");

    s_arc = lv_arc_create(scr);
    lv_obj_set_pos(s_arc, 640, 180);
    lv_obj_set_size(s_arc, 120, 120);
    lv_arc_set_range(s_arc, 0, 59);
    lv_arc_set_value(s_arc, 0);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFF2FD0), LV_PART_INDICATOR);

    btn = lv_button_create(scr);
    lv_obj_set_pos(btn, 700, 420);
    lv_obj_set_size(btn, 80, 36);
    ex = lv_label_create(btn);
    lv_obj_set_style_text_font(ex, &font_cn_16, 0);
    lv_label_set_text(ex, "EXIT");
    lv_obj_center(ex);
    lv_obj_add_event_cb(btn, on_exit, LV_EVENT_CLICKED, 0);

    lv_screen_load(scr);
    app_timer_create(on_tick, 1000, 0);
}

__attribute__((section(".app_entry_first")))
void app_boot(void) { app_main(); }