// =============================================================================
// app_test —— 问题隔离测试应用II(纯LVGL: 零跨任务帧缓冲写入, 零摄像头依赖)
//
// 上一版(直写帧缓冲洞)出现控件缺失/背景异常, 本版把变量全部消掉:
//   - 所有控件固定坐标绝对定位(不依赖LV_SIZE_CONTENT/嵌套flex组)
//   - 动态显示用纯LVGL控件: 色块颜色每200ms轮换 + 计数/帧率标签
//   - 不创建任何FreeRTOS任务, 不碰帧缓冲
// 判读: 本版渲染完整稳定 => GUI层清白, 问题在"洞直写"路径(vidwin技术);
//       本版仍缺控件/背景异常 => LVGL移植层或launcher壳本身的问题。
// =============================================================================

#include <string.h>

#include "lvgl.h"

#include "sw/app/launcher.h"
#include "sw/impl/lcd/lvgl_port.h"

#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF

//控件引用
static lv_obj_t* s_lbl_cnt;
static lv_obj_t* s_lbl_fps;
static lv_obj_t* s_lbl_pat;
static lv_obj_t* s_swatch;
static lv_obj_t* s_sw_cam;
static lv_obj_t* s_sw_hdmi;
static unsigned   s_cnt;

// -----------------------------------------------------------------------------
// 开关回调: 纯界面反馈(toast), 不发任何硬件命令
static void sw_cam_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    launcher_toast(lv_obj_has_state(s_sw_cam, LV_STATE_CHECKED)
                   ? "测试: 相机开" : "测试: 相机关");
}

static void sw_hdmi_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    launcher_toast(lv_obj_has_state(s_sw_hdmi, LV_STATE_CHECKED)
                   ? "测试: HDMI开" : "测试: HDMI关");
}

// -----------------------------------------------------------------------------
// 固定坐标控件助手(绝对定位, 去样式)
static lv_obj_t* place(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static void app_on_open(lv_obj_t* parent)
{
    //根节点尺寸取自显示分辨率(不可用lv_obj_get_width: 刚set_size的父容器
    //坐标是惰性计算的, 此刻读到0会把root建成0x0, 子控件全部被裁剪)
    lv_display_t* disp = lv_display_get_default();
    const int cw = lv_display_get_horizontal_resolution(disp) - 16;
    const int ch = lv_display_get_vertical_resolution(disp) - 40 - 16;
    lv_obj_t* root = place(parent, 0, 0, cw, ch);

    //---- 行1: 左侧计数/帧率 + 右侧相机/HDMI开关(全部固定坐标) ----
    s_lbl_cnt = lv_label_create(root);
    lv_obj_set_pos(s_lbl_cnt, 0, 2);
    lv_obj_set_style_text_font(s_lbl_cnt, &font_cn_20, 0);
    lv_obj_set_style_text_color(s_lbl_cnt, lv_color_hex(COL_CYAN), 0);
    lv_label_set_text(s_lbl_cnt, "0");

    s_lbl_fps = lv_label_create(root);
    lv_obj_set_pos(s_lbl_fps, 90, 6);
    lv_obj_set_style_text_font(s_lbl_fps, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_fps, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_fps, "-- fps");

    lv_obj_t* cam_tx = lv_label_create(root);
    lv_obj_set_pos(cam_tx, 570, 6);
    lv_obj_set_style_text_font(cam_tx, &font_cn_16, 0);
    lv_obj_set_style_text_color(cam_tx, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(cam_tx, "相机");

    s_sw_cam = lv_switch_create(root);
    lv_obj_set_pos(s_sw_cam, 612, 2);
    lv_obj_set_size(s_sw_cam, 36, 22);
    lv_obj_add_state(s_sw_cam, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_cam, sw_cam_cb, LV_EVENT_VALUE_CHANGED, 0);

    lv_obj_t* hdmi_tx = lv_label_create(root);
    lv_obj_set_pos(hdmi_tx, 662, 6);
    lv_obj_set_style_text_font(hdmi_tx, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hdmi_tx, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(hdmi_tx, "HDMI");

    s_sw_hdmi = lv_switch_create(root);
    lv_obj_set_pos(s_sw_hdmi, 708, 2);
    lv_obj_set_size(s_sw_hdmi, 36, 22);
    lv_obj_add_event_cb(s_sw_hdmi, sw_hdmi_cb, LV_EVENT_VALUE_CHANGED, 0);

    //---- 色块(纯LVGL动态显示, 代替上一版的直写图案) ----
    lv_obj_t* vcard = lv_obj_create(root);
    lv_obj_set_pos(vcard, 0, 34);
    lv_obj_set_size(vcard, 458, 263);
    lv_obj_set_style_bg_color(vcard, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(vcard, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vcard, 12, 0);
    lv_obj_set_style_border_color(vcard, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(vcard, 1, 0);
    lv_obj_set_style_pad_all(vcard, 6, 0);

    s_swatch = lv_obj_create(vcard);
    lv_obj_set_size(s_swatch, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(s_swatch, 8, 0);
    lv_obj_set_style_border_width(s_swatch, 0, 0);
    lv_obj_set_style_bg_color(s_swatch, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_bg_opa(s_swatch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_swatch, LV_OBJ_FLAG_SCROLLABLE);

    //---- 卡片下方说明行 ----
    s_lbl_pat = lv_label_create(root);
    lv_obj_set_pos(s_lbl_pat, 2, 312);
    lv_obj_set_style_text_font(s_lbl_pat, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_pat, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_pat, "帧数 0");
}

// -----------------------------------------------------------------------------
static void app_on_close(void)
{
    //无任务无资源; LVGL对象由launcher整屏删除
}

// -----------------------------------------------------------------------------
static void app_refresh(void)
{
    static const uint32_t hues[6] = {
        0x00E5FF, 0x76FF03, 0xFF1744, 0xFFEA00, 0xD500F9, 0x2962FF,
    };
    s_cnt++;
    lv_obj_set_style_bg_color(s_swatch,
                              lv_color_hex(hues[s_cnt % 6]), 0);
    lv_label_set_text_fmt(s_lbl_cnt, "%u", s_cnt);
    lv_label_set_text_fmt(s_lbl_fps, "%u fps", (unsigned)gui_fps());
    lv_label_set_text_fmt(s_lbl_pat, "帧数 %u", s_cnt);
}

// -----------------------------------------------------------------------------
const LauncherApp app_test = {
    .name       = "测试",
    .icon       = LV_SYMBOL_SETTINGS,
    .on_open    = app_on_open,
    .on_close   = app_on_close,
    .refresh    = app_refresh,
    .refresh_ms = 200,
    .accent    = 0x34E0B4,
};
