// =============================================================================
// 12_ov5640_ncnn —— OV5640 摄像头 + ncnn 实时推理(分类/检测通用)
//
// 架构(hw/sw分离, 按功能组织):
//   hw/  直接操作PL端的包: ov5640(摄像头子系统, 自含SCCB+视频链路拉起),
//        display_ctrl(显示驱动+LCD识别), vdma_api, clk_wiz
//   sw/  纯软件包: sys(堆/计时/内存布局), storage(SD读写), capture(拍照存BMP),
//        nn(ncnn推理: 通用运行器+SD模型管理+后处理)
//
// 模型管理(加模型不改代码, 全在SD卡):
//   /models/<文件夹>/ 里放 model.cfg(档案) + model.param.bin + model.bin
//   + labels.txt(类别名), 串口命令列出/切换模型
//
// 交互模型(命令驱动, 串口115200):
//   开机初始化完成后停在菜单态(LCD保持摄像头实时预览), 按 s 才开始推理,
//   再按 s 停止并回到菜单。菜单/运行两态均可用的命令:
//     s  开始/停止推理循环
//     c  拍照存BMP到SD卡
//     l  列出SD卡上的模型
//     m  切换到下一个模型
//     0-9 按索引选择模型
//     h  重新打印命令菜单
// =============================================================================

extern "C" {
#include "xil_types.h"
#include "xil_printf.h"
#include "xuartps_hw.h"
#include "xparameters.h"
#include "xiltimer.h"
}

#include "sw/sys/mem_layout.h"
#include "sw/sys/sys_platform.h"
#include "sw/storage/sd_io.h"
#include "sw/capture/bmp_capture.h"
#include "hw/display_ctrl/disp_detect.h"
#include "hw/ov5640/cam_system.h"
#include "sw/nn/model_mgr.h"
#include "sw/nn/nn_runner.h"
#include "sw/nn/post_process.h"

// -----------------------------------------------------------------------------
// 应用配置
//PS UART0 基地址(USB串口, 命令输入)
#define UART_BASEADDR  XPAR_XUARTPS_0_BASEADDR

//检测模型: 推理后检测框的叠加显示时长(毫秒), 期间LCD保持实时
#define OVERLAY_MS  300

//类别画框颜色(按类别号取模取色, 适配任意数据集的labels)
static const u8 s_palette[16][3] = {
    {255,  64,  64}, { 64, 255,  64}, { 64,  64, 255}, {255, 255,  64},
    {255,  64, 255}, { 64, 255, 255}, {255, 128,   0}, {128,   0, 255},
    {  0, 255, 128}, {255,   0, 128}, {  0, 128, 255}, {128, 255,   0},
    {192, 192, 192}, {  0,   0, 128}, {128,   0,   0}, {  0, 128, 128},
};

// -----------------------------------------------------------------------------
// 应用初始化: 按依赖顺序拉起各模块
// 返回0成功; 负数=失败步骤(错误详情已由各模块打印)
static int app_init(void)
{
    //1.视频链路: 先决定显示尺寸, 再初始化摄像头/VDMA/显示
    //  (板上自识别LCD; 换HDMI等固定尺寸屏时, 改成常量赋值即可)
    u16 disp_w = 0, disp_h = 0;
    disp_detect_lcd_size(&disp_w, &disp_h);
    if (cam_system_init(disp_w, disp_h) != 0)
        return -1;

    //2.拍照: 按画面尺寸初始化BMP文件头
    bmp_capture_config((u32)cam_width(), (u32)cam_height());

    //3.SD卡: 模型和拍照都依赖文件系统
    if (sd_mount() != 0)
        return -2;

    //4.模型: 扫描SD卡/models/, 激活第一个
    if (nn_models_scan() == 0)
        return -3;
    if (nn_model_activate(0) != 0)
        return -4;

    return 0;
}

// -----------------------------------------------------------------------------
// 串口命令状态(轮询置标志, 由所在状态的主循环消费)
static int s_capture_req = 0;   //'c' 拍照
static int s_run_toggle  = 0;   //'s' 开始/停止
static int s_switch_req  = -1;  //'m'/'0'-'9' 模型切换; -1无请求
#define SWITCH_NEXT  (-2)       //切换到下一个

static const char* post_name(NnPostType p)
{
    if (p == NN_POST_CLASSIFY)      return "classify";
    if (p == NN_POST_DETECT_SSD)    return "detect";
    if (p == NN_POST_DETECT_RETINA) return "retinaface";
    return "?";
}

//命令菜单
static void print_menu(void)
{
    const NnModelProfile* prof = nn_current();
    xil_printf("------------- ov5640_ncnn -------------\r\n");
    if (prof)
        xil_printf(" model: [%d] %s (%s, %d classes)\r\n",
                   nn_active_index(), prof->name,
                   post_name(prof->post),
                   prof->num_classes);
    xil_printf(" s:start/stop  c:capture  l:list  m:next\r\n"
               " 0-9:select model  h:help\r\n");
    xil_printf("----------------------------------------\r\n");
}

static void uart_poll(void)
{
    while (XUartPs_IsReceiveData(UART_BASEADDR))
    {
        char c = (char)XUartPs_RecvByte(UART_BASEADDR);
        switch (c)
        {
        case 'c':
            s_capture_req = 1;
            break;
        case 's':
            s_run_toggle = 1;
            break;
        case 'h':
            print_menu();
            break;
        case 'l':
            for (int i = 0; i < nn_models_count(); i++)
                xil_printf("[%d] %s%s\r\n", i, nn_model_name(i),
                           i == nn_active_index() ? "  <- active" : "");
            break;
        case 'm':
            s_switch_req = SWITCH_NEXT;
            break;
        default:
            if (c >= '0' && c <= '9')
                s_switch_req = c - '0';
            break;
        }
    }
}

//执行挂起的模型切换请求(菜单态/运行态共用)
static void handle_switch(void)
{
    if (s_switch_req == -1)
        return;
    int target = (s_switch_req == SWITCH_NEXT)
               ? (nn_active_index() + 1) % nn_models_count()
               : s_switch_req;
    s_switch_req = -1;
    if (nn_model_activate(target) != 0)
    {
        //切换失败: 尝试恢复上一个模型, 再失败只能停机
        xil_printf("switch failed, restoring...\r\n");
        if (nn_model_activate(nn_active_index()) != 0)
        {
            xil_printf("no usable model, halt.\r\n");
            while (1) {}
        }
    }
}

// -----------------------------------------------------------------------------
// 结果打印: 按模型的后处理类型分派(业务输出的呈现方式属于应用层)
static void report_classify(const NnModelProfile* prof, int cycle, unsigned ms,
                            const ncnn::Mat& out)
{
    int k = prof->topk;
    if (k < 1)            k = 1;
    if (k > NN_TOP_K_MAX) k = NN_TOP_K_MAX;

    int idx[NN_TOP_K_MAX];
    float val[NN_TOP_K_MAX];
    if (nn_post_classify(out, k, idx, val) != 0)
        return;

    xil_printf("cycle %d: infer %u ms\r\n", cycle, ms);
    for (int i = 0; i < k; i++)
    {
        if (idx[i] < 0 || idx[i] >= prof->num_classes)
            continue;
        unsigned sc = (unsigned)(val[i] * 100.0f + 0.5f);
        xil_printf("   top%d %s %u%%\r\n", i + 1, prof->labels[idx[i]], sc);
    }
}

static int boxes_to_overlay(const NnModelProfile* prof, const NnDetBox boxes[], int nb,
                            CamOverlayRect rects[], int rects_max)
{
    int nr = 0;
    for (int i = 0; i < nb && nr < rects_max; i++)
    {
        const u8* col = s_palette[boxes[i].label % 16];
        rects[nr].x0 = boxes[i].x0;
        rects[nr].y0 = boxes[i].y0;
        rects[nr].x1 = boxes[i].x1;
        rects[nr].y1 = boxes[i].y1;
        rects[nr].r  = col[0];
        rects[nr].g  = col[1];
        rects[nr].b  = col[2];
        nr++;
    }
    (void)prof;
    return nr;
}

static void print_boxes(const NnModelProfile* prof, int cycle, unsigned ms,
                        const NnDetBox boxes[], int nb)
{
    xil_printf("cycle %d: infer %u ms, %d objects\r\n", cycle, ms, nb);
    for (int i = 0; i < nb; i++)
    {
        unsigned sc = (unsigned)(boxes[i].score * 100.0f + 0.5f);
        const char* nm = (boxes[i].label >= 0 && boxes[i].label < prof->num_classes)
                       ? prof->labels[boxes[i].label] : "?";
        xil_printf("   %s %u%%  box(%d,%d)-(%d,%d)\r\n", nm, sc,
                   boxes[i].x0, boxes[i].y0, boxes[i].x1, boxes[i].y1);
    }
}

static int report_detect(const NnModelProfile* prof, int cycle, unsigned ms,
                         const ncnn::Mat& out, int W, int H,
                         CamOverlayRect rects[], int rects_max)
{
    NnDetBox boxes[NN_DET_MAX];
    int nb = 0;
    nn_post_detect(out, W, H, prof, boxes, NN_DET_MAX, &nb);
    print_boxes(prof, cycle, ms, boxes, nb);
    return boxes_to_overlay(prof, boxes, nb, rects, rects_max);
}

static int report_retinaface(const NnModelProfile* prof, int cycle, unsigned ms,
                             const ncnn::Mat cls[3], const ncnn::Mat bbox[3],
                             const ncnn::Mat lmk[3], const NnLetterbox* geom,
                             CamOverlayRect rects[], int rects_max)
{
    NnDetBox boxes[NN_DET_MAX];
    int nb = 0;
    nn_post_retinaface(cls, bbox, lmk, geom, prof, boxes, NN_DET_MAX, &nb);
    print_boxes(prof, cycle, ms, boxes, nb);
    return boxes_to_overlay(prof, boxes, nb, rects, rects_max);
}

static void overlay_loop(CamOverlayRect rects[], int nr)
{
    XTime t2;
    XTime_GetTime(&t2);
    while (nr > 0)
    {
        if (cam_draw_overlays(rects, nr, 60) != 0)
            break;
        uart_poll();
        if (s_run_toggle || s_switch_req != -1)
            break;
        XTime tn;
        XTime_GetTime(&tn);
        if (time_diff_ms(t2, tn) >= OVERLAY_MS)
            break;
    }
}

// -----------------------------------------------------------------------------
// 拍照(菜单态/运行态共用: 取一帧存BMP, LCD全程实时不受影响)
static void do_capture(void)
{
    s_capture_req = 0;
    cam_grab_frame((u8*)MEM_WORK_BUFFER_ADDR, 0);
    bmp_capture_save((const u8*)MEM_WORK_BUFFER_ADDR);
}

int main()
{
    XTime t0, t1;
    int  cycle = 0;
    int  running = 0;

    //==== 一.初始化 ====
    if (app_init() != 0)
    {
        xil_printf("app init failed, halt.\r\n");
        while (1) {}
    }
    delay_ms(1000);   //等摄像头自动曝光/白平衡稳定

    const int W = cam_width();
    const int H = cam_height();

    //==== 二.命令驱动主循环: 菜单态 <-> 运行态 ====
    print_menu();
    while (1)
    {
        if (!running)
        {
            //---- 菜单态: 等命令(LCD保持摄像头实时预览) ----
            uart_poll();
            handle_switch();
            if (s_capture_req)
                do_capture();
            if (s_run_toggle)
            {
                s_run_toggle = 0;
                running = 1;
                cycle = 0;
                xil_printf("running, s=stop ...\r\n");
            }
            else
            {
                delay_ms(20);
            }
            continue;
        }

        //---- 运行态: 一轮业务 ----
        //0.上一周期(含叠加期)收到的命令先处理, 保证停止/切换即时生效
        uart_poll();
        if (s_run_toggle)
        {
            s_run_toggle = 0;
            running = 0;
            xil_printf("stopped, %d cycles done.\r\n", cycle);
            print_menu();
            continue;
        }

        //1.模型切换请求(切换耗时数秒, 期间LCD保持实时)
        handle_switch();

        const NnModelProfile* prof = nn_current();

        //2.等帧边界取一帧到工作缓存, 空闲期轮询串口
        cam_grab_frame((u8*)MEM_WORK_BUFFER_ADDR, uart_poll);

        //3.拍照命令(复用本周期已取到的帧)
        if (s_capture_req)
            do_capture();

        //4.前向(计时)
        ncnn::Mat out;
        ncnn::Mat cls[3], bbox[3], lmk[3];
        NnLetterbox geom;
        int is_retina = (prof && prof->post == NN_POST_DETECT_RETINA);
        XTime_GetTime(&t0);
        int err = 0;
        if (is_retina)
            err = nn_forward_retinaface((const u8*)MEM_WORK_BUFFER_ADDR, W, H,
                                        cls, bbox, lmk, &geom);
        else
            err = nn_forward_frame((const u8*)MEM_WORK_BUFFER_ADDR, W, H, out);
        XTime_GetTime(&t1);
        cycle++;

        //5.推理后也查一次串口(推理期间按下的键此时生效)
        uart_poll();
        if (s_run_toggle)
        {
            s_run_toggle = 0;
            running = 0;
            xil_printf("stopped, %d cycles done.\r\n", cycle);
            print_menu();
            continue;
        }
        if (s_switch_req != -1 || s_capture_req)
        {
            handle_switch();
            if (s_capture_req)
                do_capture();
            prof = nn_current();
            is_retina = (prof && prof->post == NN_POST_DETECT_RETINA);
        }

        if (err != 0 || prof == 0 || (!is_retina && out.empty()))
        {
            xil_printf("cycle %d: forward error %d\r\n", cycle, err);
            delay_ms(500);
            continue;
        }

        //6.后处理+打印(分类/SSD/RetinaFace 由档案决定)
        if (prof->post == NN_POST_CLASSIFY)
        {
            report_classify(prof, cycle, time_diff_ms(t0, t1), out);
        }
        else
        {
            CamOverlayRect rects[NN_DET_MAX];
            int nr = 0;
            if (prof->post == NN_POST_DETECT_RETINA)
                nr = report_retinaface(prof, cycle, time_diff_ms(t0, t1),
                                       cls, bbox, lmk, &geom, rects, NN_DET_MAX);
            else
                nr = report_detect(prof, cycle, time_diff_ms(t0, t1), out, W, H,
                                   rects, NN_DET_MAX);
            overlay_loop(rects, nr);
        }
    }

    return 0;
}
