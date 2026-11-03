// =============================================================================
// app_camera —— 相机应用(内置): 实时预览 + 拍照存SD + 相册浏览/删除
//
// 三个互斥面板:
//   实时   摄像头640x360预览洞(vidwin任务直写帧缓冲, 洞内无任何控件)
//          + 底栏(状态/HDMI开关/相册/拍照); 进入时开摄像头链路
//   相册   /photos目录列表(点行看大图, 行尾垃圾桶删除), 复用FS_LIST管道
//   查看器 整幅BMP等比缩放显示(lv_image CONTAIN), 返回/删除
//
// 数据流(FatFs单线程纪律, GUI不碰SD):
//   拍照:   CAPTURE -> app_task抓帧写BMP -> pub_capture -> toast
//   浏览:   FS_LIST(/photos) -> pub_fs -> 重建列表
//   查看:   PHOTO_LOAD(路径) -> app_task读BMP -> pub_photo(像素指针) -> 建图
//   退出:   PHOTO_UNLOAD -> CAM_CLOSE(app_task停vid任务+链路)
//
// 视频洞几何(与vidwin.c的vidwin_task_start_centered一致, 两处必须镜像):
// 洞=640x360, 内容区水平居中、顶部平齐 => parent坐标((784-640)/2, 0)=(72,0)
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

#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF

#define PHOTOS_DIR    "0:/photos"

//视频洞(parent坐标; vidwin按LCD屏坐标开窗, 两者按同一布局常量推出)
#define HOLE_X        ((784 - VIDWIN_VID_W) / 2)      //152
#define HOLE_Y        0
#define BAR_Y         (VIDWIN_VID_H + 8)              //278 控制栏顶

//控件引用
static lv_obj_t* s_live;            //三个互斥面板
static lv_obj_t* s_album;
static lv_obj_t* s_viewer;
static lv_obj_t* s_lbl_status;      //实时底栏状态(未开启/预览fps)
static lv_obj_t* s_btn_hdmi;
static lv_obj_t* s_lbl_hdmi;
static lv_obj_t* s_album_list;      //相册列表容器
static lv_obj_t* s_lbl_astate;      //相册状态行(读取中/空/错误)
static lv_obj_t* s_img;             //查看器大图
static lv_obj_t* s_lbl_vname;       //查看器文件名

static GuiSnapshot s_snap;
static uint32_t   s_last_cam, s_last_vid, s_last_cap, s_last_fs, s_last_photo;
static int        s_mode;           //0=实时 1=相册 2=查看器
static char       s_view_path[GUI_FS_PATH_MAX];   //正在查看的路径(删除用)
static char       s_del_path[GUI_FS_PATH_MAX];    //待确认删除的路径
static lv_obj_t*  s_confirm;        //删除确认框(挂顶层layer, 一次一个)
static lv_image_dsc_t s_dsc;        //查看器图像描述(数据指针随gen_photo换)

// -----------------------------------------------------------------------------
static void post_cmd(int type, int val, const char* path)
{
    AppCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint8_t)type;
    cmd.val = val;
    if (path)
        strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    appctl_post(&cmd);
}

//大小格式化: B/KB/MB 自适应, buf>=16字节
static void fmt_size(char* buf, unsigned cap, uint32_t sz)
{
    if (sz >= 1024u * 1024u)
        lv_snprintf(buf, cap, "%u.%u MB", sz >> 20,
                    (unsigned)((sz & 0xFFFFFu) * 10u >> 20));
    else if (sz >= 1024u)
        lv_snprintf(buf, cap, "%u KB", (sz + 1023u) >> 10);
    else
        lv_snprintf(buf, cap, "%u B", sz);
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
// 面板切换(互斥可见); 实时面板在场时视频窗才允许写帧
static void show_panel(int mode)
{
    s_mode = mode;
    lv_obj_add_flag(s_live, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_album, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* p = (mode == 0) ? s_live : (mode == 1) ? s_album : s_viewer;
    lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
    vidwin_enable(mode == 0);
}

static void shutter_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    post_cmd(APP_CMD_CAPTURE, 0, 0);
}

static void hdmi_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    post_cmd(APP_CMD_HDMI_SET, s_snap.cam_hdmi ? 0 : 1, 0);
}

static void album_open_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    show_panel(1);
    lv_obj_remove_flag(s_lbl_astate, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_lbl_astate, "读取中…");
    lv_obj_clean(s_album_list);
    post_cmd(APP_CMD_FS_LIST, 0, PHOTOS_DIR);
}

static void live_back_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    show_panel(0);
}

// -----------------------------------------------------------------------------
// 删除确认框(与文件应用同款: 全屏遮罩+卡片, 挂顶层layer)
static void del_ok_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    post_cmd(APP_CMD_FS_DELETE, 0, s_del_path);
    post_cmd(APP_CMD_FS_LIST, 0, PHOTOS_DIR);      //重列(原地刷新)
    if (s_mode == 2)
    {                                               //查看器内删除: 关图回相册
        post_cmd(APP_CMD_PHOTO_UNLOAD, 0, 0);
        show_panel(1);
    }
    lv_obj_delete(s_confirm);
    s_confirm = 0;
}

static void del_cancel_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    lv_obj_delete(s_confirm);
    s_confirm = 0;
}

static void confirm_dialog(const char* question)
{
    if (s_confirm)
        return;
    lv_display_t* disp = lv_display_get_default();
    s_confirm = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_size(s_confirm, lv_display_get_horizontal_resolution(disp),
                    lv_display_get_vertical_resolution(disp));
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_60, 0);
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(s_confirm);
    lv_obj_set_size(card, 400, 130);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* q = lv_label_create(card);
    lv_obj_set_style_text_font(q, &font_cn_16, 0);
    lv_obj_set_style_text_color(q, lv_color_white(), 0);
    lv_label_set_text(q, question);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t* ok = make_btn(card, 85, 78, 110, 38, 0xB3261E, "删除",
                            del_ok_cb, 0);
    (void)ok;
    lv_obj_t* cancel = make_btn(card, 205, 78, 110, 38, 0x2A3450, "取消",
                                del_cancel_cb, 0);
    (void)cancel;
}

//相册行删除按钮(先记下待删路径再弹确认)
static void row_del_cb(lv_event_t* e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_snap.fs_n)
        return;
    char path[GUI_FS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", PHOTOS_DIR, s_snap.fs[idx].name);
    strncpy(s_del_path, path, sizeof(s_del_path) - 1);
    s_del_path[sizeof(s_del_path) - 1] = 0;

    char txt[GUI_FS_NAME_MAX + 16];
    lv_snprintf(txt, sizeof(txt), "删除 %s ?", s_snap.fs[idx].name);
    confirm_dialog(txt);
}

//相册行点击: 请求大图(app_task读BMP, gen_photo回来后进查看器)
static void row_click_cb(lv_event_t* e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_snap.fs_n || s_snap.fs[idx].is_dir)
        return;
    char path[GUI_FS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", PHOTOS_DIR, s_snap.fs[idx].name);
    strncpy(s_view_path, path, sizeof(s_view_path) - 1);
    s_view_path[sizeof(s_view_path) - 1] = 0;
    post_cmd(APP_CMD_PHOTO_LOAD, 0, path);
}

//查看器按钮
static void vback_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    post_cmd(APP_CMD_PHOTO_UNLOAD, 0, 0);   //gen_photo=0时refresh切回相册
}

static void vdel_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    char txt[GUI_FS_NAME_MAX + 16];
    lv_snprintf(txt, sizeof(txt), "删除 %s ?", s_snap.photo_name);
    confirm_dialog(txt);
}

// -----------------------------------------------------------------------------
// 相册列表重建(gen_fs变化且路径为/photos; 面板隐藏时也重建, 保切回即最新)
static void album_rebuild(void)
{
    lv_obj_clean(s_album_list);

    if (s_snap.fs_err != 0)
    {
        lv_label_set_text(s_lbl_astate,
                          s_snap.fs_err == -1 ? "SD卡未挂载" : "目录打开失败");
        lv_obj_remove_flag(s_lbl_astate, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_snap.fs_n == 0)
    {
        lv_label_set_text(s_lbl_astate, "暂无照片");
        lv_obj_remove_flag(s_lbl_astate, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_lbl_astate, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < s_snap.fs_n; i++)
    {
        const GuiFsEntry* en = &s_snap.fs[i];

        lv_obj_t* row = lv_button_create(s_album_list);
        lv_obj_set_size(row, lv_pct(100), 40);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_CARD_EDGE), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
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

        lv_obj_t* sz = lv_label_create(row);
        lv_obj_set_width(sz, 84);
        lv_obj_set_style_text_align(sz, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(sz, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sz, lv_color_hex(COL_TEXT_DIM), 0);
        char buf[16];
        fmt_size(buf, sizeof(buf), en->size);
        lv_label_set_text(sz, buf);
        lv_obj_align(sz, LV_ALIGN_RIGHT_MID, -44, 0);

        lv_obj_t* del = lv_button_create(row);
        lv_obj_set_size(del, 32, 32);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(0x402028), 0);
        lv_obj_set_style_border_width(del, 0, 0);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_t* di = lv_label_create(del);
        lv_obj_set_style_text_font(di, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(di, lv_color_hex(0xFF8A80), 0);
        lv_label_set_text(di, LV_SYMBOL_TRASH);
        lv_obj_center(di);
        lv_obj_add_event_cb(del, row_del_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
    }
}

//查看器装载新图(gen_photo变化且pix非0)
static void viewer_show(void)
{
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_dsc.header.w = (uint32_t)s_snap.photo_w;
    s_dsc.header.h = (uint32_t)s_snap.photo_h;
    s_dsc.header.stride = (uint32_t)s_snap.photo_w * 3;
    s_dsc.data_size = (uint32_t)s_snap.photo_w * (uint32_t)s_snap.photo_h * 3;
    s_dsc.data = (const uint8_t*)s_snap.photo_pix;

    lv_image_set_src(s_img, &s_dsc);
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(s_img, true);
    lv_label_set_text(s_lbl_vname, s_snap.photo_name);
    show_panel(2);
}

// -----------------------------------------------------------------------------
static void live_build(lv_obj_t* root, int cw, int ch)
{
    s_live = place(root, 0, 0, cw, ch);
    lv_obj_clear_flag(s_live, LV_OBJ_FLAG_SCROLLABLE);
    //视频洞(HOLE_X,0,480x270)内不放任何控件——vidwin任务每帧直写该区

    s_lbl_status = lv_label_create(s_live);
    lv_obj_set_pos(s_lbl_status, 8, BAR_Y + 18);
    lv_obj_set_width(s_lbl_status, 340);
    lv_obj_set_style_text_font(s_lbl_status, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_status, "未开启");

    s_btn_hdmi = make_btn(s_live, 368, BAR_Y + 2, 104, 48, 0x2A3450,
                          "HDMI 关", hdmi_cb, 0);
    s_lbl_hdmi = (lv_obj_t*)lv_obj_get_child(s_btn_hdmi, 0);

    make_btn(s_live, 484, BAR_Y + 2, 110, 48, 0x2A3450, "相册",
             album_open_cb, 0);
    make_btn(s_live, cw - 178, BAR_Y, 170, 52, 0xB3261E, "拍照",
             shutter_cb, &font_cn_20);
}

static void album_build(lv_obj_t* root, int cw, int ch)
{
    s_album = place(root, 0, 0, cw, ch);
    lv_obj_clear_flag(s_album, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(s_album);
    lv_obj_set_pos(title, 8, 8);
    lv_obj_set_style_text_font(title, &font_cn_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "相册");

    make_btn(s_album, cw - 118, 2, 110, 36, 0x2A3450, "返回拍摄",
             live_back_cb, 0);

    s_lbl_astate = lv_label_create(s_album);
    lv_obj_set_pos(s_lbl_astate, 8, 48);
    lv_obj_set_style_text_font(s_lbl_astate, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_astate, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_astate, "读取中…");

    s_album_list = lv_obj_create(s_album);
    lv_obj_set_pos(s_album_list, 0, 44);
    lv_obj_set_size(s_album_list, cw, ch - 44);
    lv_obj_set_style_bg_opa(s_album_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_album_list, 0, 0);
    lv_obj_set_style_pad_all(s_album_list, 0, 0);
    lv_obj_set_flex_flow(s_album_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_album_list, 4, 0);
    lv_obj_set_scrollbar_mode(s_album_list, LV_SCROLLBAR_MODE_AUTO);
}

static void viewer_build(lv_obj_t* root, int cw, int ch)
{
    s_viewer = place(root, 0, 0, cw, ch);
    lv_obj_clear_flag(s_viewer, LV_OBJ_FLAG_SCROLLABLE);

    s_img = lv_image_create(s_viewer);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_set_size(s_img, cw, ch);
    //CONTAIN: 等比缩放到最大可完整显示并居中(1280x720 -> 约750x421)
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CONTAIN);

    lv_obj_t* back = lv_button_create(s_viewer);
    lv_obj_set_pos(back, 4, 4);
    lv_obj_set_size(back, 64, 36);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_t* bi = lv_label_create(back);
    lv_obj_set_style_text_font(bi, &lv_font_montserrat_16, 0);
    lv_label_set_text(bi, LV_SYMBOL_LEFT);
    lv_obj_center(bi);
    lv_obj_add_event_cb(back, vback_cb, LV_EVENT_CLICKED, 0);

    s_lbl_vname = lv_label_create(s_viewer);
    lv_obj_set_pos(s_lbl_vname, cw / 2 - 160, 10);
    lv_obj_set_width(s_lbl_vname, 320);
    lv_obj_set_style_text_font(s_lbl_vname, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_vname, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_lbl_vname, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_lbl_vname, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_lbl_vname, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_lbl_vname, 8, 0);
    lv_obj_set_style_pad_hor(s_lbl_vname, 8, 0);
    lv_label_set_long_mode(s_lbl_vname, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_vname, "");

    lv_obj_t* del = lv_button_create(s_viewer);
    lv_obj_set_pos(del, cw - 68, 4);
    lv_obj_set_size(del, 64, 36);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x402028), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_80, 0);
    lv_obj_t* di = lv_label_create(del);
    lv_obj_set_style_text_font(di, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(di, lv_color_hex(0xFF8A80), 0);
    lv_label_set_text(di, LV_SYMBOL_TRASH);
    lv_obj_center(di);
    lv_obj_add_event_cb(del, vdel_cb, LV_EVENT_CLICKED, 0);
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

    live_build(root, cw, ch);
    album_build(root, cw, ch);
    viewer_build(root, cw, ch);

    //static状态复位(应用被删后static仍在, 重开必须从零开始)
    s_confirm = 0;
    s_view_path[0] = 0;
    s_del_path[0] = 0;
    memset(&s_dsc, 0, sizeof(s_dsc));
    show_panel(0);

    //记录当前代数: 丢弃上次会话遗留的桥状态, 之后的变化才算数
    gui_model_snapshot(&s_snap);
    s_last_cam = s_snap.gen_cam;
    s_last_vid = s_snap.gen_vid;
    s_last_cap = s_snap.gen_cap;
    s_last_fs = s_snap.gen_fs;
    s_last_photo = s_snap.gen_photo;

    post_cmd(APP_CMD_CAM_OPEN, 0, 0);      //进入即开摄像头(异步, 结果经gen_cam)
}

static void app_on_close(void)
{
    //先停视频写帧(退场动画期间洞区不能再被覆写), 链路停止走命令
    vidwin_enable(0);
    vidwin_task_stop();
    post_cmd(APP_CMD_PHOTO_UNLOAD, 0, 0);
    post_cmd(APP_CMD_CAM_CLOSE, 0, 0);

    //确认框挂在顶层layer(不属于应用屏, 框架退场不删): 必须自行清理
    if (s_confirm)
    {
        lv_obj_delete(s_confirm);
        s_confirm = 0;
    }
    s_mode = 0;
}

// -----------------------------------------------------------------------------
static void app_refresh(void)
{
    gui_model_snapshot(&s_snap);

    //摄像头链路状态: HDMI按钮文字 + 底栏状态行
    if (s_snap.gen_cam != s_last_cam)
    {
        s_last_cam = s_snap.gen_cam;
        lv_label_set_text(s_lbl_hdmi, s_snap.cam_hdmi ? "HDMI 开" : "HDMI 关");
        lv_label_set_text(s_lbl_status,
                          s_snap.cam_on ? "预览…" : "未开启");
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

    //拍照/错误提示(cap_ok=1: 文件名; =0: 错误文本), 均以toast呈现
    if (s_snap.gen_cap != s_last_cap)
    {
        s_last_cap = s_snap.gen_cap;
        if (s_snap.cap_file[0])
            launcher_toast(s_snap.cap_file);
    }

    //相册列表(FS_LIST回包)
    if (s_snap.gen_fs != s_last_fs)
    {
        const int rebuild = (strcmp(s_snap.fs_path, PHOTOS_DIR) == 0);
        s_last_fs = s_snap.gen_fs;
        if (rebuild)
            album_rebuild();
    }

    //查看器开关
    if (s_snap.gen_photo != s_last_photo)
    {
        s_last_photo = s_snap.gen_photo;
        if (s_snap.photo_pix)
            viewer_show();
        else if (s_mode == 2)
            show_panel(1);
    }
}

// -----------------------------------------------------------------------------
const LauncherApp app_camera = {
    .name       = "相机",
    .icon       = LV_SYMBOL_VIDEO,
    .on_open    = app_on_open,
    .on_close   = app_on_close,
    .refresh    = app_refresh,
    .refresh_ms = 150,
    .accent    = 0x00B0FF,
};

// =============================================================================
// 后端(app_task上下文): 摄像头链路启停/拍照/照片查看 —— FatFs纪律执行侧,
// 上方UI经appctl命令进入app_camera_dispatch。拍照目录固定 /photos。
// =============================================================================

#include "FreeRTOS.h"
#include "task.h"
#include "xil_types.h"
#include "xil_printf.h"
#include "hw/chain/cam_chain.h"
#include "sw/impl/cam/cam.h"
#include "sw/impl/sd/sd.h"
#include "sw/impl/sd/photo.h"
#include "sw/app/int_app/app_camera.h"

//照片查看缓冲的释放走"延迟一代"策略: pub_photo(0)后旧缓冲先挪进s_photo_old,
//下一次照片操作时才free —— gui侧此时早已渲染过gen_photo变化, 不会悬引用
static unsigned char* s_photo_view;      //当前发布的整文件缓冲(像素在其中)
static unsigned char* s_photo_old;       //已下发的上一张(延迟释放)
static int s_photo_w, s_photo_h;         //当前图片尺寸(图片推理输入用)

const void* app_camera_photo(int* w, int* h)
{
    *w = s_photo_w;
    *h = s_photo_h;
    return s_photo_view;
}

//抓帧等帧边界期间的让出(30fps下一帧≤33ms)
static void grab_idle(void)
{
    vTaskDelay(1);
}

//下一个照片编号: 扫描目录里IMG_NNNN.bmp的最大N + 1(目录不存在则从1开始)
static unsigned photos_next_index(void)
{
    SdDirEntry ents[GUI_FS_MAX];
    int n = 0, trunc = 0;
    unsigned maxn = 0;
    if (sd_list_dir(PHOTOS_DIR, ents, GUI_FS_MAX, &n, &trunc) == 0)
    {
        for (int i = 0; i < n; i++)
        {
            unsigned v = 0;
            if (sscanf(ents[i].name, "IMG_%u.bmp", &v) == 1 && v > maxn)
                maxn = v;
        }
    }
    return maxn + 1;
}

static void cam_open_cmd(void)
{
    //链路1280x720(HDMI默认档, 拍照同全尺寸); 预览窗640x360居中开洞
    //带一次自愈重试(链路反复启停后偶发的状态机不同步)
    const int rc = cam_chain_start_retry(1280, 720);
    if (rc == 0)
        vidwin_task_start_centered();
    gui_model_pub_cam(rc == 0, cam_chain_hdmi_get());
    if (rc != 0)
        gui_model_pub_capture(0, "相机未连接");   //toast提示, 细节看串口
    xil_printf("[cam] open 1280x720: %d\r\n", rc);
}

static void cam_close_cmd(void)
{
    vidwin_task_stop();                  //先停笔(应用on_close已调过, 幂等)
    cam_chain_stop();
    gui_model_pub_cam(0, 0);
    xil_printf("[cam] chain stopped\r\n");
}

static void capture_cmd(void)
{
    free(s_photo_old);
    s_photo_old = 0;

    if (!cam_chain_running())
    {
        gui_model_pub_capture(0, "相机未开启");
        return;
    }
    if (!sd_mounted())
    {
        gui_model_pub_capture(0, "SD卡未挂载");
        return;
    }

    const int W = cam_chain_width();
    const int H = cam_chain_height();
    uint8_t* frame = (uint8_t*)malloc((size_t)W * (size_t)H * 3);
    if (frame == 0)
    {
        gui_model_pub_capture(0, "内存不足");
        return;
    }
    cam_grab_frame(frame, grab_idle);    //等帧边界+整帧拷出(~2.7MB, 数毫秒)
    sd_mkdir(PHOTOS_DIR);                //首次拍照建目录(已存在不算错)

    char path[64], name[32];
    snprintf(name, sizeof(name), "IMG_%04u.bmp", photos_next_index());
    snprintf(path, sizeof(path), "%s/%s", PHOTOS_DIR, name);

    const int rc = photo_save_bmp(path, frame, (u32)W, (u32)H);
    free(frame);
    gui_model_pub_capture(rc == 0 ? 1 : 0, rc == 0 ? name : "保存失败");
    xil_printf("[cam] capture %s: %s\r\n", path, rc == 0 ? "ok" : "failed");
}

static void photo_load_cmd(const char* path)
{
    free(s_photo_old);
    s_photo_old = 0;

    unsigned long len = 0;
    unsigned char* file = sd_read_file(path, &len);
    if (file == 0)
    {
        gui_model_pub_capture(0, "打开失败");
        return;
    }
    u32 w = 0, h = 0, off = 0;
    if (photo_probe_bmp(file, (u32)len, &w, &h, &off) != 0)
    {
        free(file);
        gui_model_pub_capture(0, "格式不支持");
        return;
    }
    photo_flip_rows(file + off, w, h);   //BMP底行在先 -> 顶行在先(显示序)

    if (s_photo_view)                    //上一张还挂着: 延迟到下代释放
        s_photo_old = s_photo_view;
    s_photo_view = file;

    const char* base = strrchr(path, '/');
    s_photo_w = (int)w;
    s_photo_h = (int)h;
    gui_model_pub_photo(file + off, (int)w, (int)h, base ? base + 1 : path);
    xil_printf("[cam] view %s (%ux%u)\r\n", path, (unsigned)w, (unsigned)h);
}

static void photo_unload_cmd(void)
{
    free(s_photo_old);
    s_photo_old = 0;
    s_photo_w = 0;
    s_photo_h = 0;
    //照片查看缓冲释放: 照片坐标系的检测框叠加随之作废(否则vid任务会把
    //残留框画到实时预览洞上); 相机应用自身不发布框, 此处恒为无害清零
    gui_model_pub_det(0, 0, 0, 0);
    if (s_photo_view)
    {
        gui_model_pub_photo(0, 0, 0, "");
        s_photo_old = s_photo_view;      //gui可能还在画最后一帧, 延迟释放
        s_photo_view = 0;
    }
}

void app_camera_dispatch(const AppCmd* cmd)
{
    switch (cmd->type)
    {
    case APP_CMD_CAM_OPEN:
        cam_open_cmd();
        break;
    case APP_CMD_CAM_CLOSE:
        cam_close_cmd();
        break;
    case APP_CMD_HDMI_SET:
        cam_chain_hdmi_set(cmd->val);
        gui_model_pub_cam(cam_chain_running(), cam_chain_hdmi_get());
        xil_printf("[cam] hdmi %d\r\n", cmd->val ? 1 : 0);
        break;
    case APP_CMD_CAPTURE:
        capture_cmd();
        break;
    case APP_CMD_PHOTO_LOAD:
        photo_load_cmd(cmd->path);
        break;
    case APP_CMD_PHOTO_UNLOAD:
        photo_unload_cmd();
        break;
    default:
        break;
    }
}
