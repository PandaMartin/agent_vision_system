// =============================================================================
// cam_chain —— 摄像头+HDMI显示链路子系统实现
//
// 链路编排自原sw/cam_vdma/cam_vdma.cpp的cam_vdma_init逐行平移;
// 抓帧/叠加等帧访问逻辑归impl/cam(经本文件的帧环查询接口)
//
// 停启模型(热待机, 2026-10定稿): 反复"全停→全重配"实测S2MM会冻结
//(启动"成功"但写指针永不推进) —— 故stop只做 HDMI关+传感器睡眠,
//VDMA写通道保持武装等在SOF上(无数据无流量); start同尺寸一条指令
//唤醒传感器即恢复, 唤醒失败/尺寸变化才全重配(冷启动)。
//
// 撕裂修复(2026-10-18, PG020动态GenLock): 硬件参数VDMA1天生就是
// 防撕裂对(S2MM=动态主0x2, MM2S=动态从0x3, 内部genlock) ——
// 主设备写完一帧发帧号, 从设备只在主设备帧号处换缓冲, 且主设备
// 自动跳过从设备正显示的缓冲(三帧环下读写永不同帧) => HDMI无撕裂。
// 读写速率漂移(摄像头30fps vs HDMI时序29.5fps)曾致周期性撞帧撕裂。
// GenLock按HDMI状态动态挂/摘(GenlockEn位运行期可切, 通道不重启):
//   HDMI开: 读写GenlockEn=1, 硬件保证互斥;
//   HDMI关: 先清写通道GenlockEn(主设备变回纯自由循环, 从设备状态
//           与它彻底无关), 再停读通道 —— 常热模型(预览/拍照)不受影响。
// (历史上GenLock全开+停读通道曾致写指针停走: 从设备halt后灰码帧号
//  抖动, 主设备误判处处是"从设备缓冲"而全部跳过; 先解耦再停读即规避)
// =============================================================================

#include "cam_chain.h"

#include "xil_printf.h"
#include "xparameters.h"
#include "xaxivdma.h"
#include "sleep.h"

#include "hw/dev/ov5640.h"
#include "hw/ip/vdma.h"
#include "hw/ip/vtc_api.h"
#include "hw/ip/lcd_modes.h"

#include "hw/mem_layout.h"   //MEM_FRAME_BUFFER_BASE(三帧环固定地址)

//摄像头+HDMI链路固定使用VDMA1(摄像头S2MM写 + HDMI MM2S读)和VTC1(HDMI时序)
#define VDMA_BASEADDR       XPAR_XAXIVDMA_1_BASEADDR   //VDMA1基地址
#define DISP_VTC_BASEADDR   XPAR_XVTC_1_BASEADDR       //VTC1(HDMI时序)基地址

//链路启动后帧流校验: 写指针在此时间内不推进视为链路未真正出帧
//(传感器重初始化后出流较慢, 窗口留足)
#define FRAME_SYNC_TIMEOUT_MS  1000

//本模块私有的外设实例与状态
static XAxiVdma s_vdma;
static VtcDev   s_disp;
static int      s_width = 0;
static int      s_height = 0;
static int      s_hdmi = 0;      //HDMI输出(VTC1+VDMA读通道)是否开
static int      s_stream = 0;    //传感器出流中(0=热待机: 配置在,流停)

//传感器输出时序表: 尺寸 -> (DVP有效像素, 总像素)  [来自实验11]
//以后支持新分辨率在此加一行, 同时确认 lcd_modes.h 有对应显示模式
typedef struct
{
    u16 w, h;             //画面(显示)尺寸
    u16 cmos_h, cmos_v;   //DVP输出有效像素
    u16 total_h, total_v; //每行/每帧总像素(含消隐)
} CamTiming;

static const CamTiming s_cam_timings[] = {
    { 480,  272,  480,  272, 1800, 1000 },
    { 800,  480,  800,  480, 1800, 1000 },
    { 1024, 600, 1024, 600, 2200, 1000 },
    { 1280, 720, 1280, 720, 2570,  980 },   //HDMI默认档(参考正点原子28_ov5640_hdmi)
    { 1280, 800, 1280, 800, 2570,  980 },
};

static const CamTiming* find_timing(u16 w, u16 h)
{
    const int n = (int)(sizeof(s_cam_timings) / sizeof(s_cam_timings[0]));
    for (int i = 0; i < n; i++)
    {
        if (s_cam_timings[i].w == w && s_cam_timings[i].h == h)
            return &s_cam_timings[i];
    }
    return 0;
}

static int  wait_frame_sync(void);           //定义在start之后
static void full_stop(void);

int cam_chain_start(u16 width, u16 height)
{
    //幂等: 已配置同尺寸且流在跑 => 立即成功(常热模型, 退出相机后流不断)
    if (s_width != 0 && s_stream
        && s_width == (int)width && s_height == (int)height)
        return 0;

    //流曾断(故障/尺寸变化后残留): 全停后走冷启动
    if (s_width != 0)
        full_stop();

    //---- 1.显示端按传入尺寸查 VideoMode(时序参数都从这来) ----
    const VideoMode* mode = lcd_find_mode(width, height);
    if (mode == 0)
    {
        xil_printf("[cam] unsupported display size %u x %u "
                   "(no VideoMode in lcd_modes.h)\r\n", width, height);
        return -1;
    }

    //---- 2.传感器输出时序按同一尺寸查表 ----
    const CamTiming* t = find_timing(width, height);
    if (t == 0)
    {
        xil_printf("[cam] no sensor timing for %u x %u, supported:\r\n", width, height);
        const int n = (int)(sizeof(s_cam_timings) / sizeof(s_cam_timings[0]));
        for (int i = 0; i < n; i++)
            xil_printf("   %u x %u\r\n", s_cam_timings[i].w, s_cam_timings[i].h);
        return -2;
    }

    //---- 3.初始化 ov5640(SCCB总线就绪+寄存器序列, dev层内部完成) ----
    if (ov5640_init(t->cmos_h, t->cmos_v, t->total_h, t->total_v) != 0)
    {
        xil_printf("OV5640 detected failed!\r\n");
        return -3;
    }
    xil_printf("OV5640 detected successful!\r\n");

    //---- 4.配置 VDMA1(写+读)和显示控制器 ----
    //HDMI像素时钟74.25MHz由PL端clk_wiz_1固定输出, 这里不配时钟
    //动态GenLock(防撕裂核心): 读写通道按硬件参数互为主从(S2MM主/MM2S从),
    //主设备跳过从设备显示中的缓冲 => 读写永不同帧, HDMI无撕裂。
    //开机默认HDMI关, 第5步会摘掉写通道GenlockEn(解耦)后再停读通道,
    //保证"耦合态+读停"这个唯一有害组合不存在
    vdma_genlock(1);
    if (run_vdma_frame_buffer(&s_vdma, VDMA_BASEADDR, (int)mode->width,
                              (int)mode->height, MEM_FRAME_BUFFER_BASE,
                              0, 0, BOTH) != XST_SUCCESS)
    {
        //CfgInitialize内含通道软复位, 复位超时/配置失败必须如实上报
        //(此前返回值被忽略: 半配置的链路靠帧流校验兜底, 报错来得晚)
        xil_printf("[cam] vdma config failed\r\n");
        return -5;
    }
    vtc_dev_init(&s_disp, DISP_VTC_BASEADDR);
    vtc_dev_set_mode(&s_disp, mode);

    s_width  = (int)mode->width;
    s_height = (int)mode->height;

    //---- 5.HDMI默认关(应用按需cam_chain_hdmi_set打开) ----
    //先停读通道, 再立刻摘掉写通道GenlockEn(解耦: 写通道变纯自由循环,
    //从设备停跑不再影响它) —— 帧流校验(第6步)在完全解耦态下进行
    XAxiVdma_DmaStop(&s_vdma, XAXIVDMA_READ);
    vdma_write_genlock(&s_vdma, VDMA_BASEADDR, 0);
    s_hdmi = 0;

    //---- 6.帧流校验: 写指针必须推进(传感器+VDMA写通道真正出帧) ----
    //读通道自由跑会显示旧帧造成"链路正常"假象, 必须以写指针为准
    if (wait_frame_sync() != 0)
    {
        full_stop();                        //失败即彻底回退(常热前提被破坏)
        return -4;
    }
    s_stream = 1;
    xil_printf("[cam] chain up %ux%u\r\n",
               (unsigned)s_width, (unsigned)s_height);
    return 0;
}

// -----------------------------------------------------------------------------
// 停启模型(常热, 2026-10板上定稿): 实测(串口s2mm_sr=0x15810,
// FSZ_LESS/FSZ_MORE/INTERNAL错)传感器断流后无论热唤醒还是全量冷重配,
// 到达VDMA的帧几何都不再匹配 —— 传感器休眠唤醒后PLL不稳或PL桥断流
// 不恢复, PS侧均无解; 而上电首启(纯净状态)永远成功、原始固件常跑长稳。
// 故: 流一旦启动永不停。stop只关HDMI与预览(传感器写通道持续进帧环,
// DDR写入约79MB/s, 与原始固件常跑相同); start同尺寸立即成功。
// 注: 若链路因故障真正断流(full_stop后), 板上无法恢复, 需重启。
// -----------------------------------------------------------------------------

//帧流校验: FRAME_SYNC_TIMEOUT_MS内写指针必须推进; 失败时附S2MM状态寄存器
//供定位(错误位/停走位一眼可辨)
static int wait_frame_sync(void)
{
    u32 w0 = cam_chain_write_index();
    u32 w1 = w0;
    int ms = 0;
    for (; ms < FRAME_SYNC_TIMEOUT_MS && w1 == w0; ms++)
    {
        usleep(1000);
        w1 = cam_chain_write_index();
    }
    if (w1 == w0)
    {
        xil_printf("[cam] no frame sync %dms (s2mm_sr=0x%x)\r\n",
                   FRAME_SYNC_TIMEOUT_MS,
                   (unsigned)XAxiVdma_GetStatus(&s_vdma, XAXIVDMA_WRITE));
        return -1;
    }
    xil_printf("[cam] frame sync in %dms\r\n", ms);
    return 0;
}

//全停: VDMA读写都停+传感器睡眠+配置作废(仅冷启动前/尺寸变化/启动失败
//清理时用)。注意: 传感器断流后按板上实测无法再可靠重启(见顶部模型说明),
//full_stop之后的冷启动只在PL桥仍是上电纯净状态(=开机后第一次)才可靠
static void full_stop(void)
{
    vtc_dev_stop(&s_disp);
    XAxiVdma_DmaStop(&s_vdma, XAXIVDMA_WRITE);
    XAxiVdma_DmaStop(&s_vdma, XAXIVDMA_READ);
    ov5640_standby(1);
    s_width = 0;
    s_height = 0;
    s_hdmi = 0;
    s_stream = 0;
}

//带一次冷重试的链路启动: 失败时全停静默后完整重来一次再判定成败
int cam_chain_start_retry(u16 width, u16 height)
{
    int rc = cam_chain_start(width, height);
    if (rc == 0)
        return 0;
    xil_printf("[cam] start failed rc=%d, cold retry\r\n", rc);
    full_stop();
    usleep(100000);                          //100ms静默
    return cam_chain_start(width, height);
}

void cam_chain_hdmi_set(int on)
{
    if (s_width == 0)
        return;
    on = on ? 1 : 0;
    if (!s_stream && on)
        return;                              //热待机中不开HDMI(无新鲜帧)
    if (on == s_hdmi)
        return;
    if (on)
    {
        //开: 先起读从设备(其GenlockEn在配置里已带), 再挂回写主设备
        //GenlockEn。挂回前的一两帧从设备可能首帧取到正在写的缓冲,
        //属开机瞬态, 挂回后主/从互斥生效 => 撕裂消失
        XAxiVdma_DmaStart(&s_vdma, XAXIVDMA_READ);   //通道配置仍在, 直接续跑
        vtc_dev_start(&s_disp);
        vdma_write_genlock(&s_vdma, VDMA_BASEADDR, 1);
    }
    else
    {
        //关: 顺序不可换 —— 先摘写通道GenlockEn(解耦), 再停读通道,
        //杜绝"耦合态+读停"把写主设备拖停(历史实测的写指针停走)
        vdma_write_genlock(&s_vdma, VDMA_BASEADDR, 0);
        vtc_dev_stop(&s_disp);                       //关发生器: HDMI无信号自熄
        XAxiVdma_DmaStop(&s_vdma, XAXIVDMA_READ);
    }
    s_hdmi = on;
}

int cam_chain_hdmi_get(void)
{
    return s_hdmi;
}

//退到后台(常热模型): 只关HDMI输出与显示; 传感器与VDMA写通道继续跑
//(帧持续进帧环, 无消费者, DDR写入与原始固件常跑一致), 下次start立即
//恢复。系统开机后从未进过相机则本函数为无操作
void cam_chain_stop(void)
{
    if (s_width == 0)
        return;
    //与HDMI关闭同序: 先摘写通道GenlockEn解耦, 再停读通道
    vdma_write_genlock(&s_vdma, VDMA_BASEADDR, 0);
    vtc_dev_stop(&s_disp);                   //HDMI无信号自熄
    XAxiVdma_DmaStop(&s_vdma, XAXIVDMA_READ);
    s_hdmi = 0;
    //刻意保留: 传感器出流、VDMA写通道RUN、帧环内容、s_stream
}

int cam_chain_running(void)
{
    return s_stream;
}

int cam_chain_width(void)
{
    return s_width;
}

int cam_chain_height(void)
{
    return s_height;
}

u32 cam_chain_frame_count(void)
{
    return s_vdma.MaxNumFrames;
}

u32 cam_chain_write_index(void)
{
    return XAxiVdma_CurrFrameStore(&s_vdma, XAXIVDMA_WRITE);
}

u8* cam_chain_frame_addr(int idx)
{
    const u32 fsize = (u32)s_width * (u32)s_height * 3;
    return (u8*)(MEM_FRAME_BUFFER_BASE + (UINTPTR)fsize * (UINTPTR)idx);
}
