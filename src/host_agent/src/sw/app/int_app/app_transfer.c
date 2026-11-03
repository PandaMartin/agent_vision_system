// =============================================================================
// app_transfer —— 文件传输应用(内置): 网口推送文件落SD
//
// 传输只在本应用内发生: 进入时开TFTP(UDP 69)监听(net_server_start), 退出时
// 停(net_server_stop, 进行中的会话≤1个超时周期中止)——桌面态端口不监听。
// PC端: python tools/netpush/net_push.py <文件> 或任意标准tftp客户端
// (tftp -i 192.168.1.10 put <文件>, 裸名落SD根"0:/"), 板IP默认192.168.1.10。
//
// 页面(784x424):
//   顶卡  链路状态点+状态文本 + 右侧"本机 IP:端口"(开屏即渲染, 不随事件缺席)
//   记录卡  传输记录列表(最新在上, 最多8条; 成功绿/失败红 + 大小·原因)
//           接收中时列表顶部内联一条活动行(文件名+进度+速率, 实时刷新;
//           TFTP的WRQ不带文件尺寸, 收流期间总量未知->只显示已收字节+速率,
//           落SD阶段(total就位)进度条出现并置满)
// 结果与状态解耦: 结果经bridge的xfer_res_*(持久) + xfer_res_gen(结果代数)——
// DONE紧跟LISTEN挤进同一刷新拍也不会丢结果(事件竞争曾致"完成"时有时无)。
// 数据流: net任务收文件->进度经bridge(gen_xfer)->app_task落SD
// (APP_CMD_NET_FILE, 本文件dispatch)->落SD后按目录联动重扫。
// =============================================================================

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"

#include "sw/app/launcher.h"
#include "sw/app/bridge.h"
#include "sw/app/appctl.h"
#include "sw/app/int_app/app_transfer.h"
#include "sw/app/int_app/app_infer.h"
#include "sw/app/ext_app/sdapp.h"
#include "sw/impl/nn/model_mgr.h"
#include "sw/impl/lcd/lvgl_port.h"
#include "sw/impl/net/net_server.h"
#include "sw/impl/sd/sd.h"

#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF
#define COL_LIME      0x76FF03
#define COL_RED       0xFF8A80
#define COL_DOT_OFF   0x39456B      //未连接时的状态点(暗蓝灰)

//传输记录上限(UI侧环形, 最新在上)
#define XFER_HIST_MAX 8

static lv_obj_t* s_dot;            //链路状态点
static lv_obj_t* s_lbl_state;      //链路状态文本
static lv_obj_t* s_lbl_addr;       //本机 IP:端口
static lv_obj_t* s_rows;           //记录列表容器(活动行+历史行)
static lv_obj_t* s_live_name;      //活动行: 文件名
static lv_obj_t* s_live_bar;       //活动行: 进度条
static lv_obj_t* s_live_info;      //活动行: 百分比+速率

static GuiSnapshot s_snap;
static uint32_t   s_last_xfer, s_last_net, s_last_res_gen;
static int        s_shown_state = -1;
static int        s_was_rx;                 //上一拍是否RX(进出RX重建列表)
static int        s_rows_dirty;             //列表需要重建

//传输记录(本次进入应用以来; 最新在[0])
typedef struct {
    char     name[GUI_FS_NAME_MAX];
    uint32_t total;
    int      ok;                          //1成功 0失败
    char     msg[40];
} XferRec;
static XferRec s_hist[XFER_HIST_MAX];
static int     s_hist_n;

// -----------------------------------------------------------------------------
static void post_cmd(int type)
{
    AppCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = (uint8_t)type;
    appctl_post(&cmd);
}

//圆角卡片底板
static lv_obj_t* make_card(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    return c;
}

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, int color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

//字节数 -> "12.3 MB"/"456 KB"(ASCII分隔, 字库无中点)
static void fmt_size(char* buf, int cap, uint32_t b)
{
    if (b >= 1024u * 1024u)
        snprintf(buf, cap, "%u.%u MB",
                 (unsigned)(b >> 20),
                 (unsigned)(((b & 0xFFFFFu) * 10u) >> 20));
    else
        snprintf(buf, cap, "%u KB", (unsigned)(b >> 10));
}

// -----------------------------------------------------------------------------
static void app_on_open(lv_obj_t* parent)
{
    lv_display_t* disp = lv_display_get_default();
    const int cw = lv_display_get_horizontal_resolution(disp) - 16;
    const int ch = lv_display_get_vertical_resolution(disp) - 40 - 16;

    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, cw, ch);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    //---- 顶卡: 链路状态 + 本机地址 ----
    lv_obj_t* top = make_card(root, 0, 0, cw, 64);

    s_dot = lv_obj_create(top);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_pos(s_dot, 20, 26);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_DOT_OFF), 0);

    s_lbl_state = make_label(top, &font_cn_16, 0xFFFFFF);
    lv_obj_set_pos(s_lbl_state, 44, 22);
    lv_label_set_text(s_lbl_state, "开启监听…");

    s_lbl_addr = make_label(top, &font_cn_16, COL_TEXT_DIM);
    lv_obj_align(s_lbl_addr, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_label_set_text(s_lbl_addr, "");

    //---- 记录卡: 标题 + 活动行 + 历史列表 ----
    lv_obj_t* card = make_card(root, 0, 72, cw, ch - 72);
    lv_obj_t* title = make_label(card, &font_cn_16, COL_TEXT_DIM);
    lv_label_set_text(title, "传输记录");
    lv_obj_set_pos(title, 16, 10);

    s_rows = lv_obj_create(card);
    lv_obj_remove_style_all(s_rows);
    lv_obj_set_pos(s_rows, 12, 36);
    lv_obj_set_size(s_rows, cw - 24, ch - 72 - 48);
    lv_obj_set_style_pad_all(s_rows, 0, 0);
    lv_obj_set_flex_flow(s_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_rows, 4, 0);
    lv_obj_set_scrollbar_mode(s_rows, LV_SCROLLBAR_MODE_AUTO);

    s_hist_n = 0;
    s_rows_dirty = 1;
    s_shown_state = -1;
    s_was_rx = 0;

    //先取快照再定基线: 地址/链路按当前实际状态渲染(修"IP时有时无"——
    //栈就绪事件发生在进应用之前的场合, gen不再变化, 不能只靠事件驱动)
    gui_model_snapshot(&s_snap);
    s_last_xfer = s_snap.gen_xfer;
    s_last_net = s_snap.gen_net;
    s_last_res_gen = s_snap.xfer_res_gen;   //结果基线: 不回放进入前的旧结果

    char buf[48];
    if (s_snap.net_up)
        snprintf(buf, sizeof(buf), "本机 %s : 69", s_snap.net_ip);
    else
        snprintf(buf, sizeof(buf), "网络未就绪");
    lv_label_set_text(s_lbl_addr, buf);

    post_cmd(APP_CMD_NET_START);            //进入即开监听(结果经gen_xfer回)
}

static void app_on_close(void)
{
    post_cmd(APP_CMD_NET_STOP);             //退出即停监听(会话≤1个超时周期中止)
}

// -----------------------------------------------------------------------------
//记录列表重建(仅在入新记录/进出RX/开屏时发生; 进度刷新只改活动行控件)
static void rebuild_rows(void)
{
    lv_obj_clean(s_rows);

    //活动行(RX中): 名称 + 进度条 + 百分比/速率
    if (s_snap.xfer_state == XFER_RX)
    {
        lv_obj_t* row = lv_obj_create(s_rows);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 42);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x12303A), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_CYAN), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        s_live_name = make_label(row, &font_cn_16, 0xFFFFFF);
        lv_label_set_long_mode(s_live_name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_live_name, lv_pct(58));
        lv_obj_set_pos(s_live_name, 6, 2);
        lv_label_set_text(s_live_name, s_snap.xfer_file);

        s_live_bar = lv_bar_create(row);
        lv_obj_set_size(s_live_bar, lv_pct(58), 8);
        lv_obj_align(s_live_bar, LV_ALIGN_LEFT_MID, 6, 12);
        lv_bar_set_range(s_live_bar, 0, 100);
        lv_bar_set_value(s_live_bar, 0, LV_ANIM_OFF);
        //TFTP收流期间总量未知(WRQ不带尺寸): 进度条先藏, total就位后显示
        lv_obj_add_flag(s_live_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_live_bar, lv_color_hex(0x0B1120), 0);
        lv_obj_set_style_bg_opa(s_live_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_live_bar, 4, 0);
        lv_obj_set_style_bg_color(s_live_bar, lv_color_hex(COL_CYAN),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_live_bar, LV_OPA_COVER, LV_PART_INDICATOR);

        s_live_info = make_label(row, &font_cn_16, COL_CYAN);
        lv_obj_align(s_live_info, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_label_set_text(s_live_info, "0%");
    }

    //历史记录(最新在上)
    for (int i = 0; i < s_hist_n; i++)
    {
        const XferRec* r = &s_hist[i];
        lv_obj_t* row = lv_obj_create(s_rows);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 30);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_CARD_EDGE), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* ic = make_label(row, &lv_font_montserrat_16,
                                  r->ok ? COL_LIME : COL_RED);
        lv_label_set_text(ic, r->ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 4, 0);

        char b1[16], info[80];
        fmt_size(b1, sizeof(b1), r->total);
        if (r->msg[0])
            snprintf(info, sizeof(info), "%s · %s", b1, r->msg);
        else
            snprintf(info, sizeof(info), "%s", b1);

        lv_obj_t* nm = make_label(row, &font_cn_16, 0xFFFFFF);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nm, lv_pct(52));
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 26, 0);
        lv_label_set_text(nm, r->name[0] ? r->name : "(系统)");

        lv_obj_t* in = make_label(row, &font_cn_16, COL_TEXT_DIM);
        lv_label_set_long_mode(in, LV_LABEL_LONG_DOT);
        lv_obj_set_width(in, lv_pct(40));
        lv_obj_align(in, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_label_set_text(in, info);
    }

    //空态
    if (s_hist_n == 0 && s_snap.xfer_state != XFER_RX)
    {
        lv_obj_t* empty = make_label(s_rows, &font_cn_16, COL_TEXT_DIM);
        lv_label_set_text(empty, "暂无传输记录  (PC: net_push.py / tftp put)");
        lv_obj_set_style_text_opa(empty, LV_OPA_70, 0);
    }
}

//新结果入记录(最新在上, 容量截断)
static void hist_push(void)
{
    XferRec rec;
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.name, s_snap.xfer_res_name, sizeof(rec.name) - 1);
    rec.total = s_snap.xfer_total;
    rec.ok = (s_snap.xfer_res == 1);
    strncpy(rec.msg, s_snap.xfer_res_msg, sizeof(rec.msg) - 1);

    if (s_hist_n < XFER_HIST_MAX)
        s_hist_n++;
    memmove(&s_hist[1], &s_hist[0], (size_t)(s_hist_n - 1) * sizeof(XferRec));
    s_hist[0] = rec;
}

//顶卡链路状态(状态变化时才重写)
static void refresh_link(void)
{
    const char* txt;
    char buf[48];
    uint32_t dot_col = COL_DOT_OFF, txt_col = 0xFFFFFF;

    //WAIT(栈初始化中)须先于net_up判断: 等待期间net_up必然为0
    if (s_snap.xfer_state == XFER_WAIT)
    {
        txt = "网络初始化…";
        dot_col = COL_CYAN;
        txt_col = COL_TEXT_DIM;
    }
    else if (!s_snap.net_up)
    {
        txt = "网络未就绪";
        dot_col = COL_RED;
        txt_col = COL_TEXT_DIM;
    }
    else
    {
        switch (s_snap.xfer_state)
        {
        case XFER_LISTEN:
            txt = "等待PC连接";
            dot_col = COL_CYAN;
            txt_col = COL_CYAN;
            break;
        case XFER_CONN:
        case XFER_RX:
            snprintf(buf, sizeof(buf), "已连接 %s", s_snap.xfer_peer);
            txt = buf;
            dot_col = COL_CYAN;
            break;
        case XFER_DONE:
            txt = "传输完成";
            dot_col = COL_LIME;
            txt_col = COL_LIME;
            break;
        case XFER_ERR:
            txt = "传输失败";
            dot_col = COL_RED;
            txt_col = COL_RED;
            break;
        default:
            txt = "已停止";
            dot_col = COL_DOT_OFF;
            txt_col = COL_TEXT_DIM;
            break;
        }
    }

    lv_obj_set_style_bg_color(s_dot, lv_color_hex(dot_col), 0);
    lv_label_set_text(s_lbl_state, txt);
    lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(txt_col), 0);
}

//活动行进度(每拍刷新, 值变才重写)。TFTP收流期间total=0(尺寸未知): 只显示
//已收字节+速率, 进度条保持隐藏; 落SD阶段net侧带回total -> 进度条出现并置满
static void refresh_live(void)
{
    if (s_live_bar == 0)
        return;
    char b1[16], b2[16], buf[64];
    fmt_size(b1, sizeof(b1), s_snap.xfer_done);

    if (s_snap.xfer_total > 0)
    {
        const unsigned pct = s_snap.xfer_done == s_snap.xfer_total ? 100u
            : (unsigned)((uint64_t)s_snap.xfer_done * 100u
                         / s_snap.xfer_total);
        lv_obj_clear_flag(s_live_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_live_bar, (int32_t)pct, LV_ANIM_OFF);
        fmt_size(b2, sizeof(b2), s_snap.xfer_total);
        if (s_snap.xfer_msg[0])
            snprintf(buf, sizeof(buf), "%u%%  %s/%s %s", pct, b1, b2,
                     s_snap.xfer_msg);
        else
            snprintf(buf, sizeof(buf), "%u%%  %s/%s", pct, b1, b2);
    }
    else
    {
        //收流中(总量未知): "1.2 MB  345 KB/s"
        snprintf(buf, sizeof(buf), "%s%s%s", b1,
                 s_snap.xfer_msg[0] ? "  " : "", s_snap.xfer_msg);
    }
    lv_label_set_text(s_live_info, buf);
}

static void app_refresh(void)
{
    gui_model_snapshot(&s_snap);

    if (s_snap.gen_net != s_last_net)       //栈状态(开页后栈才就绪的场合)
    {
        s_last_net = s_snap.gen_net;
        char buf[48];
        if (s_snap.net_up)
            snprintf(buf, sizeof(buf), "本机 %s : 69", s_snap.net_ip);
        else
            snprintf(buf, sizeof(buf), "网络未就绪");
        lv_label_set_text(s_lbl_addr, buf);
    }

    //结果按代数入记录(不依赖瞬态事件顺序——修"完成时有时无")
    if (s_snap.xfer_res_gen != s_last_res_gen)
    {
        s_last_res_gen = s_snap.xfer_res_gen;
        if (s_snap.xfer_res != 0)
        {
            hist_push();
            s_rows_dirty = 1;
        }
    }

    const int is_rx = (s_snap.xfer_state == XFER_RX);
    if (is_rx != s_was_rx)                  //进出RX: 活动行出现/消失
    {
        s_was_rx = is_rx;
        s_rows_dirty = 1;
    }

    if (s_snap.xfer_state != s_shown_state)
    {
        s_shown_state = s_snap.xfer_state;
        refresh_link();
    }

    if (s_rows_dirty)
    {
        s_rows_dirty = 0;
        rebuild_rows();
    }

    if (is_rx)
        refresh_live();
}

// -----------------------------------------------------------------------------
const LauncherApp app_transfer = {
    .name       = "文件传输",
    .icon       = LV_SYMBOL_UPLOAD,
    .on_open    = app_on_open,
    .on_close   = app_on_close,
    .refresh    = app_refresh,
    .refresh_ms = 200,
    .accent    = 0xFFA14A,
};

// =============================================================================
// 后端(app_task上下文): 开/停监听 + 收满文件落SD(FatFs纪律执行侧)
// =============================================================================

//落SD: 建父目录(任意深层路径) -> 写文件; 成功后按目录联动重扫
static int xfer_commit(const char* path, const uint8_t* data, uint32_t size)
{
    //父目录: "0:/apps/snake.bin" -> "0:/apps"(无'/'=非法路径, 防呆)
    char dir[48];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char* slash = strrchr(dir, '/');
    if (slash == 0)
        return -100;
    *slash = 0;
    if (sd_mkdirs(dir) != 0)
    {
        xil_printf("[net] mkdirs %s failed\r\n", dir);
        return -101;
    }
    return sd_write_file(path, data, size);
}

// -----------------------------------------------------------------------------
// 落盘成功后的目录联动重扫: 去抖合并——压测连续推送时"每文件全量重扫"线性
// 叠加(几百文件后单次扫描数秒), app_task被拖住会抬高落SD回执时延甚至超时。
// 2s窗口内的多次触发合并为一次, 末次触发由app_transfer_tick(app_task 1s
// 节拍)在窗口过后补扫, 保证最后一次落盘后~3s内清单收敛(桌面图标稍迟可见)。
#define SCAN_DEBOUNCE_MS 2000
static TickType_t s_apps_scan_at, s_models_scan_at;
static int        s_apps_scan_pend, s_models_scan_pend;

static void scan_run(int apps)
{
    if (apps)
    {
        s_apps_scan_at = xTaskGetTickCount();
        s_apps_scan_pend = 0;
        ext_sdapp_scan();               //桌面SD应用图标重扫
        xil_printf("[xfer] apps rescan done\r\n");
    }
    else
    {
        s_models_scan_at = xTaskGetTickCount();
        s_models_scan_pend = 0;
        nn_models_scan();               //模型包重扫
        app_infer_pub_models();         //清单发布(推理应用模型面板当场刷新)
        app_infer_pub_model_state();    //重扫丢弃当前激活, 桥对齐到未激活
        xil_printf("[xfer] models rescan done\r\n");
    }
}

static void scan_kick(const char* path)
{
    if (strncmp(path, "0:/apps", 7) == 0)
    {
        if (xTaskGetTickCount() - s_apps_scan_at
            < pdMS_TO_TICKS(SCAN_DEBOUNCE_MS))
            s_apps_scan_pend = 1;       //窗口内: 合并到窗口后的补扫
        else
            scan_run(1);
    }
    else if (strncmp(path, "0:/models", 9) == 0)
    {
        if (xTaskGetTickCount() - s_models_scan_at
            < pdMS_TO_TICKS(SCAN_DEBOUNCE_MS))
            s_models_scan_pend = 1;
        else
            scan_run(0);
    }
}

//1s节拍(main.cpp app_task): 补扫去抖窗口内被合并的重扫请求
void app_transfer_tick(void)
{
    if (s_apps_scan_pend && xTaskGetTickCount() - s_apps_scan_at
        >= pdMS_TO_TICKS(SCAN_DEBOUNCE_MS))
        scan_run(1);
    if (s_models_scan_pend && xTaskGetTickCount() - s_models_scan_at
        >= pdMS_TO_TICKS(SCAN_DEBOUNCE_MS))
        scan_run(0);
}

void app_transfer_dispatch(const AppCmd* cmd)
{
    switch (cmd->type)
    {
    case APP_CMD_NET_START:
        if (net_server_start() != 0)    //栈判死(初始化卡死/任务没起来)
            gui_model_pub_xfer(XFER_ERR, "", 0, 0, "网络启动失败");
        break;
    case APP_CMD_NET_STOP:
        net_server_stop();
        break;
    case APP_CMD_NET_FILE:
    {
        //缓冲所有权自net任务过户至此: 写SD(无论成败)后由这里free。
        //落SD为异步(net任务已回监听): UI终态由这里发布, 失败反向补发ERROR
        const char* base = strrchr(cmd->path, '/');
        base = base ? base + 1 : cmd->path;
        xil_printf("[xfer] commit %s (%u)\r\n",
                   cmd->path, (unsigned)cmd->val);
        const int wrc = xfer_commit(cmd->path,
                                    (const uint8_t*)cmd->ptr,
                                    (uint32_t)cmd->val);
        free(cmd->ptr);
        xil_printf("[xfer] commit done rc=%d\r\n", wrc);

        if (wrc == 0)
        {
            gui_model_pub_xfer(XFER_DONE, base,
                               (uint32_t)cmd->val, (uint32_t)cmd->val, "已保存");
            //落SD成功后按目标目录联动刷新桌面/模型清单(推送即生效, 去抖见上)
            scan_kick(cmd->path);
        }
        else
        {
            gui_model_pub_xfer(XFER_ERR, base,
                               (uint32_t)cmd->val, (uint32_t)cmd->val, "SD写入失败");
            net_server_write_fail(cmd->xfer_ip, cmd->xfer_port);
        }
        break;
    }
    default:
        break;
    }
}
