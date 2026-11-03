// =============================================================================
// vidwin —— 摄像头画面LCD窗口实现
//
// 帧率来源即摄像头帧率(720p约30fps), 任务以20ms节拍轮询环形缓冲写指针:
// 帧号没变跳过(不重复降采样), 变了才整窗重写。降采样用预建横向源坐标表,
// 纵向按行取整, RGB888(BGR内存序)两边一致纯字节搬运。
// 单帧成本(640x360): 降采样约3ms + cache失效/冲洗约1ms, 30fps约12%单核CPU。
// =============================================================================

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "vidwin.h"
#include "hw/chain/cam_chain.h"
#include "sw/impl/cam/cam.h"
#include "sw/impl/lcd/lcd.h"
#include "sw/impl/lcd/ovl_draw.h"
#include "sw/impl/port/port.h"
#include "sw/app/launcher.h"
#include "sw/app/bridge.h"

//应用屏布局镜像(launcher.c的顶栏+边距, 两处必须一致):
//内容区LCD原点 = (LAUNCHER_APP_PAD, LAUNCHER_APP_TOPBAR_H+LAUNCHER_APP_PAD)
#define SHELL_ORIGIN_Y  (LAUNCHER_APP_TOPBAR_H + LAUNCHER_APP_PAD)

//任务参数(栈深单位是字word=4字节; 叠加路径含快照/画框/snprintf, 留足余量)
#define VID_TASK_STACK  4096   //字(16KB)
#define VID_TASK_PRIO   1
#define VID_POLL_TICKS     2   //轮询节拍20ms(满帧率~30fps)
#define VID_SETTLE_TICKS  50   //启动后等自动曝光/白平衡稳定(500ms)

static int s_ok = 0;                 //布局可用(vidwin_init成功)
static u16 s_lcd_w, s_lcd_h;

//视频洞(LCD屏坐标; 任务运行期间恒定, start_centered时计算)
static u16 s_vx, s_vy, s_vw, s_vh;

//视频任务生命周期(控制方启停; alive/run两标志握手停任务)
static TaskHandle_t s_task;
static volatile int s_task_run;      //0=请求退出
static volatile int s_task_alive;

//运行统计(fps窗口/EMA耗时, 仅视频任务写, 读取方无锁近似值)
static volatile u32 s_fps;
static volatile u32 s_us;

//显示开关(gui任务写, vid任务读; 单字读写在A9上原子, volatile足够)
static volatile int s_en = 0;        //默认停笔: 应用开了实时画面才enable

//开窗水平对齐(gui写, 开窗时读): 0=居中 1=靠左(内容区左缘)
static volatile int s_align_left = 0;

//HDMI描框开关(gui写, vid任务读; 默认开——推理应用三态按钮的"带框"档,
//关档输出干净画面, 单字读写原子)
static volatile int s_hdmi_ovl = 1;

//横向源坐标表(降采样用, 建表后内核内循环无除法)
#define VID_MAX_W  800
static u16 s_xtab[VID_MAX_W];

//洞列段逐行冲缓存(写透DDR); 只碰洞宽, 不波及同行LVGL区域
static void flush_hole(u8* fb, u32 fb_stride);

// -----------------------------------------------------------------------------
// 降采样整窗重写: 源帧逐行失效cache后最近邻取样, 写LCD帧缓冲窗口行
// inv_src: 源是DMA帧环帧=1(读前须失效); 源是CPU缓冲(推理回放)=0(失效会
// 丢弃未回写的脏行, 绝不可失效, 且双核SCU保证核间可见)
// do_flush: 0=只写缓存不冲刷(叠加框后由flush_hole一次上屏, VDMA不会扫出
// "无框中间帧"——框以30Hz闪烁的根因); 1=写完即冲
static void decimate_blit(const u8* src, u8* fb, u32 fb_stride, int inv_src,
                          int do_flush)
{
    const int W = cam_chain_width();
    const int H = cam_chain_height();
    const u16 vw = s_vw;
    const u16 vh = s_vh;
    const u32 src_stride = (u32)W * 3;

    u8* dst = fb + (u32)s_vy * fb_stride + (u32)s_vx * 3;
    for (int dy = 0; dy < vh; dy++)
    {
        const u8* srow = src + (u32)(dy * H / vh) * src_stride;
        if (inv_src)
            port_cache_invalidate((void*)srow, src_stride); //只碰要读的行
        u8* drow = dst + (u32)dy * fb_stride;
        for (int dx = 0; dx < vw; dx++)
        {
            const u8* p = srow + (u32)s_xtab[dx] * 3;
            drow[0] = p[0];
            drow[1] = p[1];
            drow[2] = p[2];
            drow += 3;
        }
    }
    if (do_flush)
        flush_hole(fb, fb_stride);
}

//洞列段逐行冲缓存(写透DDR); 只碰洞宽, 不波及同行LVGL区域
static void flush_hole(u8* fb, u32 fb_stride)
{
    for (int dy = 0; dy < s_vh; dy++)
        port_cache_flush(fb + (u32)(s_vy + dy) * fb_stride + (u32)s_vx * 3,
                         (u32)s_vw * 3);
}

// -----------------------------------------------------------------------------
// 检测框叠加引擎(推理应用)
//
// 推理~5fps发布目标框(gen_det), 视频帧率~30fps: 若直画, 框每200ms瞬移一格。
// 平滑策略(全部在本任务, 每新帧节拍一次):
//   1.新目标集到来 -> 与"当前在画的框"IoU配对(>=OVL_MATCH_IOU%算同一目标)
//   2.配对成功 -> 当前坐标向目标指数逼近(OVL_SMOOTH_PCT%/帧, ~3帧滑到位)
//   3.配对失败的旧框 -> 冻结位置保留OVL_GRACE_F帧后撤除(线宽收细渐隐)
//   4.未被配对的新目标 -> 以80%尺寸从中心放大就位
// 三出口共用同一份平滑坐标: LCD洞(live) / 摄像头帧环(HDMI) / 静态回放(精确)

#define OVL_MAX        16     //同时在画的框数上限(与GUI_DET_BOX_MAX一致)
#define OVL_SMOOTH_DEF 60     //插值强度默认: 每视频帧向目标靠近的百分比
                               //(推理~9.5Hz时60%约1.5帧滑到位, 兼顾顺滑与跟手)
static volatile int s_smooth_pct = OVL_SMOOTH_DEF;  //gui滑块写, vid任务读
#define OVL_MATCH_IOU  30     //IoU配对下限(%)
#define OVL_GRACE_F    4      //目标消失后的宽限帧数

typedef struct {
    int x0, y0, x1, y1;       //当前画的位置(源帧像素坐标)
    int tx0, ty0, tx1, ty1;   //推理给出的目标位置
    char name[12];
    u8   cls, score;
    int  grace;               //0=活跃; >0=消失宽限倒计
} OvlBox;

static OvlBox s_ob[OVL_MAX];
static int    s_ob_n;
static u32    s_ob_gen;               //已消费的gen_det
static int    s_ob_src_w, s_ob_src_h; //目标坐标所属源帧尺寸

//通用绘制描述(live平滑框/回放精确框共用)
typedef struct {
    int x0, y0, x1, y1;       //源帧像素坐标
    const char* name;
    int cls, score;
    int thin;                 //渐隐: 0=正常 1/2=线宽收细档位
} OvlRc;

//矩形交并比(%)
static int iou_pct(int ax0, int ay0, int ax1, int ay1,
                   int bx0, int by0, int bx1, int by1)
{
    const int ix0 = ax0 > bx0 ? ax0 : bx0;
    const int iy0 = ay0 > by0 ? ay0 : by0;
    const int ix1 = ax1 < bx1 ? ax1 : bx1;
    const int iy1 = ay1 < by1 ? ay1 : by1;
    const int iw = ix1 - ix0 + 1, ih = iy1 - iy0 + 1;
    if (iw <= 0 || ih <= 0)
        return 0;
    const int inter = iw * ih;
    const int area = (ax1 - ax0 + 1) * (ay1 - ay0 + 1)
                   + (bx1 - bx0 + 1) * (by1 - by0 + 1) - inter;
    return inter * 100 / area;
}

//消费bridge的det分组: 配对/新建/宽限(仅gen_det变化时做匹配)
//快照为静态: GuiSnapshot约4.3KB, 放栈上会溢出vid任务8KB栈(仅vid任务调用)
static void ovl_pull(void)
{
    static GuiSnapshot s;
    gui_model_snapshot(&s);
    if (s.gen_det == s_ob_gen)
        return;
    s_ob_gen = s.gen_det;
    s_ob_src_w = s.det_src_w;
    s_ob_src_h = s.det_src_h;

    int n = s.det_box_n;
    if (n > GUI_DET_BOX_MAX)
        n = GUI_DET_BOX_MAX;
    u8 used[GUI_DET_BOX_MAX] = {0};

    for (int i = 0; i < s_ob_n; i++)
    {
        int best = -1, best_iou = OVL_MATCH_IOU;
        for (int j = 0; j < n; j++)
        {
            if (used[j])
                continue;
            const GuiDetBox* d = &s.det_box[j];
            const int v = iou_pct(s_ob[i].x0, s_ob[i].y0, s_ob[i].x1, s_ob[i].y1,
                                  d->x0, d->y0, d->x1, d->y1);
            if (v >= best_iou)
            {
                best_iou = v;
                best = j;
            }
        }
        if (best >= 0)
        {
            const GuiDetBox* d = &s.det_box[best];
            used[best] = 1;
            s_ob[i].tx0 = d->x0;  s_ob[i].ty0 = d->y0;
            s_ob[i].tx1 = d->x1;  s_ob[i].ty1 = d->y1;
            memcpy(s_ob[i].name, d->name, sizeof(s_ob[i].name));
            s_ob[i].cls = d->cls;
            s_ob[i].score = d->score;
            s_ob[i].grace = 0;
        }
        else if (s_ob[i].grace == 0)
        {
            s_ob[i].grace = OVL_GRACE_F;     //进入宽限(位置冻结渐隐)
        }
    }

    //未被配对的新目标: 以80%尺寸出现(中心不变)
    for (int j = 0; j < n && s_ob_n < OVL_MAX; j++)
    {
        if (used[j])
            continue;
        const GuiDetBox* d = &s.det_box[j];
        OvlBox* b = &s_ob[s_ob_n++];
        const int cx = (d->x0 + d->x1) / 2, cy = (d->y0 + d->y1) / 2;
        const int w = (d->x1 - d->x0) * 8 / 10, h = (d->y1 - d->y0) * 8 / 10;
        b->x0 = cx - w / 2;  b->y0 = cy - h / 2;
        b->x1 = b->x0 + w;   b->y1 = b->y0 + h;
        b->tx0 = d->x0;  b->ty0 = d->y0;  b->tx1 = d->x1;  b->ty1 = d->y1;
        memcpy(b->name, d->name, sizeof(b->name));
        b->cls = d->cls;
        b->score = d->score;
        b->grace = 0;
    }
}

//每帧节拍: 宽限倒计/到期删除 + 活跃框向目标插值
static void ovl_advance(void)
{
    int n = 0;
    for (int i = 0; i < s_ob_n; i++)
    {
        OvlBox* b = &s_ob[i];
        if (b->grace > 0)
        {
            b->grace--;
            if (b->grace == 0)
                continue;                       //宽限到期: 撤除
        }
        else
        {
            b->x0 += (b->tx0 - b->x0) * s_smooth_pct / 100;
            b->y0 += (b->ty0 - b->y0) * s_smooth_pct / 100;
            b->x1 += (b->tx1 - b->x1) * s_smooth_pct / 100;
            b->y1 += (b->ty1 - b->y1) * s_smooth_pct / 100;
        }
        s_ob[n++] = *b;                          //compact式搬移
    }
    s_ob_n = n;
}

//标签文本组装("name 87%")
static void ovl_txt(char* buf, int cap, const OvlRc* b)
{
    snprintf(buf, (size_t)cap, "%s %d%%", b->name, b->score);
}

//把一组框画进LCD视频洞(源帧坐标自动缩放); live平滑框与回放精确框共用
//只写缓存不上屏——调用方画完后统一flush_hole(防VDMA扫出无框中间帧)
static void ovl_draw_lcd(u8* fb, u32 fb_stride, int fb_w, int fb_h,
                         const OvlRc* rc, int n, int sw, int sh)
{
    (void)fb_stride;
    if (sw <= 0 || sh <= 0)
        return;
    for (int i = 0; i < n; i++)
    {
        const OvlRc* b = &rc[i];
        const int ax0 = s_vx + b->x0 * s_vw / sw;
        const int ay0 = s_vy + b->y0 * s_vh / sh;
        const int ax1 = s_vx + b->x1 * s_vw / sw;
        const int ay1 = s_vy + b->y1 * s_vh / sh;
        const u32 col = ovl_cls_color(b->cls);
        const int t = b->thin ? b->thin : 2;

        char txt[18];
        ovl_txt(txt, sizeof(txt), b);
        const int ly = ay0 + 2;                  //标签条画在框内左上角(贴框顶)
        ovl_draw_box(fb, fb_w, fb_h, ax0, ay0, ax1, ay1, t, col);
        ovl_draw_label(fb, fb_w, fb_h, ax0 + t, ly, txt, col, 1);
    }
    //不在此处冲缓存: 调用方画完所有框后flush_hole一次上屏(防中间帧闪烁)
}

//HDMI路径: 与LCD同一份平滑框+标签描进"刚写完"的帧环帧(摄像头VDMA
//直通HDMI, 两屏即一致)。缓冲寿命约100ms(3缓冲@30fps写入, 60fps扫描),
//描进的框可持续多帧; 每框单区域 失效->画->冲 一遍完成, 不做三遍行段
static void ovl_draw_hdmi(const u8* src)
{
    if (s_ob_n == 0 || s_ob_src_w <= 0)
        return;
    const int W = cam_chain_width();
    const int H = cam_chain_height();
    u8* frame = (u8*)src;
    const u32 stride = (u32)W * 3;

    for (int i = 0; i < s_ob_n; i++)
    {
        OvlRc rc;
        rc.x0 = s_ob[i].x0;  rc.y0 = s_ob[i].y0;
        rc.x1 = s_ob[i].x1;  rc.y1 = s_ob[i].y1;
        rc.name = s_ob[i].name;
        rc.cls = s_ob[i].cls;
        rc.score = s_ob[i].score;
        rc.thin = (s_ob[i].grace > 0) ? (s_ob[i].grace > 2 ? 1 : 2) : 0;

        char txt[18];
        ovl_txt(txt, sizeof(txt), &rc);
        const int lh = 40;                        //scale2: (16+2*2)*2
        const int ly = rc.y0 + 6;                 //标签条画在框内左上角
        const int t = rc.thin ? rc.thin : 3;      //720p全幅, 3px对齐LCD权重

        //触及区域: 框外接矩形 + 标签条右延(标签可能比框宽)
        int rx1 = rc.x1 > rc.x0 + 430 ? rc.x1 : rc.x0 + 430;
        int ry1 = rc.y1 > ly + lh ? rc.y1 : ly + lh;
        if (rx1 >= W) rx1 = W - 1;
        if (ry1 >= H) ry1 = H - 1;
        if (rc.x0 < 0 || rc.y0 < 0 || rc.x0 >= rx1 || rc.y0 >= ry1)
            continue;

        u8* reg = frame + (size_t)rc.y0 * stride + (size_t)rc.x0 * 3;
        const size_t reg_len = (size_t)(ry1 - rc.y0 + 1)
                             * (size_t)(rx1 - rc.x0 + 1) * 3;
        port_cache_invalidate(reg, reg_len);
        ovl_draw_box(frame, W, H, rc.x0, rc.y0, rc.x1, rc.y1, t,
                     ovl_cls_color(rc.cls));
        ovl_draw_label(frame, W, H, rc.x0 + t, ly, txt,
                       ovl_cls_color(rc.cls), 2);
        port_cache_flush(reg, reg_len);
    }
}

//live路径: 平滑框 -> 洞(blit之后调用, 覆盖在干净画面上)
static void ovl_draw_live(u8* fb, u32 fb_stride, int fb_w, int fb_h)
{
    if (s_ob_n == 0 || s_ob_src_w <= 0)
        return;
    OvlRc rc[OVL_MAX];
    for (int i = 0; i < s_ob_n; i++)
    {
        rc[i].x0 = s_ob[i].x0;  rc[i].y0 = s_ob[i].y0;
        rc[i].x1 = s_ob[i].x1;  rc[i].y1 = s_ob[i].y1;
        rc[i].name = s_ob[i].name;
        rc[i].cls = s_ob[i].cls;
        rc[i].score = s_ob[i].score;
        rc[i].thin = (s_ob[i].grace > 0) ? (s_ob[i].grace > 2 ? 1 : 2) : 0;
    }
    ovl_draw_lcd(fb, fb_stride, fb_w, fb_h, rc, s_ob_n,
                 s_ob_src_w, s_ob_src_h);
}



// -----------------------------------------------------------------------------
static void vid_task(void* arg)
{
    (void)arg;

    //内核后续可能换NEON实现, 按工程惯例先登记FPU上下文(configUSE_TASK_FPU_SUPPORT=1)
    portTASK_USES_FLOATING_POINT();

    vTaskDelay(VID_SETTLE_TICKS);   //等摄像头自动曝光/白平衡稳定

    u32 fb_stride = 0;
    u16 fb_w = 0, fb_h = 0;
    u8* fb = lcd_frame(&fb_stride, &fb_w, &fb_h);
    if (fb == 0 || s_vw == 0
                || s_vx + s_vw > fb_w || s_vy + s_vh > fb_h)
    {
        port_log("[vid] framebuffer/window unavailable, vid task exit\r\n");
        s_task_alive = 0;
        vTaskDelete(0);
        return;
    }

    //横向源坐标表: 输出列 -> 源列(最近邻, 按本次链路宽度构建)
    const int W = cam_chain_width();
    if (W <= 0)
    {
        port_log("[vid] no cam chain, vid task exit\r\n");
        s_task_alive = 0;
        vTaskDelete(0);
        return;
    }
    for (int dx = 0; dx < s_vw; dx++)
        s_xtab[dx] = (u16)(dx * W / s_vw);

    port_log("[vid] window %ux%u @(%u,%u)\r\n",
               s_vw, s_vh, s_vx, s_vy);

    int   last_idx = -1;
    u32   win_push = 0;                 //fps统计窗口内推送帧数
    u32   us_ema = 0;                   //单帧耗时指数平滑
    TickType_t t_win = xTaskGetTickCount();
    int   stall = 0;                    //停滞诊断计数(连续无画帧的循环数)

    while (s_task_run && cam_chain_running())
    {
        vTaskDelay(VID_POLL_TICKS);

        //停滞诊断: 连续2秒(100x20ms)没有可画的新帧时, 打印循环状态定位
        //卡点(帧环冻结/显示关/任务异常各分支的数值特征不同)
        if (++stall >= 100)
        {
            stall = 0;
            port_log("[vid] stall: en=%d run=%d stream=%d widx=%u last=%d\r\n",
                     s_en, s_task_run, cam_chain_running(),
                     (unsigned)cam_chain_write_index(), last_idx);
        }

        const u8* src;
        const int idx = cam_snap_latest(&src);
        if (idx < 0 || idx == last_idx)
            continue;                            //帧没更新, 不重复写
        last_idx = idx;

        //检测框平滑: 免锁快读det代数把关, 推理从未发布过框时零桥开销
        if (s_ob_gen != gui_model_det_gen_fast())
            ovl_pull();
        ovl_advance();

        u64 t0 = port_time_us();
        if (s_en)
        {
            decimate_blit(src, fb, fb_stride, 1, 0);    //先写缓存不冲
            ovl_draw_live(fb, fb_stride, fb_w, fb_h);   //画平滑框+标签
            if (s_en)                        //双检: 关预览瞬间不冲半帧上屏
                flush_hole(fb, fb_stride);   //画面+框一次上屏
        }
        //HDMI: 同一组平滑框+标签描进帧环。注: genlock锁相下MM2S正在扫
        //该缓冲, 描框必然与光束竞速(残影/局部显示, 12工程同此特性),
        //属硬件通路固有限制, 彻底消除需PL侧叠加模块
        if (cam_chain_hdmi_get() && s_hdmi_ovl)
            ovl_draw_hdmi(src);
        u64 t1 = port_time_us();

        if (!s_en)
            continue;                            //停笔期不进统计(重开下帧即恢复)

        stall = 0;                               //画上了一帧: 停滞计数清零
        win_push++;

        const u32 us = (u32)(t1 - t0);
        us_ema = us_ema ? (us_ema * 3 + us) / 4 : us;

        //fps统计窗口(1s)滚动: 发布到数据桥 + 每10s串口实测报告
        if (xTaskGetTickCount() - t_win >= pdMS_TO_TICKS(1000))
        {
            s_fps = win_push;
            s_us  = us_ema;
            gui_model_pub_vid(s_fps, s_us);
            static u32 s_rpt = 0;
            if (++s_rpt % 10 == 0)
                port_log("[vid] %u fps, %u.%ums/frame\r\n",
                           (unsigned)s_fps, (unsigned)(s_us / 1000),
                           (unsigned)(s_us % 1000) / 100);
            win_push = 0;
            t_win = xTaskGetTickCount();
        }
    }

    port_log("[vid] task exit: run=%d stream=%d\r\n",
             s_task_run, cam_chain_running());
    s_task_alive = 0;
    vTaskDelete(0);                              //任务自删: 栈由空闲任务回收
}

// -----------------------------------------------------------------------------
int vidwin_init(u16 lcd_w, u16 lcd_h)
{
    if (lcd_w < VIDWIN_VID_W + 16 || lcd_h < VIDWIN_VID_H + LAUNCHER_APP_TOPBAR_H)
        return -1;

    s_lcd_w = lcd_w;
    s_lcd_h = lcd_h;
    s_vw = 0;                                    //未开窗
    s_fps = 0;
    s_us = 0;
    s_ok = 1;
    return 0;
}

int vidwin_hole_rect(u16* x, u16* y, u16* w, u16* h)
{
    if (!s_ok || s_vw == 0)
        return -1;
    if (x) *x = s_vx;
    if (y) *y = s_vy;
    if (w) *w = s_vw;
    if (h) *h = s_vh;
    return 0;
}

void vidwin_task_start_centered(void)
{
    if (!s_ok)
        return;

    //窗口: 顶部与内容区平齐; 水平按对齐模式(居中/靠左)
    s_vw = VIDWIN_VID_W;
    s_vh = VIDWIN_VID_H;
    s_vx = s_align_left ? (u16)LAUNCHER_APP_PAD
                        : (u16)((s_lcd_w - s_vw) / 2);
    s_vy = SHELL_ORIGIN_Y;

    if (s_task_alive)
        return;                                  //已在跑(幂等, 只重开窗)

    //alive由控制方先置1, 任务退出时清0: start/stop串行调用无竞态
    s_task_alive = 1;
    s_task_run = 1;
    if (xTaskCreate(vid_task, "vid", VID_TASK_STACK, 0, VID_TASK_PRIO,
                    &s_task) != pdPASS)
    {
        s_task_alive = 0;
        s_task_run = 0;
        port_log("[vid] task create failed\r\n");
    }
}

void vidwin_task_stop(void)
{
    if (!s_task_alive)
        return;
    s_task_run = 0;
    while (s_task_alive)                         //任务≤一个轮询周期内自行退出
        vTaskDelay(1);
    s_task_run = 0;
}

void vidwin_stats(u32* fps, u32* us)
{
    if (fps) *fps = s_fps;
    if (us)  *us  = s_us;
}

void vidwin_enable(int on)
{
    s_en = on ? 1 : 0;
}

int vidwin_enabled(void)
{
    return s_en;
}

void vidwin_align_left(int on)
{
    s_align_left = on ? 1 : 0;
}

//HDMI输出是否叠加检测框(三态按钮的"带框"档; gui任务直调, 仅标志位)
void vidwin_hdmi_ovl_set(int on)
{
    s_hdmi_ovl = on ? 1 : 0;
}

//检测框跟手速度(gui滑块直调, 实时生效; gui写vid读, 单字原子):
//每视频帧向推理目标坐标靠近的百分比, 10=很平滑但拖, 100=立即跳到位
void vidwin_ovl_smooth_set(int pct)
{
    if (pct < 10)  pct = 10;
    if (pct > 100) pct = 100;
    s_smooth_pct = pct;
}

int vidwin_hdmi_ovl_enabled(void)
{
    return s_hdmi_ovl;
}

//(当前未用: 预览关已改为黑屏纯加速, 不做标注回放; 保留作后续扩展)
int vidwin_show_annotated(const u8* frame)
{
    if (frame == 0 || !s_ok || s_vw == 0 || !s_task_alive)
        return -1;
    u32 stride = 0;
    u16 w = 0, h = 0;
    u8* fb = lcd_frame(&stride, &w, &h);
    if (fb == 0)
        return -1;

    //源是CPU缓冲(推理帧拷贝): 不失效(脏行会被丢弃), SCU保证核间一致
    decimate_blit(frame, fb, stride, 0, 0);

    //画桥里当前检测框: 与该帧同一次推理产出, 精确对位(无需平滑)
    //快照为静态: 约4.3KB, 且仅app_task调用(与ovl_pull的静态快照分属两个任务)
    static GuiSnapshot s;
    gui_model_snapshot(&s);
    if (s.det_box_n > 0 && s.det_src_w > 0)
    {
        OvlRc rc[GUI_DET_BOX_MAX];
        int n = s.det_box_n;
        if (n > GUI_DET_BOX_MAX)
            n = GUI_DET_BOX_MAX;
        for (int i = 0; i < n; i++)
        {
            rc[i].x0 = s.det_box[i].x0;  rc[i].y0 = s.det_box[i].y0;
            rc[i].x1 = s.det_box[i].x1;  rc[i].y1 = s.det_box[i].y1;
            rc[i].name = s.det_box[i].name;
            rc[i].cls = s.det_box[i].cls;
            rc[i].score = s.det_box[i].score;
            rc[i].thin = 0;
        }
        ovl_draw_lcd(fb, stride, (int)w, (int)h, rc, n,
                     s.det_src_w, s.det_src_h);
    }
    flush_hole(fb, stride);              //画面+框一次上屏(防中间帧闪烁)
    return 0;
}

void vidwin_paint_bg(void)
{
    if (!s_ok || s_vw == 0)
        return;
    u32 stride = 0;
    u16 w = 0, h = 0;
    u8* fb = lcd_frame(&stride, &w, &h);
    if (fb == 0)
        return;
    //洞区涂应用底色(与launcher卡片同源深蓝), 遮掉冻结的残帧
    ovl_fill(fb, (int)w, (int)h, s_vx, s_vy, s_vx + s_vw - 1, s_vy + s_vh - 1,
             0x141B2E);
    flush_hole(fb, stride);        //只刷洞列段(整行屏宽会殃及同行LVGL区)
}
