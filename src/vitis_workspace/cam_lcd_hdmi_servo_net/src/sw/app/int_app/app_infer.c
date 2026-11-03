// =============================================================================
// app_infer —— 推理应用(内置): ncnn摄像头实时推理 + SD图片单次推理
//
// 四个互斥面板(布局移植自17工程上板验证版):
//   实时   视频洞480x270靠左(检测框由vid任务平滑叠加) + 右栏(模型卡/
//          按钮组/耗时趋势卡) + 左下结果卡; 进入即开摄像头链路
//   图片   /photos目录BMP列表(复用FS_LIST与相册行样式), 点行进查看
//   查看   整幅图(LVGL显示) + 检测框为LVGL控件叠加(CONTAIN等比换算) +
//          [推理]单次触发 + 结果行; 图片模式不依赖摄像头
//   模型   /models清单(名字+模块自述tag, 当前项高亮), 点击加载, 带刷新
//
// 检测框出口: 洞内平滑框(vid任务, IoU配对+插值)/ 图片查看框(LVGL控件,
// 完美对位)/ HDMI描框(三态: 关/开无框/开带框, vidwin_hdmi_ovl_set)
// 预览开关: 开=30fps直播; 关=纯黑屏加速(洞只留切换时涂的底色, 推理无头
//   跑, 界面数值/趋势/结果卡照常更新)
//
// 数据流(FatFs单线程纪律, GUI不碰SD):
//   开始/停止: RUN_SET -> app_task推理回泵(每帧: 抓拍进0x11100000工作缓存
//             -> nn_infer -> bridge发布 -> vidwin叠加)
//   切模型:   MODEL_SELECT(行点击无条件发, 后端幂等) -> nn_model_activate
//             -> pub_model; 自检/重扫后由app_infer_pub_model_state对齐
//   重扫:     MODELS_RESCAN -> nn_models_scan -> pub_models
//   图片:     PHOTO_LOAD/UNLOAD复用相机应用管道; PHOTO_INFER对当前图单次推理
//   结果:     gen_infer/gen_res/gen_det驱动模型卡数值/结果卡行/查看器框叠加
//
// 视频洞几何(与vidwin.c一致, 两处必须镜像): 本应用开窗靠左
// (vidwin_align_left), 洞=480x270 @内容区(0,0); 右栏x492宽292:
// 模型卡(0..140) / 按钮组(148..296) / 耗时趋势卡(304..424);
// 视频正下方为结果卡(282..416): 每目标一行(色点+类别+置信度条)
// =============================================================================

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "lvgl.h"

#include "sw/app/launcher.h"
#include "sw/app/bridge.h"
#include "sw/app/appctl.h"
#include "sw/impl/lcd/lvgl_port.h"
#include "sw/impl/lcd/vidwin.h"
#include "sw/impl/lcd/ovl_draw.h"

#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF
#define COL_BTN       0x2A3450
#define COL_ACCENT    0x0F5E6E      //开始/推理按钮(青)

#define PHOTOS_DIR    "0:/photos"

//内容区几何(800x480屏, 顶栏40+边距16)
#define CW            784
#define CH            424

//面板号
enum { MODE_LIVE = 0, MODE_IMGS, MODE_VIEW, MODE_MODELS };

//控件引用
static lv_obj_t* s_live;             //四个互斥面板
static lv_obj_t* s_imgs;
static lv_obj_t* s_view;
static lv_obj_t* s_models;
//右栏模型卡: 名+徽标+三行统计(单帧耗时/推理帧率/目标总数|Top1)
static lv_obj_t* s_mc_name;
static lv_obj_t* s_mc_badge;
static lv_obj_t* s_mc_key3;          //第三行键(检测=目标总数 分类=Top1)
static lv_obj_t* s_mc_val[3];        //三行值
//左下结果卡: 每目标一行(色点+类别名+置信度条+百分数)
#define RES_ROWS  3
static lv_obj_t* s_res_row[RES_ROWS];
static lv_obj_t* s_res_dot[RES_ROWS];
static lv_obj_t* s_res_name[RES_ROWS];
static lv_obj_t* s_res_fill[RES_ROWS];
static lv_obj_t* s_res_score[RES_ROWS];
static lv_obj_t* s_res_none;         //空态("暂无结果")
//右下耗时趋势卡: TREND_N根柱(桥hist末段) + 均值/峰值说明
#define TREND_N   33
#define TREND_H   60                 //柱最大高(像素)
static lv_obj_t* s_trend_bar[TREND_N];
static lv_obj_t* s_lbl_trend_cap;
static char s_res_buf[RES_ROWS][8];  //分数文本
static char s_trend_buf[32];         //均值/峰值文本
static lv_obj_t* s_lbl_run;          //开始/停止按钮文本
static lv_obj_t* s_lbl_prev;         //预览按钮文本
static lv_obj_t* s_lbl_hdmi;         //HDMI按钮文本(三态)
static lv_obj_t* s_smooth;           //跟手速度滑块(gui直调vidwin, 实时生效)
static lv_obj_t* s_lbl_smooth;       //滑块当前值("60%")
static lv_obj_t* s_imgs_list;        //图片列表容器
static lv_obj_t* s_imgs_state;       //图片列表状态行
static lv_obj_t* s_img;              //查看: 大图
static lv_obj_t* s_view_boxes;       //查看: 检测框控件层(在建好的图之上)
static lv_obj_t* s_lbl_vres;         //查看: 结果行
static lv_obj_t* s_models_list;      //模型列表容器
static lv_obj_t* s_models_state;     //模型列表状态行
static lv_image_dsc_t s_dsc;         //查看: 图像描述

static GuiSnapshot s_snap;
static uint32_t   s_last_cam, s_last_cap, s_last_fs, s_last_photo,
                  s_last_model, s_last_infer, s_last_res, s_last_run,
                  s_last_models, s_last_det;
static int        s_mode;            //MODE_xxx
static int        s_prev_on = 1;     //预览开关(0=标注回放加速模式)
static int        s_hdmi;            //HDMI三态: 0关 1开无框 2开带框
//实时面板在场标志(gui写, app_task回泵读: 非实时面板时不得直写视频洞)
static volatile int s_gui_live;

// -----------------------------------------------------------------------------
static void post_cmd(int type, int val, const char* path)
{
    AppCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint8_t)type;
    cmd.val = val;
    if (path)
    {
        strncpy(cmd.path, path, sizeof(cmd.path) - 1);
        cmd.path[sizeof(cmd.path) - 1] = 0;
    }
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
                          const lv_font_t* font)
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
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, 0);
    return b;
}

// -----------------------------------------------------------------------------
// 面板切换(互斥可见); 实时面板且预览开时视频窗才写帧
static void show_panel(int mode)
{
    s_mode = mode;
    lv_obj_add_flag(s_live, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_imgs, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_models, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* p = (mode == MODE_LIVE) ? s_live
                 : (mode == MODE_IMGS) ? s_imgs
                 : (mode == MODE_VIEW) ? s_view : s_models;
    lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
    s_gui_live = (mode == MODE_LIVE);
    vidwin_enable(s_gui_live && s_prev_on);
}

// -----------------------------------------------------------------------------
// 实时面板: 右栏模型卡 / 左下结果卡 / 右下趋势卡

static void model_card_refresh(void)
{
    if (s_snap.model_idx < 0)
    {
        lv_label_set_text(s_mc_name, "未选模型");
        lv_obj_add_flag(s_mc_badge, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mc_key3, "目标总数");
        lv_label_set_text(s_mc_val[0], "-");
        lv_label_set_text(s_mc_val[1], "-");
        lv_label_set_text(s_mc_val[2], "-");
        return;
    }
    lv_obj_remove_flag(s_mc_badge, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_mc_name, s_snap.model_name);
    lv_label_set_text(s_mc_badge,
                      s_snap.model_type[0] ? s_snap.model_type : "模型");

    char buf[24];
    if (s_snap.infer_ms > 0)
        lv_snprintf(buf, sizeof(buf), "%ums", (unsigned)s_snap.infer_ms);
    else
        lv_snprintf(buf, sizeof(buf), "-");
    lv_label_set_text(s_mc_val[0], buf);

    if (s_snap.running && s_snap.infer_ms > 0)
        lv_snprintf(buf, sizeof(buf), "%ufps",
                    (unsigned)(1000u / s_snap.infer_ms));
    else
        lv_snprintf(buf, sizeof(buf), "-");
    lv_label_set_text(s_mc_val[1], buf);

    if (s_snap.post_type == 1)
    {
        lv_label_set_text(s_mc_key3, "目标总数");
        lv_snprintf(buf, sizeof(buf), "%d", s_snap.det_total);
    }
    else
    {
        lv_label_set_text(s_mc_key3, "Top1");
        if (s_snap.topk_n > 0)
            lv_snprintf(buf, sizeof(buf), "%d%%", s_snap.topk[0].score);
        else
            lv_snprintf(buf, sizeof(buf), "-");
    }
    lv_label_set_text(s_mc_val[2], buf);
}

//结果卡: 检测=前3目标(类别色) 分类=Top3; 无结果显示空态
static void result_rows_refresh(void)
{
    int n = 0;
    if (s_snap.post_type == 1)
    {
        n = s_snap.det_box_n;
        if (n > RES_ROWS) n = RES_ROWS;
        for (int i = 0; i < n; i++)
        {
            const uint32_t col = ovl_cls_color(s_snap.det_box[i].cls);
            lv_obj_set_style_bg_color(s_res_dot[i], lv_color_hex(col), 0);
            lv_label_set_text(s_res_name[i], s_snap.det_box[i].name);
            lv_obj_set_style_bg_color(s_res_fill[i], lv_color_hex(col), 0);
            lv_obj_set_width(s_res_fill[i],
                             s_snap.det_box[i].score * 230 / 100);
            lv_snprintf(s_res_buf[i], sizeof(s_res_buf[i]), "%d%%",
                        s_snap.det_box[i].score);
            lv_label_set_text(s_res_score[i], s_res_buf[i]);
        }
    }
    else
    {
        n = s_snap.topk_n;
        if (n > RES_ROWS) n = RES_ROWS;
        for (int i = 0; i < n; i++)
        {
            const uint32_t col = ovl_cls_color(i);
            lv_obj_set_style_bg_color(s_res_dot[i], lv_color_hex(col), 0);
            lv_label_set_text(s_res_name[i], s_snap.topk[i].name);
            lv_obj_set_style_bg_color(s_res_fill[i], lv_color_hex(col), 0);
            lv_obj_set_width(s_res_fill[i],
                             s_snap.topk[i].score * 230 / 100);
            lv_snprintf(s_res_buf[i], sizeof(s_res_buf[i]), "%d%%",
                        s_snap.topk[i].score);
            lv_label_set_text(s_res_score[i], s_res_buf[i]);
        }
    }

    for (int i = 0; i < RES_ROWS; i++)
    {
        if (i < n)
            lv_obj_remove_flag(s_res_row[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_res_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (n == 0)
        lv_obj_remove_flag(s_res_none, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_res_none, LV_OBJ_FLAG_HIDDEN);
}

//趋势卡: 桥hist末段33根柱(旧->新, 最新在右) + 均值/峰值说明
static void trend_refresh(void)
{
    unsigned peak = 0;
    for (int i = 0; i < s_snap.hist_n; i++)
        if (s_snap.hist[i] > peak)
            peak = s_snap.hist[i];
    if (peak == 0)
        peak = 1;

    for (int i = 0; i < TREND_N; i++)
    {
        //i=0最旧: 从hist取末段(下标向最新靠拢); 数据不足的柱给最小高度
        const int src = s_snap.hist_n - TREND_N + i;
        const unsigned v = (src >= 0) ? s_snap.hist[src] : 0;
        const int h = 3 + (int)((unsigned)TREND_H * v / peak);
        lv_obj_set_height(s_trend_bar[i], h);
        lv_obj_set_y(s_trend_bar[i], 84 - h);   //柱基线(卡高96, 底缘留12)
    }
    lv_snprintf(s_trend_buf, sizeof(s_trend_buf), "均值%ums 峰值%ums",
                (unsigned)s_snap.avg_ms, (unsigned)s_snap.peak_ms);
    lv_label_set_text(s_lbl_trend_cap, s_trend_buf);
}

//查看面板结果行文本: fps/目标摘要 + 前几项 + 耗时
//(框源尺寸与当前图不符时不列目标——那是摄像头帧的结果)
static void result_line(char* buf, unsigned cap)
{
    buf[0] = 0;
    const int dims_ok = (s_snap.det_src_w == s_snap.photo_w
                         && s_snap.det_src_h == s_snap.photo_h);
    if (s_snap.post_type == 1)
    {
        if (dims_ok && s_snap.infer_ms > 0)
            lv_snprintf(buf, cap, "%ufps 目标%d  ",
                        (unsigned)(1000u / s_snap.infer_ms),
                        s_snap.det_total);
        int n = dims_ok ? s_snap.det_box_n : 0;
        if (n > 3) n = 3;
        for (int i = 0; i < n; i++)
            lv_snprintf(buf + strlen(buf), cap - strlen(buf), "%s %d%%  ",
                        s_snap.det_box[i].name, s_snap.det_box[i].score);
        if (n == 0 && s_snap.infer_ms == 0)
            lv_snprintf(buf, cap, "无目标");
    }
    else
    {
        int n = s_snap.topk_n;
        if (n > 3) n = 3;
        for (int i = 0; i < n; i++)
            lv_snprintf(buf + strlen(buf), cap - strlen(buf), "%s %d%%  ",
                        s_snap.topk[i].name, s_snap.topk[i].score);
    }
}

//查看面板: 检测框为LVGL控件(CONTAIN等比换算, 完美对位)
static void view_boxes_rebuild(void)
{
    lv_obj_clean(s_view_boxes);
    if (s_mode != MODE_VIEW || !s_snap.photo_pix || s_snap.det_box_n == 0)
        return;
    //框必须属于当前查看的图(摄像头推理的框坐标不匹配则不画)
    if (s_snap.det_src_w != s_snap.photo_w || s_snap.det_src_h != s_snap.photo_h)
        return;

    const int pw = s_snap.photo_w;
    const int ph = s_snap.photo_h;
    if (pw <= 0 || ph <= 0)
        return;
    int s1000 = CW * 1000 / pw;
    const int sh1000 = CH * 1000 / ph;
    if (sh1000 < s1000) s1000 = sh1000;
    const int ox = (CW - pw * s1000 / 1000) / 2;
    const int oy = (CH - ph * s1000 / 1000) / 2;

    int n = s_snap.det_box_n;
    if (n > GUI_DET_BOX_MAX) n = GUI_DET_BOX_MAX;
    for (int i = 0; i < n; i++)
    {
        const GuiDetBox* b = &s_snap.det_box[i];
        const int bx0 = ox + b->x0 * s1000 / 1000;
        const int by0 = oy + b->y0 * s1000 / 1000;
        const int bx1 = ox + b->x1 * s1000 / 1000;
        const int by1 = oy + b->y1 * s1000 / 1000;
        const uint32_t col = ovl_cls_color(b->cls);

        lv_obj_t* r = lv_obj_create(s_view_boxes);
        lv_obj_remove_style_all(r);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(r, bx0, by0);
        lv_obj_set_size(r, bx1 - bx0 + 1, by1 - by0 + 1);
        lv_obj_set_style_border_color(r, lv_color_hex(col), 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_set_style_radius(r, 0, 0);

        char txt[24];
        lv_snprintf(txt, sizeof(txt), "%s %d%%", b->name, b->score);
        lv_obj_t* t = lv_label_create(s_view_boxes);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(t, &font_cn_16, 0);
        lv_obj_set_style_text_color(t, lv_color_white(), 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(col), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(t, 4, 0);
        lv_obj_set_style_pad_ver(t, 2, 0);
        lv_label_set_text(t, txt);
        lv_obj_set_pos(t, bx0 + 2, by0 + 2);     //标签条画在框内左上角(同视频框)
    }
}

static void view_result_refresh(void)
{
    if (!s_snap.photo_pix)
    {
        lv_label_set_text(s_lbl_vres, "");
        return;
    }
    char buf[128];
    result_line(buf, sizeof(buf));
    if (s_snap.infer_ms > 0)
        lv_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "  %ums",
                    (unsigned)s_snap.infer_ms);
    lv_label_set_text(s_lbl_vres, buf);
}

// -----------------------------------------------------------------------------
// 实时面板按钮
static void run_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    post_cmd(APP_CMD_RUN_SET, s_snap.running ? 0 : 1, 0);
}

static void model_open_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    show_panel(MODE_MODELS);
}

static void imgs_open_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    show_panel(MODE_IMGS);
    lv_obj_remove_flag(s_imgs_state, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_imgs_state, "读取中…");
    lv_obj_clean(s_imgs_list);
    post_cmd(APP_CMD_FS_LIST, 0, PHOTOS_DIR);
}

static void prev_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    s_prev_on = !s_prev_on;
    vidwin_enable(s_mode == MODE_LIVE && s_prev_on);
    lv_label_set_text(s_lbl_prev, s_prev_on ? "预览 开" : "预览 关");
    if (!s_prev_on)
        vidwin_paint_bg();          //遮掉冻结残帧(推理回放会重新喂数据)
}

static void hdmi_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    //三态: 关 -> 开(无框) -> 开(带框) -> 关
    s_hdmi = (s_hdmi + 1) % 3;
    vidwin_hdmi_ovl_set(s_hdmi == 2);
    post_cmd(APP_CMD_HDMI_SET, s_hdmi != 0, 0);
    lv_label_set_text(s_lbl_hdmi,
                      s_hdmi == 0 ? "HDMI 关"
                      : s_hdmi == 1 ? "HDMI 开" : "HDMI 检测");
}

//检测框跟手速度: 滑块直调vidwin(每视频帧向目标靠近的百分比, 10~100)
static void smooth_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    const int v = lv_slider_get_value(s_smooth);
    vidwin_ovl_smooth_set(v);
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_lbl_smooth, buf);
}

// -----------------------------------------------------------------------------
// 图片列表
static void img_row_cb(lv_event_t* e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_snap.fs_n || s_snap.fs[idx].is_dir)
        return;
    char path[GUI_FS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", PHOTOS_DIR, s_snap.fs[idx].name);
    post_cmd(APP_CMD_PHOTO_LOAD, 0, path);      //gen_photo回来后进查看
}

static void img_back_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    show_panel(MODE_LIVE);
}

//查看面板
static void view_infer_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (s_snap.model_idx < 0)
    {
        launcher_toast("未选模型");
        return;
    }
    post_cmd(APP_CMD_PHOTO_INFER, 0, 0);
}

static void view_back_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    post_cmd(APP_CMD_PHOTO_UNLOAD, 0, 0);       //gen_photo=0时refresh切回列表
}

//模型面板: 点击无条件发激活命令(后端幂等并总是发布, 桥状态保持对齐)
static void model_row_cb(lv_event_t* e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_snap.n_models)
        return;
    post_cmd(APP_CMD_MODEL_SELECT, idx, 0);     //加载结果经gen_model
    show_panel(MODE_LIVE);
}

static void model_rescan_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    lv_label_set_text(s_models_state, "读取中…");
    lv_obj_remove_flag(s_models_state, LV_OBJ_FLAG_HIDDEN);
    post_cmd(APP_CMD_MODELS_RESCAN, 0, 0);
}

static void model_back_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    show_panel(MODE_LIVE);
}

// -----------------------------------------------------------------------------
// 图片列表重建(照搬相机应用相册行样式)
static void imgs_rebuild(void)
{
    lv_obj_clean(s_imgs_list);

    if (s_snap.fs_err != 0)
    {
        lv_label_set_text(s_imgs_state,
                          s_snap.fs_err == -1 ? "SD卡未挂载" : "目录打开失败");
        lv_obj_remove_flag(s_imgs_state, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_snap.fs_n == 0)
    {
        lv_label_set_text(s_imgs_state, "暂无图片");
        lv_obj_remove_flag(s_imgs_state, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_imgs_state, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < s_snap.fs_n; i++)
    {
        const GuiFsEntry* en = &s_snap.fs[i];

        lv_obj_t* row = lv_button_create(s_imgs_list);
        lv_obj_set_size(row, lv_pct(100), 40);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_CARD_EDGE), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_add_event_cb(row, img_row_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);

        lv_obj_t* ic = lv_label_create(row);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text(ic, LV_SYMBOL_IMAGE);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t* nm = lv_label_create(row);
        lv_obj_set_width(nm, 560);
        lv_obj_set_height(nm, lv_pct(100));
        lv_obj_set_style_text_font(nm, &font_cn_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_white(), 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, en->name);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 30, 0);

        char sz[16];
        if (en->size >= 1024u * 1024u)
            lv_snprintf(sz, sizeof(sz), "%u.%uMB", en->size >> 20,
                        (unsigned)((en->size & 0xFFFFFu) * 10u >> 20));
        else
            lv_snprintf(sz, sizeof(sz), "%uKB", (en->size + 1023u) >> 10);
        lv_obj_t* sl = lv_label_create(row);
        lv_obj_set_width(sl, 90);
        lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sl, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text(sl, sz);
        lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -4, 0);
    }
}

//模型列表重建(tag=模块自述类型标签, 原样显示)
static void models_rebuild(void)
{
    lv_obj_clean(s_models_list);

    if (s_snap.n_models == 0)
    {
        lv_label_set_text(s_models_state, "SD卡/models无模型");
        lv_obj_remove_flag(s_models_state, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_models_state, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < s_snap.n_models; i++)
    {
        const int cur = (i == s_snap.model_idx);

        lv_obj_t* row = lv_button_create(s_models_list);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(cur ? 0x1B2A45
                                                        : COL_CARD), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(cur ? COL_CYAN
                                                            : COL_CARD_EDGE), 0);
        lv_obj_set_style_border_width(row, cur ? 2 : 1, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_add_event_cb(row, model_row_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);

        lv_obj_t* nm = lv_label_create(row);
        lv_obj_set_width(nm, 460);
        lv_obj_set_height(nm, lv_pct(100));
        lv_obj_set_style_text_font(nm, &font_cn_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_white(), 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, s_snap.models_name[i]);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t* tag = lv_label_create(row);
        lv_obj_set_style_text_font(tag, &font_cn_16, 0);
        lv_obj_set_style_text_color(tag, lv_color_hex(COL_TEXT_DIM), 0);
        lv_label_set_text(tag, s_snap.models_tag[i]);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -36, 0);

        if (cur)
        {
            lv_obj_t* ok = lv_label_create(row);
            lv_obj_set_style_text_font(ok, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(ok, lv_color_hex(COL_CYAN), 0);
            lv_label_set_text(ok, LV_SYMBOL_OK);
            lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -6, 0);
        }
    }
}

// -----------------------------------------------------------------------------
static void live_build(lv_obj_t* root)
{
    s_live = place(root, 0, 0, CW, CH);
    lv_obj_clear_flag(s_live, LV_OBJ_FLAG_SCROLLABLE);
    //视频洞(0,0,480x270, 靠左)内不放任何控件——vidwin任务每帧直写该区

    //---- 左下: 结果卡(0,282,480,134) ----
    lv_obj_t* rc = lv_obj_create(s_live);
    lv_obj_set_pos(rc, 0, 282);
    lv_obj_set_size(rc, 480, 134);
    lv_obj_set_style_bg_color(rc, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(rc, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(rc, 1, 0);
    lv_obj_set_style_radius(rc, 10, 0);
    lv_obj_set_style_pad_all(rc, 0, 0);      //默认主题自带内边距, 必须清零
    lv_obj_clear_flag(rc, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* rt = lv_label_create(rc);
    lv_obj_set_pos(rt, 14, 10);
    lv_obj_set_style_text_font(rt, &font_cn_16, 0);
    lv_obj_set_style_text_color(rt, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(rt, "检测结果");

    for (int i = 0; i < RES_ROWS; i++)
    {
        const int ry = 34 + i * 32;          //行顶(card相对)
        s_res_row[i] = place(rc, 0, ry, 456, 22);
        lv_obj_remove_flag(s_res_row[i], LV_OBJ_FLAG_CLICKABLE);

        s_res_dot[i] = place(s_res_row[i], 14, 6, 10, 10);
        lv_obj_set_style_radius(s_res_dot[i], 5, 0);
        lv_obj_set_style_bg_color(s_res_dot[i], lv_color_hex(COL_CYAN), 0);
        lv_obj_set_style_bg_opa(s_res_dot[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_res_dot[i], LV_OBJ_FLAG_CLICKABLE);

        s_res_name[i] = lv_label_create(s_res_row[i]);
        lv_obj_set_pos(s_res_name[i], 34, 0);
        lv_obj_set_width(s_res_name[i], 128);
        lv_label_set_long_mode(s_res_name[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_res_name[i], &font_cn_16, 0);
        lv_obj_set_style_text_color(s_res_name[i], lv_color_white(), 0);
        lv_label_set_text(s_res_name[i], "");

        lv_obj_t* track = place(s_res_row[i], 170, 7, 230, 8);
        lv_obj_set_style_radius(track, 4, 0);
        lv_obj_set_style_bg_color(track, lv_color_hex(0x202A44), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

        s_res_fill[i] = place(track, 0, 0, 0, 8);
        lv_obj_set_style_radius(s_res_fill[i], 4, 0);
        lv_obj_set_style_bg_color(s_res_fill[i], lv_color_hex(COL_CYAN), 0);
        lv_obj_set_style_bg_opa(s_res_fill[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_res_fill[i], LV_OBJ_FLAG_CLICKABLE);

        s_res_score[i] = lv_label_create(s_res_row[i]);
        lv_obj_set_pos(s_res_score[i], 408, 0);
        lv_obj_set_width(s_res_score[i], 38);
        lv_obj_set_style_text_align(s_res_score[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(s_res_score[i], &font_cn_16, 0);
        lv_obj_set_style_text_color(s_res_score[i], lv_color_white(), 0);
        lv_label_set_text(s_res_score[i], "");
    }

    s_res_none = lv_label_create(rc);
    lv_obj_set_pos(s_res_none, 0, 56);
    lv_obj_set_width(s_res_none, 480);
    lv_obj_set_style_text_align(s_res_none, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_res_none, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_res_none, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_res_none, "暂无结果");

    //---- 右栏: 模型卡(492,0,292,140) ----
    lv_obj_t* mc = lv_obj_create(s_live);
    lv_obj_set_pos(mc, 492, 0);
    lv_obj_set_size(mc, 292, 140);
    lv_obj_set_style_bg_color(mc, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(mc, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(mc, 1, 0);
    lv_obj_set_style_radius(mc, 10, 0);
    lv_obj_set_style_pad_all(mc, 0, 0);
    lv_obj_clear_flag(mc, LV_OBJ_FLAG_SCROLLABLE);

    s_mc_name = lv_label_create(mc);
    lv_obj_set_pos(s_mc_name, 14, 12);
    lv_obj_set_width(s_mc_name, 190);
    lv_label_set_long_mode(s_mc_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_mc_name, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_mc_name, lv_color_hex(COL_CYAN), 0);
    lv_label_set_text(s_mc_name, "未选模型");

    s_mc_badge = lv_label_create(mc);
    lv_obj_set_pos(s_mc_badge, 226, 10);
    lv_obj_set_width(s_mc_badge, 52);
    lv_obj_set_style_text_align(s_mc_badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_mc_badge, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_mc_badge, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_bg_color(s_mc_badge, lv_color_hex(0x123A45), 0);
    lv_obj_set_style_bg_opa(s_mc_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_mc_badge, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_border_width(s_mc_badge, 1, 0);
    lv_obj_set_style_radius(s_mc_badge, 11, 0);
    lv_obj_set_style_pad_ver(s_mc_badge, 2, 0);
    lv_label_set_text(s_mc_badge, "");

    lv_obj_t* hr = place(mc, 14, 42, 264, 1);
    lv_obj_set_style_bg_color(hr, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(hr, LV_OBJ_FLAG_CLICKABLE);

    static const char* const keys[3] = { "单帧耗时", "推理帧率", "目标总数" };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t* k = lv_label_create(mc);
        lv_obj_set_pos(k, 14, 54 + i * 26);
        lv_obj_set_style_text_font(k, &font_cn_16, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(COL_TEXT_DIM), 0);
        if (i == 2)
            s_mc_key3 = k;                    //第三行键随模型类型切换
        else
            lv_label_set_text(k, keys[i]);

        s_mc_val[i] = lv_label_create(mc);
        lv_obj_set_pos(s_mc_val[i], 14, 54 + i * 26);
        lv_obj_set_width(s_mc_val[i], 264);
        lv_obj_set_style_text_align(s_mc_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(s_mc_val[i], &font_cn_16, 0);
        lv_obj_set_style_text_color(s_mc_val[i], lv_color_white(), 0);
        lv_label_set_text(s_mc_val[i], "-");
    }

    //---- 右栏: 按钮组 ----
    lv_obj_t* b_run = make_btn(s_live, 492, 148, 292, 44, COL_ACCENT, "开始",
                               run_cb, &font_cn_20);
    s_lbl_run = (lv_obj_t*)lv_obj_get_child(b_run, 0);

    make_btn(s_live, 492, 200, 142, 44, COL_BTN, "模型",
             model_open_cb, 0);
    make_btn(s_live, 642, 200, 142, 44, COL_BTN, "图片",
             imgs_open_cb, 0);

    lv_obj_t* b_prev = make_btn(s_live, 492, 252, 142, 44, COL_BTN,
                                "预览 开", prev_cb, 0);
    s_lbl_prev = (lv_obj_t*)lv_obj_get_child(b_prev, 0);

    lv_obj_t* b_hdmi = make_btn(s_live, 642, 252, 142, 44, COL_BTN,
                                "HDMI 关", hdmi_cb, 0);
    s_lbl_hdmi = (lv_obj_t*)lv_obj_get_child(b_hdmi, 0);

    //---- 右下: 耗时趋势卡(492,304,292,96; 压缩24px给底下跟手滑块行) ----
    lv_obj_t* tc = lv_obj_create(s_live);
    lv_obj_set_pos(tc, 492, 304);
    lv_obj_set_size(tc, 292, 96);
    lv_obj_set_style_bg_color(tc, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(tc, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(tc, 1, 0);
    lv_obj_set_style_radius(tc, 10, 0);
    lv_obj_set_style_pad_all(tc, 0, 0);
    lv_obj_clear_flag(tc, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tt = lv_label_create(tc);
    lv_obj_set_pos(tt, 14, 8);
    lv_obj_set_style_text_font(tt, &font_cn_16, 0);
    lv_obj_set_style_text_color(tt, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(tt, "耗时趋势");

    s_lbl_trend_cap = lv_label_create(tc);
    lv_obj_set_pos(s_lbl_trend_cap, 14, 8);
    lv_obj_set_width(s_lbl_trend_cap, 264);
    lv_obj_set_style_text_align(s_lbl_trend_cap, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_lbl_trend_cap, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_trend_cap, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_trend_cap, "");

    //33根柱(8px宽0间距, 底边y=84), 高度按峰值归一(gen_infer时更新)
    for (int i = 0; i < TREND_N; i++)
    {
        s_trend_bar[i] = place(tc, 14 + i * 8, 81, 8, 3);
        lv_obj_set_style_bg_color(s_trend_bar[i], lv_color_hex(0x2FBCD4), 0);
        lv_obj_set_style_bg_opa(s_trend_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_trend_bar[i], 1, 0);
        lv_obj_remove_flag(s_trend_bar[i], LV_OBJ_FLAG_CLICKABLE);
    }

    //---- 右底: 跟手速度滑块(492,404,292,20; gui直调vidwin实时生效) ----
    lv_obj_t* sl = lv_label_create(s_live);
    lv_obj_set_pos(sl, 492, 405);
    lv_obj_set_style_text_font(sl, &font_cn_16, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(sl, "速度");

    s_smooth = lv_slider_create(s_live);
    lv_obj_set_pos(s_smooth, 534, 407);
    lv_obj_set_size(s_smooth, 196, 14);
    lv_slider_set_range(s_smooth, 10, 100);
    lv_slider_set_value(s_smooth, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_smooth, lv_color_hex(0x202A44), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_smooth, lv_color_hex(COL_CYAN),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_smooth, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_smooth, 0, LV_PART_KNOB);   //旋钮贴轨道尺寸
    lv_obj_add_event_cb(s_smooth, smooth_cb, LV_EVENT_VALUE_CHANGED, 0);

    s_lbl_smooth = lv_label_create(s_live);
    lv_obj_set_pos(s_lbl_smooth, 736, 405);
    lv_obj_set_width(s_lbl_smooth, 48);
    lv_obj_set_style_text_align(s_lbl_smooth, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_lbl_smooth, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_smooth, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_smooth, "60%");
}

static void imgs_build(lv_obj_t* root)
{
    s_imgs = place(root, 0, 0, CW, CH);
    lv_obj_clear_flag(s_imgs, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(s_imgs);
    lv_obj_set_pos(title, 8, 8);
    lv_obj_set_style_text_font(title, &font_cn_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "图片");

    make_btn(s_imgs, CW - 118, 2, 110, 36, COL_BTN, "返回",
             img_back_cb, 0);

    s_imgs_state = lv_label_create(s_imgs);
    lv_obj_set_pos(s_imgs_state, 8, 48);
    lv_obj_set_style_text_font(s_imgs_state, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_imgs_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_imgs_state, "读取中…");

    s_imgs_list = lv_obj_create(s_imgs);
    lv_obj_set_pos(s_imgs_list, 0, 44);
    lv_obj_set_size(s_imgs_list, CW, CH - 44);
    lv_obj_set_style_bg_opa(s_imgs_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_imgs_list, 0, 0);
    lv_obj_set_style_pad_all(s_imgs_list, 0, 0);
    lv_obj_set_flex_flow(s_imgs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_imgs_list, 4, 0);
    lv_obj_set_scrollbar_mode(s_imgs_list, LV_SCROLLBAR_MODE_AUTO);
}

static void view_build(lv_obj_t* root)
{
    s_view = place(root, 0, 0, CW, CH);
    lv_obj_clear_flag(s_view, LV_OBJ_FLAG_SCROLLABLE);

    s_img = lv_image_create(s_view);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_set_size(s_img, CW, CH);
    //CONTAIN: 等比缩放到最大可完整显示并居中(框控件按同参数换算坐标)
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CONTAIN);

    //检测框控件层(先建, 按钮后建压其上; 不吞触摸)
    s_view_boxes = place(s_view, 0, 0, CW, CH);
    lv_obj_remove_flag(s_view_boxes, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* back = lv_button_create(s_view);
    lv_obj_set_pos(back, 4, 4);
    lv_obj_set_size(back, 64, 36);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_t* bi = lv_label_create(back);
    lv_obj_set_style_text_font(bi, &lv_font_montserrat_16, 0);
    lv_label_set_text(bi, LV_SYMBOL_LEFT);
    lv_obj_center(bi);
    lv_obj_add_event_cb(back, view_back_cb, LV_EVENT_CLICKED, 0);

    make_btn(s_view, CW / 2 - 60, 4, 120, 40, COL_ACCENT, "推理",
             view_infer_cb, &font_cn_20);

    s_lbl_vres = lv_label_create(s_view);
    lv_obj_set_pos(s_lbl_vres, 8, CH - 30);
    lv_obj_set_width(s_lbl_vres, CW - 16);
    lv_obj_set_style_text_font(s_lbl_vres, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_vres, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_bg_color(s_lbl_vres, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lbl_vres, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_lbl_vres, 8, 0);
    lv_label_set_long_mode(s_lbl_vres, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_vres, "");
}

static void models_build(lv_obj_t* root)
{
    s_models = place(root, 0, 0, CW, CH);
    lv_obj_clear_flag(s_models, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(s_models);
    lv_obj_set_pos(title, 8, 8);
    lv_obj_set_style_text_font(title, &font_cn_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "模型");

    make_btn(s_models, CW - 230, 2, 104, 36, COL_BTN, "刷新",
             model_rescan_cb, 0);
    make_btn(s_models, CW - 118, 2, 110, 36, COL_BTN, "返回",
             model_back_cb, 0);

    s_models_state = lv_label_create(s_models);
    lv_obj_set_pos(s_models_state, 8, 48);
    lv_obj_set_style_text_font(s_models_state, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_models_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_models_state, "");

    s_models_list = lv_obj_create(s_models);
    lv_obj_set_pos(s_models_list, 0, 44);
    lv_obj_set_size(s_models_list, CW, CH - 44);
    lv_obj_set_style_bg_opa(s_models_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_models_list, 0, 0);
    lv_obj_set_style_pad_all(s_models_list, 0, 0);
    lv_obj_set_flex_flow(s_models_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_models_list, 4, 0);
    lv_obj_set_scrollbar_mode(s_models_list, LV_SCROLLBAR_MODE_AUTO);
}

// -----------------------------------------------------------------------------
static void app_on_open(lv_obj_t* parent)
{
    lv_obj_t* root = place(parent, 0, 0, CW, CH);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    live_build(root);
    imgs_build(root);
    view_build(root);
    models_build(root);

    //static状态复位(应用被删后static仍在, 重开必须从零开始)
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_prev_on = 1;
    vidwin_align_left(1);                    //本应用视频洞靠左(右栏控制台)

    //记录当前代数: 丢弃上次会话遗留的桥事件, 之后的变化才算数
    //(状态量直读当前快照: 模型/清单/运行态开局即显示)
    gui_model_snapshot(&s_snap);
    s_last_cam = s_snap.gen_cam;
    s_last_cap = s_snap.gen_cap;
    s_last_fs = s_snap.gen_fs;
    s_last_photo = s_snap.gen_photo;
    s_last_model = s_snap.gen_model;
    s_last_infer = s_snap.gen_infer;
    s_last_res = s_snap.gen_res;
    s_last_run = s_snap.gen_run;
    s_last_models = s_snap.gen_models;
    s_last_det = s_snap.gen_det;
    s_hdmi = s_snap.cam_hdmi ? 2 : 0;       //链路开=带框档(默认开叠加)

    lv_label_set_text(s_lbl_run, s_snap.running ? "停止" : "开始");
    lv_label_set_text(s_lbl_prev, "预览 开");
    lv_label_set_text(s_lbl_hdmi,
                      s_hdmi == 0 ? "HDMI 关"
                      : s_hdmi == 1 ? "HDMI 开" : "HDMI 检测");
    model_card_refresh();
    result_rows_refresh();
    trend_refresh();
    models_rebuild();                        //模型清单开机已发布, 直接建
    show_panel(MODE_LIVE);

    post_cmd(APP_CMD_CAM_OPEN, 0, 0);        //进入即开摄像头(图片模式不依赖)
}

static void app_on_close(void)
{
    //先停推理与视频写帧, 再停链路(命令异步, 均幂等)
    post_cmd(APP_CMD_RUN_SET, 0, 0);
    vidwin_align_left(0);                    //还原居中(相机/云台应用用)
    s_gui_live = 0;
    vidwin_enable(0);
    vidwin_task_stop();
    post_cmd(APP_CMD_PHOTO_UNLOAD, 0, 0);
    post_cmd(APP_CMD_CAM_CLOSE, 0, 0);
    s_mode = MODE_LIVE;
}

// -----------------------------------------------------------------------------
static void app_refresh(void)
{
    gui_model_snapshot(&s_snap);
    int res_changed = 0;

    //摄像头链路(相机应用共用; HDMI态可能被外部改变, 同步三态标签)
    if (s_snap.gen_cam != s_last_cam)
    {
        s_last_cam = s_snap.gen_cam;
        if (!s_snap.cam_hdmi && s_hdmi != 0)
        {
            s_hdmi = 0;
            lv_label_set_text(s_lbl_hdmi, "HDMI 关");
        }
    }

    //运行态: 开始/停止按钮文本 + 模型卡帧率值
    if (s_snap.gen_run != s_last_run)
    {
        s_last_run = s_snap.gen_run;
        lv_label_set_text(s_lbl_run, s_snap.running ? "停止" : "开始");
        model_card_refresh();
    }

    //模型切换
    if (s_snap.gen_model != s_last_model)
    {
        s_last_model = s_snap.gen_model;
        model_card_refresh();
        result_rows_refresh();
        models_rebuild();                    //面板藏着也重建, 切回即最新
    }

    //单帧耗时/推理指标: 模型卡数值 + 趋势柱
    if (s_snap.gen_infer != s_last_infer)
    {
        s_last_infer = s_snap.gen_infer;
        model_card_refresh();
        trend_refresh();
    }

    //识别结果(分类Top-K/检测类别摘要)
    if (s_snap.gen_res != s_last_res)
    {
        s_last_res = s_snap.gen_res;
        res_changed = 1;
    }

    //检测框列表(查看面板LVGL框; 洞内框由vid任务按gen_det自取)
    if (s_snap.gen_det != s_last_det)
    {
        s_last_det = s_snap.gen_det;
        view_boxes_rebuild();
        res_changed = 1;
    }

    if (res_changed)
    {
        result_rows_refresh();
        view_result_refresh();
    }

    //图片列表(FS_LIST回包)
    if (s_snap.gen_fs != s_last_fs)
    {
        const int rebuild = (strcmp(s_snap.fs_path, PHOTOS_DIR) == 0);
        s_last_fs = s_snap.gen_fs;
        if (rebuild)
            imgs_rebuild();
    }

    //查看器开关(gen_photo: pix非0=新图, =0=卸载)
    if (s_snap.gen_photo != s_last_photo)
    {
        s_last_photo = s_snap.gen_photo;
        if (s_snap.photo_pix)
        {
            memset(&s_dsc, 0, sizeof(s_dsc));
            s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
            s_dsc.header.w = (uint32_t)s_snap.photo_w;
            s_dsc.header.h = (uint32_t)s_snap.photo_h;
            s_dsc.header.stride = (uint32_t)s_snap.photo_w * 3;
            s_dsc.data_size = (uint32_t)s_snap.photo_w
                            * (uint32_t)s_snap.photo_h * 3;
            s_dsc.data = (const uint8_t*)s_snap.photo_pix;
            lv_image_set_src(s_img, &s_dsc);
            lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CONTAIN);
            lv_image_set_antialias(s_img, true);
            lv_obj_clean(s_view_boxes);
            lv_label_set_text(s_lbl_vres, "");
            show_panel(MODE_VIEW);
        }
        else if (s_mode == MODE_VIEW)
        {
            show_panel(MODE_IMGS);
            lv_obj_clean(s_view_boxes);
            lv_label_set_text(s_lbl_vres, "");
        }
    }

    //模型清单(开机/重扫/推送联动)
    if (s_snap.gen_models != s_last_models)
    {
        s_last_models = s_snap.gen_models;
        models_rebuild();
    }

    //错误/提示toast(相机未连接/模型读取失败/推理失败/无图片…)
    if (s_snap.gen_cap != s_last_cap)
    {
        s_last_cap = s_snap.gen_cap;
        if (!s_snap.cap_ok && s_snap.cap_file[0])
            launcher_toast(s_snap.cap_file);
    }
}

// -----------------------------------------------------------------------------
const LauncherApp app_infer = {
    .name       = "推理",
    .icon       = LV_SYMBOL_EYE_OPEN,
    .on_open    = app_on_open,
    .on_close   = app_on_close,
    .refresh    = app_refresh,
    .refresh_ms = 150,
    .accent     = 0x2FBCD4,
};

// =============================================================================
// 后端(app_task上下文): 推理回泵 + 模型管理 + 图片推理 —— FatFs纪律执行侧,
// 上方UI经appctl命令进入app_infer_dispatch。抓帧缓冲=0x11100000工作缓存
// (mem_layout登记, 2.8MB, 1280x720x3)。
// =============================================================================

#include "FreeRTOS.h"
#include "task.h"
#include "xil_types.h"
#include "xil_printf.h"
#include "hw/chain/cam_chain.h"
#include "hw/mem_layout.h"
#include "sw/impl/cam/cam.h"
#include "sw/impl/port/port.h"
#include "sw/impl/nn/model_mgr.h"
#include "sw/impl/nn/nn_runner.h"
#include "sw/impl/nn/nn_wdt.h"
#include "sw/app/int_app/app_camera.h"
#include "sw/app/int_app/app_infer.h"

//连续推理失败上限(超过自动停止; 单帧偶发失败不断流)
#define INFER_FAIL_MAX  3

static volatile int s_run = 0;      //推理循环运行中(RUN_SET驱动)
static unsigned s_cycle = 0;        //累计推理帧数
static int s_fail_streak = 0;       //连续失败计数

int app_infer_running(void)
{
    return s_run;
}

//类别名翻译(labels.txt行号; 越界/无表打"?")
static const char* label_name(const NnModel* m, int label)
{
    if (m != 0 && label >= 0 && label < m->num_classes && m->labels != 0)
        return m->labels[label];
    return "?";
}

static u16 clamp_u16(int v)
{
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return (u16)v;
}

//NnResult -> bridge发布:
//  检测 -> pub_det(框, 源帧坐标系, vidwin/查看器共用) + pub_detect(类别摘要)
//  分类 -> pub_classify(Top-K) + 清框(上一次可能是检测)
//  空   -> 按检测空处理(目标数清零+清框)
static void publish_result(const NnResult* res, int src_w, int src_h,
                           unsigned ms)
{
    const NnModel* m = nn_active_model();
    gui_model_pub_infer(ms);

    if (res->topk_n > 0)
    {
        GuiTopKItem items[GUI_TOPK_MAX];
        int n = res->topk_n;
        if (n > GUI_TOPK_MAX) n = GUI_TOPK_MAX;
        for (int i = 0; i < n; i++)
        {
            const char* nm = label_name(m, res->topk_label[i]);
            strncpy(items[i].name, nm, GUI_LBL_MAX - 1);
            items[i].name[GUI_LBL_MAX - 1] = 0;
            int sc = (int)(res->topk_score[i] * 100.0f + 0.5f);
            if (sc < 0) sc = 0;
            if (sc > 100) sc = 100;
            items[i].score = sc;
        }
        gui_model_pub_classify(n, items);
        gui_model_pub_det(0, 0, 0, 0);
        return;
    }

    GuiDetBox boxes[GUI_DET_BOX_MAX];
    int n = res->box_n;
    if (n > GUI_DET_BOX_MAX) n = GUI_DET_BOX_MAX;
    for (int i = 0; i < n; i++)
    {
        const NnDetBox* b = &res->box[i];
        boxes[i].cls = (u8)clamp_u16(b->label);
        int sc = (int)(b->score * 100.0f + 0.5f);
        if (sc < 0) sc = 0;
        if (sc > 100) sc = 100;
        boxes[i].score = (u8)sc;
        boxes[i].x0 = clamp_u16(b->x0);
        boxes[i].y0 = clamp_u16(b->y0);
        boxes[i].x1 = clamp_u16(b->x1);
        boxes[i].y1 = clamp_u16(b->y1);
        const char* nm = label_name(m, b->label);
        strncpy(boxes[i].name, nm, sizeof(boxes[i].name) - 1);
        boxes[i].name[sizeof(boxes[i].name) - 1] = 0;
    }
    gui_model_pub_det(src_w, src_h, boxes, n);

    //类别摘要: 每类计数+最高分(检测; 空结果把目标数清零)
    GuiDetCls cls[GUI_DET_CLS_MAX];
    int nc = 0;
    for (int i = 0; i < n; i++)
    {
        const char* nm = boxes[i].name;
        int k = 0;
        for (; k < nc; k++)
            if (strncmp(cls[k].name, nm, sizeof(cls[k].name)) == 0)
                break;
        if (k < nc)
        {
            cls[k].count++;
            if (boxes[i].score > cls[k].best)
                cls[k].best = boxes[i].score;
        }
        else if (nc < GUI_DET_CLS_MAX)
        {
            //源是定长12字节的框名(NUL必在其中), 整块拷贝即天然终止
            memcpy(cls[nc].name, boxes[i].name, sizeof(boxes[i].name));
            cls[nc].count = 1;
            cls[nc].best = boxes[i].score;
            nc++;
        }
    }
    gui_model_pub_detect(nc, res->box_n, cls);
}

//停止推理(统一出口: 清叠加框, vidwin渐隐撤除)
static void run_stop(const char* toast)
{
    s_run = 0;
    nn_wdt_disarm();
    gui_model_pub_run(0, s_cycle);
    gui_model_pub_det(0, 0, 0, 0);
    if (toast != 0)
        gui_model_pub_capture(0, toast);
}

static void run_start(void)
{
    if (s_run)
        return;
    if (nn_active_model() == 0)
    {
        gui_model_pub_capture(0, "未选模型");
        return;
    }
    if (!cam_chain_running())
    {
        gui_model_pub_capture(0, "相机未连接");
        return;
    }
    s_run = 1;
    s_fail_streak = 0;
    gui_model_pub_run(1, s_cycle);
}

//抓帧等帧边界期间的让出(30fps下一帧≤33ms)
static void pump_idle(void)
{
    vTaskDelay(1);
}

//推一帧: 抓拍 -> 推理 -> 发布 -> (加速模式)标注回放。
//看门狗按帧独立会话(不跨帧持狗: 切模型的激活会话内部自带arm/disarm,
//嵌套持狗会在会话析构时把运行中的狗拆掉)
void app_infer_pump(void)
{
    if (!s_run)
        return;

    //兜底: 已回桌面/链路已停(on_close的RUN_SET还在队列里)。
    //SD应用在场(launcher_active_app==-2)不算退出: SD应用(人脸追踪等)
    //借本泵跑推理, 结果照常经bridge det通道发布
    if (launcher_active_app() == -1 || !cam_chain_running())
    {
        run_stop(0);
        return;
    }

    u8* work = (u8*)MEM_WORK_BUFFER_ADDR;
    nn_wdt_arm();
    nn_wdt_kick();
    cam_grab_frame(work, pump_idle);        //等帧边界+整帧拷出(~2.7MB)
    const NnImage img = { work, cam_chain_width(), cam_chain_height(),
                          NN_PIX_BGR };
    NnResult res;
    const u64 t0 = port_time_us();
    const int rc = nn_infer(&img, &res);
    const u64 t1 = port_time_us();
    nn_wdt_disarm();

    if (rc != 0)
    {
        xil_printf("[infer] frame failed rc=%d\r\n", rc);
        if (++s_fail_streak >= INFER_FAIL_MAX)
            run_stop("推理失败,已停止");
        return;
    }
    s_fail_streak = 0;
    publish_result(&res, cam_chain_width(), cam_chain_height(),
                   (unsigned)((t1 - t0) / 1000));
    gui_model_pub_run(1, ++s_cycle);

    //预览关=纯黑屏加速: 推理无头跑(数值/趋势照常), 洞里不写任何画面
    //(切换时prev_cb已vidwin_paint_bg涂底色, 洞保持不动)
}

//模型清单发布(model_mgr当前清单 -> 桥; boot/重扫/传输联动共用)
void app_infer_pub_models(void)
{
    char names[GUI_MODEL_MAX][GUI_LBL_MAX];
    char tags[GUI_MODEL_MAX][16];
    const int n = nn_models_count();
    for (int i = 0; i < n && i < GUI_MODEL_MAX; i++)
    {
        strncpy(names[i], nn_model_name(i), GUI_LBL_MAX - 1);
        names[i][GUI_LBL_MAX - 1] = 0;
        strncpy(tags[i], nn_model_tag(i), sizeof(tags[i]) - 1);
        tags[i][sizeof(tags[i]) - 1] = 0;
    }
    gui_model_pub_models(names, tags, n);
}

//当前激活模型状态发布(自检装好模型/重扫丢弃模型后, 桥与model_mgr对齐)
void app_infer_pub_model_state(void)
{
    const int idx = nn_active_index();
    if (idx >= 0)
        gui_model_pub_model(idx, nn_model_name(idx), nn_model_tag(idx));
    else
        gui_model_pub_model(-1, "", "");
}

//图片单次推理: 输入=相机应用照片查看缓冲(BGR顶行在先=NN_PIX_BGR)
static void photo_infer_cmd(void)
{
    if (nn_active_model() == 0)
    {
        gui_model_pub_capture(0, "未选模型");
        return;
    }
    int w = 0, h = 0;
    const void* pix = app_camera_photo(&w, &h);
    if (pix == 0 || w <= 0 || h <= 0)
    {
        gui_model_pub_capture(0, "未打开图片");
        return;
    }
    const NnImage img = { (const unsigned char*)pix, w, h, NN_PIX_BGR };
    NnResult res;
    nn_wdt_arm();
    nn_wdt_kick();
    const u64 t0 = port_time_us();
    const int rc = nn_infer(&img, &res);
    const u64 t1 = port_time_us();
    nn_wdt_disarm();
    if (rc != 0)
    {
        xil_printf("[infer] photo failed rc=%d\r\n", rc);
        gui_model_pub_capture(0, "推理失败");
        return;
    }
    publish_result(&res, w, h, (unsigned)((t1 - t0) / 1000));
}

void app_infer_dispatch(const AppCmd* cmd)
{
    switch (cmd->type)
    {
    case APP_CMD_RUN_SET:
        if (cmd->val)
            run_start();
        else
            run_stop(0);
        break;
    case APP_CMD_RUN_TOGGLE:
        if (s_run)
            run_stop(0);
        else
            run_start();
        break;
    case APP_CMD_MODEL_SELECT:
    {
        const int idx = cmd->val;
        if (idx < 0 || idx >= nn_models_count())
            break;
        if (idx != nn_active_index())
        {
            if (nn_model_activate(idx) == 0)
                gui_model_pub_model(idx, nn_model_name(idx),
                                   nn_model_tag(idx));
            else
            {
                gui_model_pub_model(-1, "", "");  //激活失败=无模型在载
                gui_model_pub_capture(0, "读取失败");
            }
        }
        else
        {
            //已激活(如开机推送自检装好): 补发一次让界面与实际对齐
            app_infer_pub_model_state();
        }
        break;
    }
    case APP_CMD_MODELS_RESCAN:
        if (s_run)
            run_stop("已停止");             //重扫会丢弃激活模型
        nn_models_scan();
        app_infer_pub_models();
        app_infer_pub_model_state();        //重扫后激活态归零/保持, 对齐桥
        break;
    case APP_CMD_PHOTO_INFER:
        photo_infer_cmd();
        break;
    default:
        break;
    }
}
