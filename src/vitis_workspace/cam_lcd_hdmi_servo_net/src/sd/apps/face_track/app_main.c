// =============================================================================
// 人脸追踪 —— SD卡加载应用: 人脸检测模型(wd_face_ret_fp32) + 双轴云台闭环
//
// 数据流(复用固件现成链路, 应用自身零推理代码):
//   启动: post CAM_OPEN -> MODEL_SELECT(wd_face_ret_fp32) -> RUN_SET 1。
//   推理: app_task的app_infer_pump每帧抓拍推理(固件已放行SD应用在场时借泵,
//         结果照常经bridge det通道发布), vidwin预览洞自动叠加平滑检测框。
//   追踪: 本应用定时器(gui任务)订阅det快照 -> 选目标框算归一化误差 ->
//         PID连续输出角速度, 每50ms一小步servo_set_angle直调(微秒级无
//         阻塞, 与app_gimbal同款gui上下文豁免); 链路启停仍走CAM命令。
//
// PID结构(检测~4Hz, 控制20Hz, 检测间隔靠预测+补偿填平):
//   误差e=归一化偏差(pan: 画面左为正需增角 e=-nx; tilt: 画面上为正需减角
//   e=+ny; 符号按2026-10-18装机实测, 换装夹取反)。e的单位=FOV/2度。
//   检测到来: 延迟前馈(检测框是infer_ms前抓的, 扣掉期间自身转角) ->
//     目标速度估计 v=(Δe+Δ自转/half_fov)/T(LPF);
//   每50ms: e_est = e_det + v*Δt预测 - 自转累计/half_fov(自运动补偿,
//     让误差随自身转动实时收敛, 是"跟得紧+不顿挫"的关键);
//   PID: u = P*e + I*∫e + D*de/dt(度/秒), 静差死区内冻结并衰减积分,
//     输出限速, angle += u*0.05s 连续小步。
//   目标框取"离上一目标中心最近的框"(多脸不跳目标), 无历史取最大面积;
//   检测无脸/检测停摆>1s报丢失, 超LOSS_RC_MS且"丢失回中"开着则缓慢归中。
//
// 视频洞几何(与vidwin.c的vidwin_task_start_centered镜像, 洞内禁放控件):
//   洞=LCD(160,48)起480x270居中, 左右栏与底栏均避开该区。
// =============================================================================

#include <string.h>

#include "lvgl.h"

#include "sw/app/ext_app/loader.h"
#include "sw/app/launcher.h"
#include "sw/app/appctl.h"
#include "sw/app/bridge.h"
#include "hw/dev/servo.h"
#include "sw/impl/lcd/vidwin.h"

//---- 配色(与内置应用一致) ----
#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TRACK_BG  0x202A44
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF
#define COL_OK        0x76FF03
#define COL_WARN      0xFFB300

//---- 布局(LCD 800x480; 视频洞LCD(160,48,480,270)内不放任何控件) ----
//底栏卡内坐标(卡原点LCD(8,326), 子对象一律父坐标系)
#define PANL_L_X      8      //左栏卡
#define PANL_L_Y      56
#define PANL_L_W      144
#define PANL_L_H      256
#define BTN_X         648    //右栏按钮列(洞右缘640止)
#define BTN_W         144
#define PANL_B_X      8      //底栏卡
#define PANL_B_Y      326
#define PANL_B_W      784
#define PANL_B_H      138
#define BAR_RX        16     //偏差条x(底栏卡内)
#define BAR_RY_H      58
#define BAR_RY_V      88
#define BAR_W         320
#define BAR_H         12
#define CUR_W         10     //游标宽

//---- 控制节拍/门槛(不进UI) ----
#define TICK_MS        50    //PID控制节拍(20Hz, 与50Hz舵机PWM合拍)
#define DT_S           (TICK_MS * 0.001f)
#define PRED_CAP_S     0.6f  //目标预测视界(超过不再外推, 检测停摆防漂移)
#define STALL_LOST_MS  1000  //检测停摆超时=丢失
#define LOSS_RC_MS     2500  //丢失超时后开始缓慢回中
#define RC_STEP_DEG    0.8f  //回中步进(度/tick)
#define TGT_NEAR_PX    300   //换锁阈值: 距上一目标中心小于此值优先跟旧目标
#define V_EST_MAX      3.0f  //目标速度估计限幅(e/s)
#define INTEG_MAX      0.25f //积分项限幅(e·s)

//---- 可调参数(底栏右侧滑条; 默认值在此改) ----
#define FMT_NONE 0
#define FMT_PCT  1
#define FMT_DEG  2
enum { SP_P = 0, SP_I, SP_D, SP_FOV, SP_DB, SP_VMAX, SP_N };
static const struct { const char* name; int vmin, vmax, vdef; uint8_t fmt; }
    s_param_cfg[SP_N] = {
        [SP_P]     = { "P",    0, 150,  80, FMT_NONE }, //度/秒 每满幅误差
        [SP_I]     = { "I",    0,  60,  15, FMT_NONE }, //度/秒 每(e·s)
        [SP_D]     = { "D",    0,  30,   8, FMT_NONE }, //度/秒 每(e/s)
        [SP_FOV]   = { "FOV", 40, 100,  60, FMT_DEG }, //相机水平视野(度):
                                                       // 自运动补偿的换算基准
        [SP_DB]    = { "静差", 0,  10,   2, FMT_PCT }, //死区(半幅百分比)
        [SP_VMAX]  = { "速度", 15, 120, 45, FMT_NONE }, //输出限速(度/秒)
    };
static int s_param[SP_N];
static lv_obj_t* s_lbl_val[SP_N];

//目标模型(SD卡/models下文件夹名前缀)
#define FACE_MODEL_PREFIX  "wd_face"

//---- 控件引用 ----
static lv_obj_t* s_lbl_state;      //顶栏: 追踪状态
static lv_obj_t* s_lbl_line[6];    //左栏: 相机/模型/推理/人脸/置信/舵机
static lv_obj_t* s_lbl_angle;      //底栏: 云台角度
static lv_obj_t* s_btn_track;      //右栏: 开始/停止追踪
static lv_obj_t* s_btn_rc;         //右栏: 丢失回中开关
static lv_obj_t* s_cur_h;          //底栏: 水平偏差游标
static lv_obj_t* s_cur_v;          //底栏: 垂直偏差游标

//---- PID轴状态([0]=pan [1]=tilt) ----
typedef struct {
    float e_det;    //最近检测的归一化误差(延迟前馈已并入)
    float v_t;      //目标误差速度估计(1/s, 检测节拍LPF)
    float moved;    //上次检测以来累计命令角(度, 自运动补偿用)
    float u;        //最近输出角速度(度/s, 延迟前馈用)
    float integ;    //积分项(e·s)
    float deriv;    //微分项(e/s, LPF)
    float e_prev;   //上一tick有效误差
} PidAxis;
static PidAxis s_ax[2];

//---- 运行状态(static=bss, 每次启动由加载器清零复位) ----
static GuiSnapshot s_snap;
static lv_timer_t* s_timer;
static int   s_model_idx  = -1;    //人脸模型在清单中的下标(-1未找到)
static int   s_run_posted = 0;     //RUN_SET 1已投递(失败自停后不自动重启)
static int   s_track      = 0;     //舵机追踪开关
static int   s_rc_on      = 1;     //丢失回中开关(默认开)
static int   s_have_tgt   = 0;     //有跟踪目标
static int   s_lost       = 0;     //目标丢失中
static float s_tgt_cx, s_tgt_cy;   //上一目标中心(源帧像素)
static uint32_t s_last_det_ms;     //最近一次有效检测的时刻
static uint32_t s_t_det_prev;      //上一次检测时刻(目标速度估计用)
static uint32_t s_last_gen;        //已消费的det代数
static int   s_last_score   = -1;

// -----------------------------------------------------------------------------
static void post_cmd(uint8_t type, int val)
{
    AppCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.val = val;
    appctl_post(&cmd);
}

//在快照模型清单里找人脸模型(名字前缀匹配; 找不到=-1)
static int find_face_model(const GuiSnapshot* s)
{
    for (int i = 0; i < s->n_models && i < GUI_MODEL_MAX; i++)
        if (strncmp(s->models_name[i], FACE_MODEL_PREFIX,
                    sizeof(FACE_MODEL_PREFIX) - 1) == 0)
            return i;
    return -1;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// -----------------------------------------------------------------------------
// 界面小工具(UI铁律: 固定坐标+固定尺寸, 纯容器去CLICKABLE/SCROLLABLE)
static lv_obj_t* card(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    return o;
}

static lv_obj_t* label(lv_obj_t* parent, int x, int y, int w,
                       const char* text, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    if (w > 0)
        lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t* make_btn(lv_obj_t* parent, int x, int y, int w, int h,
                          uint32_t bg, const char* txt, const lv_font_t* font,
                          lv_event_cb_t cb, void* ud)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t* t = lv_label_create(b);
    lv_obj_set_style_text_font(t, font, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, txt);
    lv_obj_center(t);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t* block(lv_obj_t* parent, int x, int y, int w, int h,
                       uint32_t color, int radius)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    if (radius > 0)
        lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

//偏差指示条(底栏卡内父坐标系): 底槽+中心刻度, 返回游标
static lv_obj_t* err_bar(lv_obj_t* parent, int y)
{
    block(parent, BAR_RX, y, BAR_W, BAR_H, 0x0E1422, 6);
    block(parent, BAR_RX + BAR_W / 2 - 1, y - 3, 2, BAR_H + 6,
          COL_CARD_EDGE, 0);
    return block(parent, BAR_RX + (BAR_W - CUR_W) / 2, y - 2,
                 CUR_W, BAR_H + 4, COL_CYAN, 3);
}

//游标定位: v∈[-1,1] -> 槽内位置(出界截断)
static void cursor_set(lv_obj_t* cur, float v)
{
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f)  v = 1.0f;
    const int span = (BAR_W - CUR_W) / 2;
    lv_obj_set_x(cur, BAR_RX + (BAR_W - CUR_W) / 2 + (int)(v * span + 0.5f));
}

// -----------------------------------------------------------------------------
// 状态文案(顶栏)
static void state_refresh(void)
{
    const char* txt;
    uint32_t col;
    if (!s_track)               { txt = "待机";   col = COL_TEXT_DIM; }
    else if (!s_lost)           { txt = "追踪中"; col = COL_OK; }
    else                        { txt = "丢失";   col = COL_WARN; }
    lv_label_set_text(s_lbl_state, txt);
    lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(col), 0);
}

static void track_btn_refresh(void)
{
    lv_obj_t* t = lv_obj_get_child(s_btn_track, 0);
    lv_label_set_text(t, s_track ? "停止追踪" : "开始追踪");
    lv_obj_set_style_bg_color(s_btn_track,
                              lv_color_hex(s_track ? 0x8F2E2E : 0x1E6E5E), 0);
}

static void rc_btn_refresh(void)
{
    lv_obj_t* t = lv_obj_get_child(s_btn_rc, 0);
    lv_label_set_text(t, s_rc_on ? "丢失回中 开" : "丢失回中 关");
}

//参数值回显
static void param_val_refresh(int id)
{
    char buf[12];
    if (s_param_cfg[id].fmt == FMT_PCT)
        lv_snprintf(buf, sizeof(buf), "%u%%", (unsigned)s_param[id]);
    else if (s_param_cfg[id].fmt == FMT_DEG)
        lv_snprintf(buf, sizeof(buf), "%u度", (unsigned)s_param[id]);
    else
        lv_snprintf(buf, sizeof(buf), "%u", (unsigned)s_param[id]);
    lv_label_set_text(s_lbl_val[id], buf);
}

// -----------------------------------------------------------------------------
// 检测到来: 选目标 + 误差更新(延迟前馈/目标速度估计)
static void control_step(const GuiDetBox* boxes, int n, int src_w, int src_h)
{
    //选目标: 有历史优先"离上一目标近的框", 否则取最大面积
    int pick = -1;
    long best_area = 0, best_d2 = 0;
    for (int i = 0; i < n; i++)
    {
        const long w = boxes[i].x1 - boxes[i].x0, h = boxes[i].y1 - boxes[i].y0;
        if (w <= 0 || h <= 0)
            continue;
        const long area = w * h;
        const float cx = (boxes[i].x0 + boxes[i].x1) * 0.5f;
        const float cy = (boxes[i].y0 + boxes[i].y1) * 0.5f;
        if (s_have_tgt)
        {
            const float dx = cx - s_tgt_cx, dy = cy - s_tgt_cy;
            const long d2 = (long)(dx * dx + dy * dy);
            if (d2 < (long)TGT_NEAR_PX * TGT_NEAR_PX &&
                (pick < 0 || d2 < best_d2))
            {
                pick = i; best_d2 = d2; best_area = area;
            }
        }
        else if (area > best_area)
        {
            pick = i; best_area = area;
        }
    }

    const uint32_t now = lv_tick_get();
    s_last_det_ms = now;

    if (pick < 0)
    {
        //检测在跑但没有脸: 即时进丢失(回中由超时门槛把关)
        s_have_tgt = 0;
        s_last_score = -1;
        for (int i = 0; i < 2; i++)
        {
            s_ax[i].v_t = 0;
            s_ax[i].integ = 0;
            s_ax[i].deriv = 0;
            s_ax[i].u = 0;
        }
        if (s_track && !s_lost)
        {
            s_lost = 1;
            state_refresh();
        }
        return;
    }

    const GuiDetBox* b = &boxes[pick];
    const int had_prev = s_have_tgt;
    s_have_tgt = 1;
    s_lost = 0;
    s_tgt_cx = (b->x0 + b->x1) * 0.5f;
    s_tgt_cy = (b->y0 + b->y1) * 0.5f;
    s_last_score = b->score;

    //归一化误差(方向化: 正=该轴角度需增大; e的单位=FOV/2度)
    const float half_fov = s_param[SP_FOV] * 0.5f;
    float e_new[2];
    e_new[0] = clampf(-(s_tgt_cx - src_w * 0.5f) / (src_w * 0.5f), -1, 1);
    e_new[1] = clampf(+(s_tgt_cy - src_h * 0.5f) / (src_h * 0.5f), -1, 1);

    //推理延迟前馈: 检测框是infer_ms前抓的, 期间云台已按当前速度转过去一段
    const float lat_s = clampf(s_snap.infer_ms * 0.001f, 0, 1);
    for (int i = 0; i < 2; i++)
    {
        PidAxis* a = &s_ax[i];

        //目标速度估计(两检测间: e_k = e_{k-1} + v*T - 自转/half_fov)
        if (had_prev)
        {
            float T = (now - s_t_det_prev) * 0.001f;
            T = clampf(T, 0.1f, 1.5f);
            float v_raw = (e_new[i] - a->e_det + a->moved / half_fov) / T;
            a->v_t = clampf(a->v_t * 0.55f + v_raw * 0.45f,
                            -V_EST_MAX, V_EST_MAX);
        }
        else
        {
            a->v_t = 0;             //新目标: 速度历史无效
        }

        a->e_det = e_new[i] - a->u * lat_s / half_fov;
        a->moved = 0;
    }
    s_t_det_prev = now;
}

//PID一步(每50ms, 追踪中且未丢失时): 预测+补偿 -> PID -> 连续小步
static void pid_tick(void)
{
    const float half_fov = s_param[SP_FOV] * 0.5f;
    const float db = s_param[SP_DB] * 0.01f;
    const float vmax = (float)s_param[SP_VMAX];
    const float dt_pred =
        clampf((lv_tick_get() - s_last_det_ms) * 0.001f, 0, PRED_CAP_S);
    float e_eff[2] = { 0, 0 };

    static const ServoCh ch[2] = { SERVO_PAN, SERVO_TILT };
    for (int i = 0; i < 2; i++)
    {
        PidAxis* a = &s_ax[i];

        //有效误差 = 检测值 + 目标运动预测 - 自身转动补偿
        float e = a->e_det + a->v_t * dt_pred - a->moved / half_fov;
        e = clampf(e, -1, 1);
        if (e > -db && e < db)
            e = 0;                          //静差死区
        e_eff[i] = e;

        //PID -> 角速度(度/s)
        const float deriv_raw = (e - a->e_prev) / DT_S;
        a->deriv = a->deriv * 0.7f + deriv_raw * 0.3f;
        if (e != 0)
            a->integ = clampf(a->integ + e * DT_S, -INTEG_MAX, INTEG_MAX);
        else
            a->integ *= 0.9f;               //死区内泄放, 防穿越振荡
        float u = s_param[SP_P] * e
                + s_param[SP_I] * a->integ
                + s_param[SP_D] * a->deriv;
        u = clampf(u, -vmax, vmax);
        a->u = u;

        //连续小步(限速即限步): 角度增量 = u*dt
        const float step = u * DT_S;
        servo_set_angle(ch[i], servo_get_angle(ch[i]) + step);
        a->moved += step;
        a->e_prev = e;
    }

    cursor_set(s_cur_h, -e_eff[0]);         //游标按画面方向(nx=-e_pan)
    cursor_set(s_cur_v, e_eff[1]);
}

//缓慢归中(丢失且"丢失回中"开着; 1步/tick, 到中停)
static void recenter_step(void)
{
    float a = servo_get_angle(SERVO_PAN);
    float b = servo_get_angle(SERVO_TILT);
    if (a < 90.0f) a += RC_STEP_DEG;
    if (a > 90.0f) a -= RC_STEP_DEG;
    if (b < 90.0f) b += RC_STEP_DEG;
    if (b > 90.0f) b -= RC_STEP_DEG;
    if (a > 90.0f - RC_STEP_DEG && a < 90.0f + RC_STEP_DEG) a = 90.0f;
    if (b > 90.0f - RC_STEP_DEG && b < 90.0f + RC_STEP_DEG) b = 90.0f;
    servo_set_angle(SERVO_PAN, a);
    servo_set_angle(SERVO_TILT, b);
}

// -----------------------------------------------------------------------------
// 界面刷新(只读快照)
static void panel_refresh(void)
{
    char buf[48];

    //左栏六行
    lv_label_set_text(s_lbl_line[0], s_snap.cam_on ? "相机 预览" : "相机 未开启");
    if (s_model_idx < 0)
        lv_label_set_text(s_lbl_line[1], "模型 未找到");
    else if (s_snap.model_idx == s_model_idx)
        lv_label_set_text(s_lbl_line[1], "模型 已就绪");
    else
        lv_label_set_text(s_lbl_line[1], "模型 加载中");
    if (s_snap.running)
    {
        lv_snprintf(buf, sizeof(buf), "推理 %ums", s_snap.infer_ms);
        lv_label_set_text(s_lbl_line[2], buf);
    }
    else
        lv_label_set_text(s_lbl_line[2], "推理 停止");
    lv_snprintf(buf, sizeof(buf), "人脸 %u张", (unsigned)s_snap.det_box_n);
    lv_label_set_text(s_lbl_line[3], buf);
    if (s_last_score >= 0)
    {
        lv_snprintf(buf, sizeof(buf), "置信 %d%%", s_last_score);
        lv_label_set_text(s_lbl_line[4], buf);
    }
    else
        lv_label_set_text(s_lbl_line[4], "置信 --");
    lv_label_set_text(s_lbl_line[5],
                      servo_ready() ? "舵机 已连接" : "舵机 未连接");

    //底栏: 角度(右侧滑条区各自刷新, 此处不管)
    lv_snprintf(buf, sizeof(buf), "左右 %u度  上下 %u度",
                (unsigned)(servo_get_angle(SERVO_PAN) + 0.5f),
                (unsigned)(servo_get_angle(SERVO_TILT) + 0.5f));
    lv_label_set_text(s_lbl_angle, buf);

    state_refresh();
}

// -----------------------------------------------------------------------------
// 定时器节拍(gui任务上下文): 控制律 + 启动状态机 + 界面刷新
static void tick_cb(lv_timer_t* t)
{
    (void)t;
    static uint8_t div = 0;
    const uint8_t phase = div++ % 5;         //5拍一轮回: 0-3控制拍, 4状态拍

    //---- 启动状态机(250ms一查: 找模型/等激活/开推理) ----
    if (phase == 4)
    {
        gui_model_snapshot(&s_snap);

        if (s_model_idx < 0)
        {
            s_model_idx = find_face_model(&s_snap);
            if (s_model_idx >= 0)
                post_cmd(APP_CMD_MODEL_SELECT, s_model_idx);
        }
        //相机已开+模型已激活+推理未跑 -> 开推理(只投一次; 连续失败自停
        //后由"开始追踪"按钮重启, 不在此自动拉起)
        if (!s_run_posted && s_snap.cam_on && s_model_idx >= 0 &&
            s_snap.model_idx == s_model_idx && !s_snap.running)
        {
            post_cmd(APP_CMD_RUN_SET, 1);
            s_run_posted = 1;
        }
        panel_refresh();
    }

    //---- 控制律 ----
    if (s_track && servo_ready())
    {
        const uint32_t gen = gui_model_det_gen_fast();
        if (gen != s_last_gen)
        {
            s_last_gen = gen;
            gui_model_snapshot(&s_snap);
            //只消费摄像头帧系的框(照片单次推理的框坐标属于图片, 不跟)
            if (s_snap.running && s_snap.det_src_w > 0)
                control_step(s_snap.det_box, s_snap.det_box_n,
                             s_snap.det_src_w, s_snap.det_src_h);
        }

        //检测停摆兜底(推理挂了/无新框): 视同丢失
        if (!s_lost && lv_tick_get() - s_last_det_ms > STALL_LOST_MS)
        {
            s_lost = 1;
            for (int i = 0; i < 2; i++)
                s_ax[i].v_t = s_ax[i].integ = s_ax[i].deriv = s_ax[i].u = 0;
            state_refresh();
        }

        if (!s_lost)
            pid_tick();                      //连续小步(20Hz)
        else if (s_rc_on &&
                 lv_tick_get() - s_last_det_ms > LOSS_RC_MS)
            recenter_step();                 //丢失回中重新搜索
    }
}

// -----------------------------------------------------------------------------
// 输入
static void track_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (!servo_ready())
    {
        launcher_toast("舵机未连接");
        return;
    }
    s_track = !s_track;
    if (s_track)
    {
        //PID状态清零重开(旧误差/积分/速度估计全部作废)
        memset(s_ax, 0, sizeof(s_ax));
        s_have_tgt = 0;
        s_lost = 0;
        s_last_det_ms = s_t_det_prev = lv_tick_get();
        //推理被失败自停过(或未拉起): 随追踪一起重启
        gui_model_snapshot(&s_snap);
        if (!s_snap.running && s_snap.cam_on && s_model_idx >= 0 &&
            s_snap.model_idx == s_model_idx)
        {
            post_cmd(APP_CMD_RUN_SET, 1);
            s_run_posted = 1;
        }
    }
    track_btn_refresh();
    state_refresh();
}

static void center_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (!servo_ready())
        return;
    servo_center();
    for (int i = 0; i < 2; i++)
        s_ax[i].moved = s_ax[i].integ = 0;   //手动归中: 补偿账本同步作废
    cursor_set(s_cur_h, 0.0f);
    cursor_set(s_cur_v, 0.0f);
}

static void rc_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    s_rc_on = !s_rc_on;
    rc_btn_refresh();
}

static void slide_cb(lv_event_t* e)
{
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id < 0 || id >= SP_N)
        return;
    s_param[id] = lv_slider_get_value(lv_event_get_target(e));
    param_val_refresh(id);
}

static void btn_exit_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    //顺序: 先停推理再关链路(appctl FIFO); 停笔在退场动画前(同app_gimbal)
    s_track = 0;
    post_cmd(APP_CMD_RUN_SET, 0);
    vidwin_enable(0);
    vidwin_task_stop();
    post_cmd(APP_CMD_CAM_CLOSE, 0);
    if (servo_ready())
        servo_center();
    fw_app_exit();                    //之后不得再碰任何LVGL对象
}

// -----------------------------------------------------------------------------
// 参数滑条行(底栏卡内父坐标系): 名字 + 滑条 + 当前值
static void param_row(lv_obj_t* parent, int id, int y)
{
    label(parent, 436, y + 1, 64, s_param_cfg[id].name,
          &font_cn_16, COL_TEXT_DIM);
    lv_obj_t* s = lv_slider_create(parent);
    lv_obj_set_pos(s, 508, y + 1);
    lv_obj_set_size(s, 174, 14);
    lv_slider_set_range(s, s_param_cfg[id].vmin, s_param_cfg[id].vmax);
    lv_slider_set_value(s, s_param_cfg[id].vdef, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(COL_TRACK_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(COL_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);   //旋钮贴轨道尺寸
    lv_obj_add_event_cb(s, slide_cb, LV_EVENT_VALUE_CHANGED,
                        (void*)(intptr_t)id);
    s_lbl_val[id] = label(parent, 690, y + 1, 78, "", &font_cn_16, COL_CYAN);
    s_param[id] = s_param_cfg[id].vdef;
    param_val_refresh(id);
}

void app_main(void)
{
    lv_obj_t* scr = lv_obj_create(0); //应用独占screen
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    //顶栏(y0..44; 视频洞从LCD y48起)
    label(scr, 8, 10, 0, "人脸追踪", &font_cn_20, COL_CYAN);
    s_lbl_state = label(scr, 150, 13, 120, "待机", &font_cn_20, COL_TEXT_DIM);
    lv_obj_t* btn_exit = lv_button_create(scr);
    lv_obj_set_pos(btn_exit, 708, 6);
    lv_obj_set_size(btn_exit, 68, 30);
    lv_obj_t* ex = lv_label_create(btn_exit);
    lv_obj_set_style_text_font(ex, &font_cn_16, 0);
    lv_obj_set_style_text_color(ex, lv_color_white(), 0);
    lv_label_set_text(ex, "退出");
    lv_obj_center(ex);
    lv_obj_add_event_cb(btn_exit, btn_exit_cb, LV_EVENT_CLICKED, 0);

    //左栏状态卡(LCD x8..152; 洞x160起) —— 行文案由panel_refresh覆写
    lv_obj_t* panl = card(scr, PANL_L_X, PANL_L_Y, PANL_L_W, PANL_L_H);
    for (int i = 0; i < 6; i++)
        s_lbl_line[i] = label(panl, 16, 14 + i * 30, PANL_L_W - 24,
                              "--", &font_cn_16, COL_TEXT_DIM);

    //右栏按钮(LCD x648..792; 洞右缘640止)
    s_btn_track = make_btn(scr, BTN_X, 64, BTN_W, 64, 0x1E6E5E,
                           "开始追踪", &font_cn_20, track_cb, 0);
    make_btn(scr, BTN_X, 144, BTN_W, 48, 0x0F5E6E, "回中",
             &font_cn_20, center_cb, 0);
    s_btn_rc = make_btn(scr, BTN_X, 204, BTN_W, 48, 0x2A3450,
                        "丢失回中 开", &font_cn_16, rc_cb, 0);

    //底栏遥测卡(LCD y326..464; 洞底y318止):
    //左侧角度+偏差条, 右侧6个PID参数滑条
    lv_obj_t* bot = card(scr, PANL_B_X, PANL_B_Y, PANL_B_W, PANL_B_H);
    s_lbl_angle = label(bot, 16, 12, 320, "", &font_cn_20, COL_CYAN);
    s_cur_h = err_bar(bot, BAR_RY_H);
    s_cur_v = err_bar(bot, BAR_RY_V);
    label(bot, BAR_RX + BAR_W + 16, BAR_RY_H - 4, 0,
          "水平偏差", &font_cn_16, COL_TEXT_DIM);
    label(bot, BAR_RX + BAR_W + 16, BAR_RY_V - 4, 0,
          "垂直偏差", &font_cn_16, COL_TEXT_DIM);
    for (int id = 0; id < SP_N; id++)
        param_row(bot, id, 6 + id * 20);

    //初始状态(static每次启动被清零, 此处只对齐当前系统态)
    s_last_gen = gui_model_det_gen_fast();     //丢弃历史det(上一会话残留)
    s_last_det_ms = s_t_det_prev = lv_tick_get();
    track_btn_refresh();
    rc_btn_refresh();

    vidwin_align_left(0);                  //居中开窗(与本布局镜像)
    vidwin_enable(1);                      //上一会话on_close停过笔
    post_cmd(APP_CMD_CAM_OPEN, 0);         //进应用即开摄像头(异步, 经gen_cam)

    //模型清单是开机快照: 直接全量拉一次找人脸模型(没有则兜底重扫)
    gui_model_snapshot(&s_snap);
    s_model_idx = find_face_model(&s_snap);
    if (s_model_idx >= 0)
        post_cmd(APP_CMD_MODEL_SELECT, s_model_idx);
    else
        post_cmd(APP_CMD_MODELS_RESCAN, 0);

    panel_refresh();
    if (!servo_ready())
        launcher_toast("舵机未连接");

    lv_screen_load(scr);                   //应用自加载其screen(契约)

    s_timer = app_timer_create(tick_cb, TICK_MS, 0);
}

//恒定入口(APP_LOAD_BASE, 见tools/sdapp/app.ld)
__attribute__((section(".app_entry_first")))
void app_boot(void)
{
    app_main();
}
