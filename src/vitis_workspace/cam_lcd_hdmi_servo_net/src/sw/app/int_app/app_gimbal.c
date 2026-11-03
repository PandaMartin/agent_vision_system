// =============================================================================
// app_gimbal —— 云台舵机控制应用(内置): 摄像头画面居中预览 + 舵机上下左右操控
//
// 摄像头装在双轴舵机云台上, 本应用 = 中央实时画面 + 底栏操控:
//   视频洞640x360(内容区水平居中、顶部平齐, 与相机应用/vidwin几何镜像),
//   洞内不放任何控件(vidwin任务每帧直写帧缓冲); 进入时开摄像头链路。
//   底栏 = 预览状态/云台角度 + [上][下](俯仰) [左][右](水平) + [回中]
//
// 舵机命令为何不走app_task命令管道: servo_set_angle = 两次变量写+一次TTC
// match寄存器写, 微秒级无阻塞、不碰FatFs —— 与camera.c在gui上下文直调
// vidwin_enable同类; 直调让按键即时生效+角度即时回显, 免去命令队列+桥
// 代数的一来一回(150ms刷新节拍)。摄像头链路启停仍走CAM_OPEN/CLOSE命令。
//
// 操作语义: 点按=步进SERVO_STEP_DEG; 长按=按LVGL重复节拍连续步进;
//   上下=SERVO_TILT(U9), 左右=SERVO_PAN(W8), 限幅30~150°由驱动保证。
//   本板实测(2026-10-18): 云台装夹方向与角度增减相反, 上/左=角度减小,
//   下/右=角度增大(符号见s_steps表; 若换装夹再反转, 整表取反即可)。
//
// 视频洞几何(与vidwin.c的vidwin_task_start_centered一致, 两处必须镜像):
// 洞=640x360, 内容区水平居中、顶部平齐 => parent坐标((784-640)/2, 0)=(72,0)
// =============================================================================

#include <string.h>

#include "lvgl.h"

#include "hw/dev/servo.h"

#include "sw/app/launcher.h"
#include "sw/app/bridge.h"
#include "sw/app/appctl.h"
#include "sw/impl/lcd/lvgl_port.h"
#include "sw/impl/lcd/vidwin.h"

#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF

//视频洞(parent坐标; vidwin按LCD屏坐标开窗, 两者按同一布局常量推出)
#define HOLE_X        ((784 - VIDWIN_VID_W) / 2)      //152
#define HOLE_Y        0
#define BAR_Y         (VIDWIN_VID_H + 8)              //278 控制栏顶

//舵机步进角(度): 点按一步; 长按按LVGL重复节拍连续步进
#define SERVO_STEP_DEG  5.0f

//控件引用
static lv_obj_t* s_lbl_status;      //底栏: 预览状态(未开启/预览fps)
static lv_obj_t* s_lbl_angle;       //底栏: 云台角度(左右90度 上下90度)

static GuiSnapshot s_snap;
static uint32_t   s_last_cam, s_last_vid, s_last_cap;

//单步命令表(按钮user_data=下标); 符号按2026-10-18装机实测: 上/左=角度减小
typedef struct { ServoCh ch; float delta; } StepDef;
static const StepDef s_steps[] = {
    { SERVO_TILT, -SERVO_STEP_DEG },      //上
    { SERVO_TILT, +SERVO_STEP_DEG },      //下
    { SERVO_PAN,  +SERVO_STEP_DEG },      //左
    { SERVO_PAN,  -SERVO_STEP_DEG },      //右
};
#define STEP_N  ((int)(sizeof(s_steps) / sizeof(s_steps[0])))

// -----------------------------------------------------------------------------
static void post_cmd(int type, int val)
{
    AppCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint8_t)type;
    cmd.val = val;
    appctl_post(&cmd);
}

static lv_obj_t* place(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static lv_obj_t* make_btn(lv_obj_t* parent, int x, int y, int w, int h,
                          uint32_t bg, const char* txt, lv_event_cb_t cb,
                          const lv_font_t* font, void* ud)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t* t = lv_label_create(b);
    lv_obj_set_style_text_font(t, font ? font : &font_cn_16, 0);
    lv_label_set_text(t, txt);
    lv_obj_center(t);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

// -----------------------------------------------------------------------------
// 云台角度回显(驱动缓存即真实位置: 波形由TTC硬件持续保持)
static void angle_refresh(void)
{
    if (!servo_ready())
    {
        lv_label_set_text(s_lbl_angle, "舵机未连接");
        return;
    }
    char buf[40];
    lv_snprintf(buf, sizeof(buf), "左右%u度 上下%u度",
                (unsigned)(servo_get_angle(SERVO_PAN) + 0.5f),
                (unsigned)(servo_get_angle(SERVO_TILT) + 0.5f));
    lv_label_set_text(s_lbl_angle, buf);
}

//步进键: 点按一步, 长按连续步进(同一回调挂两种事件)
static void step_cb(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= STEP_N)
        return;
    servo_set_angle(s_steps[i].ch,
                    servo_get_angle(s_steps[i].ch) + s_steps[i].delta);
    angle_refresh();
}

static void center_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (!servo_ready())
        return;
    servo_center();
    angle_refresh();
}

// -----------------------------------------------------------------------------
static void app_on_open(lv_obj_t* parent)
{
    //内容区几何(launcher应用屏: 顶栏40+左右边距8), 800x480 => 784x424
    //尺寸取显示分辨率计算(不可读parent坐标: LVGL惰性布局, 此刻为0)
    lv_display_t* disp = lv_display_get_default();
    const int cw = lv_display_get_horizontal_resolution(disp) - 16;
    const int ch = lv_display_get_vertical_resolution(disp) - 40 - 16;
    lv_obj_t* root = place(parent, 0, 0, cw, ch);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    //视频洞(HOLE_X,HOLE_Y,480x270)内不放任何控件——vidwin任务每帧直写该区

    //底栏左列: 预览状态 + 云台角度
    s_lbl_status = lv_label_create(root);
    lv_obj_set_pos(s_lbl_status, 8, BAR_Y + 6);
    lv_obj_set_width(s_lbl_status, 240);
    lv_obj_set_style_text_font(s_lbl_status, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_status, "未开启");

    s_lbl_angle = lv_label_create(root);
    lv_obj_set_pos(s_lbl_angle, 8, BAR_Y + 28);
    lv_obj_set_width(s_lbl_angle, 250);
    lv_obj_set_style_text_font(s_lbl_angle, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_angle, lv_color_hex(COL_CYAN), 0);
    lv_label_set_text(s_lbl_angle, "");

    //D-pad: 上下(俯仰)/左右(水平)两组, 长按连发
    static const int    bx[STEP_N]    = { 262, 342, 432, 512 };   //x坐标
    static const char*  const bn[STEP_N] = { "上", "下", "左", "右" };
    for (int i = 0; i < STEP_N; i++)
    {
        lv_obj_t* b = make_btn(root, bx[i], BAR_Y + 2, 72, 52, 0x2A3450,
                               bn[i], 0, &font_cn_20, (void*)(intptr_t)i);
        lv_obj_add_event_cb(b, step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                            (void*)(intptr_t)i);
    }

    //回中(双轴90度), 配色区别于步进键
    make_btn(root, cw - 178, BAR_Y, 170, 52, 0x0F5E6E, "回中",
             center_cb, &font_cn_20, 0);

    //static状态复位(应用被删后static仍在, 重开必须从零开始)
    gui_model_snapshot(&s_snap);
    s_last_cam = s_snap.gen_cam;
    s_last_vid = s_snap.gen_vid;
    s_last_cap = s_snap.gen_cap;
    angle_refresh();
    vidwin_enable(1);                      //上一会话on_close停过笔

    post_cmd(APP_CMD_CAM_OPEN, 0);         //进入即开摄像头(异步, 结果经gen_cam)
}

static void app_on_close(void)
{
    //先停视频写帧(退场动画期间洞区不能再被覆写), 链路停止走命令
    vidwin_enable(0);
    vidwin_task_stop();
    post_cmd(APP_CMD_CAM_CLOSE, 0);
}

// -----------------------------------------------------------------------------
static void app_refresh(void)
{
    gui_model_snapshot(&s_snap);

    //摄像头链路状态(gen_cam: 相机应用与本应用共用同一链路命令)
    if (s_snap.gen_cam != s_last_cam)
    {
        s_last_cam = s_snap.gen_cam;
        lv_label_set_text(s_lbl_status, s_snap.cam_on ? "预览" : "未开启");
    }

    //预览帧率(vid任务1s发布一次)
    if (s_snap.gen_vid != s_last_vid)
    {
        s_last_vid = s_snap.gen_vid;
        if (s_snap.cam_on)
        {
            char buf[48];
            lv_snprintf(buf, sizeof(buf), "预览 %ufps",
                        (unsigned)s_snap.vid_fps);
            lv_label_set_text(s_lbl_status, buf);
        }
    }

    //相机打开失败等提示(cap_ok=0: 错误文本), 以toast呈现
    if (s_snap.gen_cap != s_last_cap)
    {
        s_last_cap = s_snap.gen_cap;
        if (!s_snap.cap_ok && s_snap.cap_file[0])
            launcher_toast(s_snap.cap_file);
    }
}

// -----------------------------------------------------------------------------
const LauncherApp app_gimbal = {
    .name       = "舵机",
    .icon       = LV_SYMBOL_SHUFFLE,
    .on_open    = app_on_open,
    .on_close   = app_on_close,
    .refresh    = app_refresh,
    .refresh_ms = 150,
    .accent    = 0x9D7BFF,
};
