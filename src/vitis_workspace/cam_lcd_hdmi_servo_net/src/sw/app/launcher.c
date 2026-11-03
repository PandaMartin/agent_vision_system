// =============================================================================
// launcher —— 桌面启动器实现
//
// 桌面(v3 卡片仪表盘, 参考 LVGL Thermostat/Smart Hub 官方演示的语言):
//   超薄状态栏(运行时钟 + fps + SD应用重扫)
//   HERO系统卡: 发光弧环(呼吸) + 环心大号运行时长 + 外设LED胶囊行(SD/NET/
//     CAM/TP, 活动者光晕呼吸)
//   应用tile网格: 一行4列, 近方形固定比例tile(顶边与HERO卡对齐, 不强占
//     满高度); 每tile独立accent渐变底 + 白色大符号, 按压缩放回弹,
//     开机错峰淡入上浮; SD应用(/apps)自动排在内置应用之后, 同一网格
//   壁纸: 0:/theme/wallpaper.bmp(app_task加载经bridge发布, lv_image垫底淡入;
//     卡片半透明浮在壁纸上; 无壁纸保持原渐变)
// 应用屏: 顶栏(返回键+应用名) + 内容区(on_open填); 退出滑回桌面并整屏删除
// toast: 挂lv_layer_top(屏切换不影响), 底部胶囊淡入停留淡出排队
// =============================================================================

#include <string.h>
#include <stdio.h>
#include <malloc.h>

#include "lvgl.h"

#include "sw/app/launcher.h"
#include "sw/app/int_app/pages.h"
#include "sw/app/bridge.h"
#include "sw/app/ext_app/loader.h"
#include "sw/app/appctl.h"
#include "sw/impl/lcd/lvgl_port.h"

//界面配色(移植层/页面共用的一套语义色)
#define COL_BG_TOP    0x0A0E1A
#define COL_BG_BOT    0x05070D
#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF
#define COL_LIME      0x76FF03
#define COL_LED_OFF   0x8A94B8      //LED熄灭色(明确可辨的灰=未运行)
#define COL_ARC_TRACK 0x1A2440      //弧环轨道

#define APP_MAX       8             //应用注册上限
#define TOAST_RING    4             //toast排队深度
#define TOAST_HOLD_MS      2400     //普通toast停留
#define TOAST_HOLD_LONG_MS 4000     //操作结果等需细读的toast停留
#define TOAST_FADE_MS 300
#define TICK_MS       100           //框架节拍(应用刷新/toast状态机)
#define STATUS_MS     500           //桌面状态栏刷新周期

#define STATUS_H      32            //桌面状态栏高
#define HERO_W        292           //HERO系统卡宽(高=内容区全高)
#define GRID_COLS     4             //应用网格列数(SD应用排内置后同网格)
#define GRID_PAD      8             //网格内边距
#define GRID_GAP      10            //网格行列间距

//外设LED(hero胶囊行)
enum { LED_SD = 0, LED_NET, LED_TP, LED_N };

//注册表与运行状态
static const LauncherApp* s_apps[APP_MAX];
static int   s_app_n;
static int   s_active = -1;                   //-1=桌面, -2=SD应用运行中
static lv_obj_t* s_scr_desktop;               //桌面屏(常驻)
static lv_obj_t* s_scr_app;                   //当前应用屏(退出时删除)
static lv_obj_t* s_tiles;                     //应用tile容器(内置+SD统一网格)
static uint32_t  s_refr_accu;                 //活动应用刷新累积ms
static uint32_t  s_status_accu;               //状态栏刷新累积ms
static uint32_t  s_last_sdapp_gen;            //SD应用列表代数(重建网格用)
static uint32_t  s_last_app_gen;              //加载状态代数(toast/交接用)
static int       s_first_build = 1;           //开机首建(进场动画只此一次)

//桌面控件
static lv_obj_t* s_st_clock;                  //状态栏运行时钟 hh:mm:ss
static lv_obj_t* s_st_fps;
static lv_obj_t* s_hero_time;                 //弧环中心 HH:MM
static lv_obj_t* s_hero_arc;                  //弧环(内存占用仪表)
static lv_obj_t* s_hero_mem;                  //环心下方 MEM xx%%
static lv_obj_t* s_info_val[2];               //IP/屏 值
//堆边界由lscript.ld导出(与platform.cpp同源): 弧环的总量基准
extern char HeapBase;
extern char HeapLimit;
static lv_obj_t* s_led_dot[LED_N];
static lv_obj_t* s_led_halo[LED_N];
static lv_obj_t* s_wall;                      //壁纸(有则垫底)
static uint32_t  s_last_wall_gen;

//toast状态机(挂lv_layer_top, 跨屏常驻)
static lv_obj_t* s_toast;
static lv_obj_t* s_toast_lbl;
static char      s_toast_q[TOAST_RING][40];
static uint16_t  s_toast_hold[TOAST_RING];    //每条停留时长(普通/长)
static int       s_toast_head, s_toast_tail;
static int       s_toast_state;               //0隐藏 1显示中
static uint16_t  s_toast_hold_cur;            //当前显示条的停留时长
static uint32_t  s_toast_t;

// -----------------------------------------------------------------------------
// 小动画助手(exec回调)
static void anim_set_opa_cb(void* var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

static void anim_set_translate_cb(void* var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t*)var, v, 0);
}

//进场: 淡入+上浮(delay<0不动画, 用于SD应用变化后的重建)
static void entry_anim(lv_obj_t* obj, int delay)
{
    if (delay < 0)
        return;
    lv_obj_set_style_opa(obj, LV_OPA_0, 0);
    lv_obj_set_style_translate_y(obj, 26, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_delay(&a, (uint32_t)delay * 40);
    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_start(&a);
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 26, 0);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_delay(&a, (uint32_t)delay * 40);
    lv_anim_set_exec_cb(&a, anim_set_translate_cb);
    lv_anim_start(&a);
}

//按压缩放回弹(声明式状态过渡: PRESSED态scale 0.95, 220ms过冲曲线)
static void press_fx(lv_obj_t* obj)
{
    static lv_style_transition_dsc_t tr;       //共享一份描述符(常驻)
    static int inited;
    if (!inited)
    {
        static const lv_style_prop_t props[] = {
            LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, 0
        };
        lv_style_transition_dsc_init(&tr, props, lv_anim_path_overshoot,
                                     220, 0, NULL);
        inited = 1;
    }
    lv_obj_set_style_transition(obj, &tr, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_x(obj, 243, LV_STATE_PRESSED);  //0.95x
    lv_obj_set_style_transform_scale_y(obj, 243, LV_STATE_PRESSED);
}

//呼吸循环(光晕/弧环共用套路: 无限往复)
static void breathe(lv_obj_t* obj, lv_anim_exec_xcb_t cb,
                    int32_t from, int32_t to, uint32_t ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_playback_duration(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static uint32_t accent_dim(uint32_t c)         //accent的暗端(55%)
{
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return ((r * 55 / 100) << 16) | ((g * 55 / 100) << 8) | (b * 55 / 100);
}

static void fmt_uptime(char* buf, int cap, int with_sec)
{
    uint32_t sec = lv_tick_get() / 1000;
    uint32_t h = sec / 3600, m = (sec / 60) % 60;
    if (with_sec)
        snprintf(buf, (size_t)cap, "%02u:%02u:%02u",
                 (unsigned)h, (unsigned)m, (unsigned)(sec % 60));
    else
        snprintf(buf, (size_t)cap, "%02u:%02u", (unsigned)h, (unsigned)m);
}

// -----------------------------------------------------------------------------
// toast: 底部胶囊, 淡入->停留->淡出, 排队逐条
static void toast_show_next(void)
{
    if (s_toast_state || s_toast_head == s_toast_tail)
        return;                                    //显示中或队列空
    lv_label_set_text(s_toast_lbl, s_toast_q[s_toast_head]);
    s_toast_hold_cur = s_toast_hold[s_toast_head];
    s_toast_head = (s_toast_head + 1) % TOAST_RING;
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_toast, LV_OPA_COVER, 0);
    s_toast_state = 1;
    s_toast_t = 0;
}

static void toast_push(const char* text, uint16_t hold)
{
    int next = (s_toast_tail + 1) % TOAST_RING;
    if (next == s_toast_head)
        return;                                    //队列满: 丢弃(提示本就是瞬态)
    strncpy(s_toast_q[s_toast_tail], text, sizeof(s_toast_q[0]) - 1);
    s_toast_q[s_toast_tail][sizeof(s_toast_q[0]) - 1] = 0;
    s_toast_hold[s_toast_tail] = hold;
    s_toast_tail = next;
    toast_show_next();
}

void launcher_toast(const char* text)
{
    toast_push(text, TOAST_HOLD_MS);
}

void launcher_toast_long(const char* text)
{
    toast_push(text, TOAST_HOLD_LONG_MS);
}

// -----------------------------------------------------------------------------
// 应用打开/关闭: 框架管理screen与对象生命周期
static void app_back_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    launcher_app_close();
}

static void icon_click_cb(lv_event_t* e)
{
    launcher_app_open((int)(intptr_t)lv_event_get_user_data(e));
}

//SD应用图标: 投递启动命令(加载/校验/搬运在app_task, gui侧只在READY时调entry)
static void sdapp_click_cb(lv_event_t* e)
{
    AppCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = APP_CMD_APP_LAUNCH;
    cmd.val = (int)(intptr_t)lv_event_get_user_data(e);
    appctl_post(&cmd);
}

static void sdapp_rescan_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    AppCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = APP_CMD_APP_RESCAN;
    appctl_post(&cmd);
}

void launcher_app_open(int idx)
{
    if (idx < 0 || idx >= s_app_n || s_active >= 0)
        return;
    const LauncherApp* app = s_apps[idx];

    //---- 应用屏: 顶栏 + 内容区 ----
    lv_display_t* disp = lv_display_get_default();
    const int32_t scr_w = lv_display_get_horizontal_resolution(disp);
    const int32_t scr_h = lv_display_get_vertical_resolution(disp);

    lv_obj_t* scr = lv_obj_create(0);
    s_scr_app = scr;
    lv_obj_set_size(scr, scr_w, scr_h);
    //纯色背景(不用渐变: 排除主题/渐渲染变量, 桌面仍保留渐变)
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG_TOP), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, scr_w, LAUNCHER_APP_TOPBAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, LAUNCHER_APP_PAD, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);

    lv_obj_t* back = lv_button_create(bar);
    lv_obj_set_height(back, LAUNCHER_APP_TOPBAR_H - 10);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_obj_set_style_text_font(back_lbl, &font_cn_16, 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " 返回");
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back, app_back_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t* title = lv_label_create(bar);
    lv_obj_set_style_text_font(title, &font_cn_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, app->name);

    //内容区: 顶栏之下的全部区域(on_open在此自填满)
    //位置/边距与vidwin的SHELL_ORIGIN镜像一致(帧缓冲"洞"按此定位)
    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, LAUNCHER_APP_PAD,
                   LAUNCHER_APP_TOPBAR_H + LAUNCHER_APP_PAD);
    lv_obj_set_size(content, scr_w - 2 * LAUNCHER_APP_PAD,
                    scr_h - LAUNCHER_APP_TOPBAR_H - 2 * LAUNCHER_APP_PAD);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    //---- 应用建内容 + 滑入 ----
    if (app->on_open)
        app->on_open(content);

    s_active = idx;
    s_refr_accu = 0;
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
    if (app->refresh)                             //切入立即刷一次
        app->refresh();
}

void launcher_app_close(void)
{
    if (s_active < 0)
        return;
    const LauncherApp* app = s_apps[s_active];

    //先停应用的非LVGL资源(此回调里不得再触碰LVGL对象)
    if (app->on_close)
        app->on_close();

    s_active = -1;
    s_scr_app = 0;
    //滑回桌面, 动画结束后框架自动删除应用屏(整屏对象随之释放)
    lv_screen_load_anim(s_scr_desktop, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, true);
}

// -----------------------------------------------------------------------------
// 桌面状态刷新(500ms): 时钟/弧环大数字/外设LED/fps
static void hero_leds_refresh(const GuiSnapshot* s)
{
    //SD/NET/TOUCH 在位性与语义色(活动者显示呼吸光晕)。
    //相机/HDMI状态不在此列: 它只在相机应用内为"运行", 而那时看不到主页
    const int on[LED_N] = { s->sd_ok, s->net_up, s->touch_id != 0 };
    const uint32_t col[LED_N] = { COL_LIME, COL_LIME, COL_CYAN };
    for (int i = 0; i < LED_N; i++)
    {
        lv_obj_set_style_bg_color(s_led_dot[i],
            lv_color_hex(on[i] ? col[i] : COL_LED_OFF), 0);
        if (on[i])
            lv_obj_remove_flag(s_led_halo[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_led_halo[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void status_refresh(void)
{
    GuiSnapshot s;
    gui_model_snapshot(&s);

    char b[16];
    fmt_uptime(b, sizeof(b), 1);
    lv_label_set_text(s_st_clock, b);
    fmt_uptime(b, sizeof(b), 0);
    lv_label_set_text(s_hero_time, b);
    hero_leds_refresh(&s);
    lv_label_set_text_fmt(s_st_fps, "%u fps", (unsigned)gui_fps());

    //弧环=剩余内存占总堆容量的百分比(newlib堆, 容量取lscript.ld堆段)
    struct mallinfo mi = mallinfo();
    uint32_t cap = (uint32_t)((char*)&HeapLimit - (char*)&HeapBase);
    uint32_t used = (uint32_t)mi.uordblks;
    uint32_t free_b = (cap > used) ? (cap - used) : 0;
    uint32_t free_pct = cap ? (unsigned)((uint64_t)free_b * 100u / cap) : 0;
    lv_arc_set_value(s_hero_arc, (int32_t)free_pct);
    lv_label_set_text_fmt(s_hero_mem, "FREE %u%%", (unsigned)free_pct);

    //信息行: IP随网络状态
    lv_label_set_text(s_info_val[0], s.net_up ? s.net_ip : "--");
}

// -----------------------------------------------------------------------------
// 壁纸(gen_wall变化时调用一次): 内存RGB888垫底淡入, 卡片浮其上
static lv_image_dsc_t s_wall_dsc;              //gui常驻引用(静态)
static void wallpaper_apply(void* pix)
{
    lv_display_t* disp = lv_display_get_default();
    const int32_t scr_w = lv_display_get_horizontal_resolution(disp);
    const int32_t scr_h = lv_display_get_vertical_resolution(disp);

    memset(&s_wall_dsc, 0, sizeof(s_wall_dsc));
    s_wall_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_wall_dsc.header.cf = LV_COLOR_FORMAT_RGB888;   //内存序B,G,R与BMP一致
    s_wall_dsc.header.w = (uint32_t)scr_w;
    s_wall_dsc.header.h = (uint32_t)scr_h;
    s_wall_dsc.header.stride = (uint32_t)scr_w * 3;
    s_wall_dsc.data_size = (uint32_t)scr_w * (uint32_t)scr_h * 3;
    s_wall_dsc.data = (const uint8_t*)pix;

    s_wall = lv_image_create(s_scr_desktop);
    lv_image_set_src(s_wall, &s_wall_dsc);
    lv_image_set_antialias(s_wall, false);     //1:1显示, 免缩放采样开销
    lv_obj_set_pos(s_wall, 0, 0);
    lv_obj_set_size(s_wall, scr_w, scr_h);
    lv_obj_remove_flag(s_wall, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(s_wall, 0);           //垫底(状态栏/卡片之上层不变)

    lv_obj_set_style_opa(s_wall, LV_OPA_0, 0);
    lv_anim_t a;                               //单次淡入(勿用breathe: 那是往复的)
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_wall);
    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_duration(&a, 450);
    lv_anim_start(&a);
}

// -----------------------------------------------------------------------------
// 应用tile(网格单元): accent渐变底 + 白色大符号 + 名称; 按压缩放回弹
// 近方形固定比例(高=宽+8), 内容竖排居中; 应用多时网格竖向滚动
static void tile_add(lv_obj_t* parent, const char* name, const char* icon,
                     uint32_t accent, lv_event_cb_t cb, intptr_t ud, int idx,
                     int w, int h)
{
    const uint32_t a = accent ? accent : 0x00B0FF;

    lv_obj_t* t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_radius(t, 18, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(a), 0);
    lv_obj_set_style_bg_grad_color(t, lv_color_hex(accent_dim(a)), 0);
    lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_90, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_shadow_color(t, lv_color_hex(a), 0);
    lv_obj_set_style_shadow_width(t, 12, 0);
    lv_obj_set_style_shadow_opa(t, LV_OPA_30, 0);
    lv_obj_set_style_shadow_offset_y(t, 3, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    press_fx(t);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t, 8, 0);

    lv_obj_t* ic = lv_label_create(t);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    lv_label_set_text(ic, icon);

    lv_obj_t* tx = lv_label_create(t);
    lv_obj_set_style_text_font(tx, &font_cn_16, 0);
    lv_obj_set_style_text_color(tx, lv_color_white(), 0);
    lv_label_set_long_mode(tx, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tx, w - 12);
    lv_obj_set_style_text_align(tx, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(tx, name);

    lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, (void*)ud);
    entry_anim(t, idx);
}

//网格重建: 内置应用在前, SD应用(/apps)自动排其后, 同一网格(一行4列);
//tile近方形固定比例, 顶边与HERO卡对齐, 不强占满高度; gen_sdapp变化时重建
static void desktop_rebuild(void)
{
    if (s_tiles == 0)
        return;
    lv_obj_clean(s_tiles);

    //网格几何与launcher_init建容器同源(惰性布局不可读坐标, 按分辨率算)
    lv_display_t* disp = lv_display_get_default();
    const int grid_w = lv_display_get_horizontal_resolution(disp)
                       - HERO_W - 3 * LAUNCHER_APP_PAD;
    const int tile_w = (grid_w - 2 * GRID_PAD
                        - (GRID_COLS - 1) * GRID_GAP) / GRID_COLS;
    const int tile_h = tile_w + 8;           //近方形固定比例, 不随行数拉伸

    GuiSnapshot s;
    gui_model_snapshot(&s);

    int idx = 0;                             //全局序号: 进场动画错峰用
    for (int i = 0; i < s_app_n; i++, idx++)
        tile_add(s_tiles, s_apps[i]->name, s_apps[i]->icon,
                 s_apps[i]->accent, icon_click_cb, (intptr_t)i, idx,
                 tile_w, tile_h);
    for (int i = 0; i < s.sdapp_n; i++, idx++)
        tile_add(s_tiles, s.sdapp_name[i], LV_SYMBOL_PLAY, 0,
                 sdapp_click_cb, (intptr_t)i, idx, tile_w, tile_h);

    s_last_sdapp_gen = s.gen_sdapp;
    s_first_build = 0;
}

// -----------------------------------------------------------------------------
// 框架节拍: 活动应用周期刷新 + 桌面刷新 + toast状态机
static void app_tick_cb(lv_timer_t* t)
{
    LV_UNUSED(t);

    //活动应用周期刷新(桌面态/应用退场动画期不刷)
    if (s_active >= 0)
    {
        const LauncherApp* a = s_apps[s_active];
        if (a->refresh && a->refresh_ms)
        {
            s_refr_accu += TICK_MS;
            if (s_refr_accu >= a->refresh_ms)
            {
                s_refr_accu = 0;
                a->refresh();
            }
        }
    }

    //桌面态: SD应用列表/壁纸代数变化 + SD应用加载状态机 + 状态栏
    if (s_active < 0)
    {
        GuiSnapshot s;
        gui_model_snapshot(&s);

        if (s.gen_sdapp != s_last_sdapp_gen)
            desktop_rebuild();

        //壁纸到达(开机app_task加载完): 垫底淡入一次
        if (s.gen_wall != s_last_wall_gen)
        {
            s_last_wall_gen = s.gen_wall;
            if (s.wall_pix)
                wallpaper_apply(s.wall_pix);
        }

        //SD应用加载状态机(app_task搬运完READY -> 这里调entry; 退出请求->清理)
        if (s.gen_app != s_last_app_gen)
        {
            s_last_app_gen = s.gen_app;
            switch (s.app_state)
            {
            case APPST_READY:
                //重置计时器记账后跳应用入口(代码已由app_task同步好cache)
                s_active = -2;                   //应用运行中(非内置)
                loader_timers_reset();
                ((void (*)(void))APP_LOAD_BASE)();   //app_boot, 快速返回
                gui_model_pub_app_state(APPST_RUNNING, 0);
                break;
            case APPST_EXIT_REQ:
                loader_timers_kill();            //应用定时器强制回收
                s_active = -1;
                //滑回桌面; 动画结束自动删除应用屏及其全部子对象
                lv_screen_load_anim(s_scr_desktop, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                                    220, 0, true);
                gui_model_pub_app_state(APPST_IDLE, 0);
                break;
            case APPST_FAILED:
                launcher_toast(s.app_err);
                break;
            case APPST_LOADING:
                launcher_toast("加载中");
                break;
            default:
                break;
            }
        }

        s_status_accu += TICK_MS;
        if (s_status_accu >= STATUS_MS)
        {
            s_status_accu = 0;
            status_refresh();
        }
    }

    //toast: 停留(当前条时长)-FADE后开始淡出, 到点隐藏并放下一条
    if (s_toast_state)
    {
        s_toast_t += TICK_MS;
        if (s_toast_t == s_toast_hold_cur - TOAST_FADE_MS)
        {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_toast);
            lv_anim_set_exec_cb(&a, anim_set_opa_cb);
            lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_0);
            lv_anim_set_duration(&a, TOAST_FADE_MS);
            lv_anim_start(&a);
        }
        else if (s_toast_t >= s_toast_hold_cur)
        {
            lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            s_toast_state = 0;
            toast_show_next();
        }
    }
    else
    {
        toast_show_next();                          //空闲时检查队列
    }
}

// -----------------------------------------------------------------------------
void launcher_register(const LauncherApp* app)
{
    if (s_app_n < APP_MAX && app)
        s_apps[s_app_n++] = app;
}

int launcher_active_app(void)
{
    return s_active;
}

// -----------------------------------------------------------------------------
// 桌面屏: 状态栏 + HERO系统卡 + 应用tile网格(内置+SD统一)
static lv_obj_t* style_screen(lv_obj_t* scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(COL_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    return scr;
}

//HERO系统卡: 弧环(呼吸) + 环心大号运行时长 + 外设LED胶囊行
static lv_obj_t* hero_build(lv_obj_t* parent, int32_t x, int32_t y,
                            int32_t w, int32_t h)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(card, 1, 0);

    //弧环(视觉主角): 青色进度条呼吸往复; 旋钮点带青色光晕
    lv_obj_t* arc = lv_arc_create(card);
    lv_obj_set_size(arc, 236, 236);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 20);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 70);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_ARC_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(arc, 5, LV_PART_KNOB);
    lv_obj_set_style_bg_color(arc, lv_color_hex(COL_BG_TOP), LV_PART_KNOB);
    lv_obj_set_style_border_color(arc, lv_color_hex(COL_CYAN), LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 2, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(arc, lv_color_hex(COL_CYAN), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(arc, 24, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(arc, LV_OPA_60, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    s_hero_arc = arc;

    //环心: 大号运行时长(HH:MM) + 小标
    s_hero_time = lv_label_create(arc);
    lv_obj_set_style_text_font(s_hero_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_hero_time, lv_color_white(), 0);
    lv_label_set_text(s_hero_time, "00:00");
    lv_obj_center(s_hero_time);

    s_hero_mem = lv_label_create(arc);
    lv_obj_set_style_text_font(s_hero_mem, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hero_mem, lv_color_hex(COL_CYAN), 0);
    lv_label_set_text(s_hero_mem, "MEM --");
    lv_obj_align(s_hero_mem, LV_ALIGN_CENTER, 0, 38);

    //外设LED胶囊行: 点+呼吸光晕+标签
    static const char* names[LED_N] = { "SD", "NET", "TOUCH" };
    for (int i = 0; i < LED_N; i++)
    {
        lv_obj_t* cap = lv_obj_create(card);
        lv_obj_remove_style_all(cap);
        lv_obj_set_pos(cap, 16 + i * 74, 272);
        lv_obj_set_size(cap, 66, 44);
        lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cap, 3, 0);

        lv_obj_t* stack = lv_obj_create(cap);
        lv_obj_remove_style_all(stack);
        lv_obj_set_size(stack, 16, 16);
        lv_obj_remove_flag(stack, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* halo = lv_obj_create(stack);      //光晕(常驻呼吸, 隐藏切换)
        lv_obj_remove_style_all(halo);
        lv_obj_set_size(halo, 16, 16);
        lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(halo, lv_color_hex(COL_CYAN), 0);
        lv_obj_set_style_bg_opa(halo, LV_OPA_40, 0);
        lv_obj_center(halo);
        lv_obj_add_flag(halo, LV_OBJ_FLAG_HIDDEN);
        breathe(halo, anim_set_opa_cb, LV_OPA_20, LV_OPA_70, 1200);

        lv_obj_t* dot = lv_obj_create(stack);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COL_LED_OFF), 0);
        lv_obj_center(dot);

        lv_obj_t* lbl = lv_label_create(cap);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text(lbl, names[i]);

        s_led_dot[i] = dot;
        s_led_halo[i] = halo;
    }

    //分隔细线 + 信息行(IP/LCD)
    lv_obj_t* line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 16, 330);
    lv_obj_set_size(line, w - 32, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    static const char* ikeys[2] = { "IP", "LCD" };
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t* k = lv_label_create(card);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text(k, ikeys[i]);
        lv_obj_set_pos(k, 16, 344 + i * 30);

        lv_obj_t* v = lv_label_create(card);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(v, lv_color_white(), 0);
        lv_label_set_text(v, "--");
        lv_obj_set_pos(v, 90, 344 + i * 30);
        s_info_val[i] = v;
    }

    //LCD分辨率静态一次成
    lv_display_t* disp = lv_display_get_default();
    lv_label_set_text_fmt(s_info_val[1], "%ux%u",
                          (unsigned)lv_display_get_horizontal_resolution(disp),
                          (unsigned)lv_display_get_vertical_resolution(disp));
    return card;
}

void launcher_init(void)
{
    //---- 内置应用(注册顺序=图标顺序) ----
    launcher_register(&app_camera);
    launcher_register(&app_infer);
    launcher_register(&app_gimbal);
    launcher_register(&app_files);
    launcher_register(&app_transfer);
    launcher_register(&app_test);

    //---- 桌面屏 ----
    s_scr_desktop = style_screen(lv_screen_active());

    lv_display_t* disp = lv_display_get_default();
    const int32_t scr_w = lv_display_get_horizontal_resolution(disp);
    const int32_t scr_h = lv_display_get_vertical_resolution(disp);

    //---- 超薄状态栏: 左运行时钟, 右fps + SD重扫 ----
    lv_obj_t* bar = lv_obj_create(s_scr_desktop);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, scr_w, STATUS_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, LAUNCHER_APP_PAD + 4, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);

    lv_obj_t* left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 8, 0);

    lv_obj_t* pwr = lv_label_create(left);
    lv_obj_set_style_text_font(pwr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(pwr, lv_color_hex(COL_CYAN), 0);
    lv_label_set_text(pwr, LV_SYMBOL_POWER);

    s_st_clock = lv_label_create(left);
    lv_obj_set_style_text_font(s_st_clock, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_st_clock, lv_color_white(), 0);
    lv_label_set_text(s_st_clock, "00:00:00");

    lv_obj_t* right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 12, 0);

    s_st_fps = lv_label_create(right);
    lv_obj_set_style_text_font(s_st_fps, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_st_fps, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_st_fps, "-- fps");

    //SD应用重扫按钮(换卡/传新应用后点一下)
    lv_obj_t* btn_rescan = lv_button_create(right);
    lv_obj_set_size(btn_rescan, 30, 24);
    lv_obj_set_style_bg_color(btn_rescan, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(btn_rescan, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(btn_rescan, 1, 0);
    lv_obj_t* rs_ic = lv_label_create(btn_rescan);
    lv_obj_set_style_text_font(rs_ic, &lv_font_montserrat_14, 0);
    lv_label_set_text(rs_ic, LV_SYMBOL_REFRESH);
    lv_obj_center(rs_ic);
    lv_obj_add_event_cb(btn_rescan, sdapp_rescan_cb, LV_EVENT_CLICKED, 0);

    //---- 内容区几何 ----
    const int32_t ct_y = STATUS_H + LAUNCHER_APP_PAD;
    const int32_t ct_h = scr_h - ct_y - LAUNCHER_APP_PAD;

    //---- HERO系统卡(左, 全内容高) ----
    hero_build(s_scr_desktop, LAUNCHER_APP_PAD, ct_y, HERO_W, ct_h);

    //---- 应用tile网格(右, 一行4列, 顶边与HERO卡对齐;
    //      tile近方形固定比例, SD应用排内置后, 应用多时竖向滚动) ----
    s_tiles = lv_obj_create(s_scr_desktop);
    lv_obj_remove_style_all(s_tiles);
    lv_obj_set_pos(s_tiles, LAUNCHER_APP_PAD + HERO_W + LAUNCHER_APP_PAD, ct_y);
    lv_obj_set_size(s_tiles, scr_w - HERO_W - 3 * LAUNCHER_APP_PAD, ct_h);
    lv_obj_set_flex_flow(s_tiles, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_tiles, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_tiles, GRID_PAD, 0);
    lv_obj_set_style_pad_row(s_tiles, GRID_GAP, 0);
    lv_obj_set_style_pad_column(s_tiles, GRID_GAP, 0);
    lv_obj_set_scrollbar_mode(s_tiles, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_tiles, LV_DIR_VER);

    //---- toast胶囊(挂顶层layer, 屏切换不影响) ----
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_toast, 16, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_pad_hor(s_toast, 16, 0);
    lv_obj_set_style_pad_ver(s_toast, 8, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_toast_lbl = lv_label_create(s_toast);
    lv_obj_set_style_text_font(s_toast_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_toast_lbl, lv_color_white(), 0);
    lv_label_set_text(s_toast_lbl, "");

    //---- 节拍定时器 + 图标首建 + 首刷状态栏 ----
    lv_timer_create(app_tick_cb, TICK_MS, 0);
    desktop_rebuild();
    status_refresh();
}
