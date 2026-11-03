// =============================================================================
// 18_cam_lcd_hdmi_servo_net —— 应用系统固件(17整体迁移 + 网口文件传输)
//
// 系统 = 核心链路(串口打印+LCD+触摸+SD) + 桌面launcher
//   + 内置应用(sw/app/int_app: 相机/云台/推理/文件/文件传输/测试, 各一文件)
//   + SD应用加载(sw/app/ext_app: /apps扫描与搬运, 应用=SD加载的裸机码模块)
// ; 摄像头+HDMI链为可选子系统, 由相机应用按需启停, 未插摄像头不影响开机。
//
// 文件传输(替代17的串口FWPUT): 网口lwIP220(SOCKET_API, 18平台已验证配方),
// 静态IP 192.168.1.10/24, 标准TFTP(UDP 69)只在"文件传输"应用内监听
// (sw/impl/net), PC端 tools/netpush/net_push.py 或任意tftp客户端推送,
// 落SD后/apps与/models联动重扫。串口只保留打印(接收已随FWPUT退役删除)。
//
// 目录组织(hw/sw分离):
//   hw/  硬件驱动包: bus(EMIO GPIO+位绑I2C) -> ip(VDMA/VTC/clk_wiz/AXI GPIO)
//        -> dev(面板/传感器/触摸/云台舵机) -> chain(LCD链/摄像头+HDMI链编排)
//   sw/impl/ 实现层: port(平台边界) sys(堆/计时) sd(文件/照片) lcd(绘制/
//        LVGL移植/视频窗口) cam(帧访问/叠加) nn(ncnn推理+SD模型管理)
//        net(lwIP网口文件接收)
//   sw/app/  应用系统: launcher(桌面) appctl(命令总线) bridge(数据桥)
//        int_app/(内置应用) ext_app/(SD应用加载框架)
//
// FreeRTOS任务划分:
//   app_task(业务任务): 开机快照+命令分发到各应用后端(FatFs唯一使用者)
//   gui任务: 唯一的LVGL驱动者(桌面渲染+触摸)
//   vid任务(相机应用期间): 摄像头预览窗降采样直写帧缓冲
//   nstack/xemac_in任务: lwIP协议栈+收包(net_stack_boot常驻, 监听受应用门控)
// =============================================================================

extern "C" {
#include "xil_types.h"
#include "xil_printf.h"
#include "FreeRTOS.h"
#include "task.h"
//厂商驱动的C头文件无extern "C"保护, 必须包在本块内(否则链接C++修饰名)
#include "hw/dev/touch.h"
#include "hw/dev/lcd_panel.h"
#include "hw/dev/servo.h"
#include "hw/chain/lcd_chain.h"
#include "hw/chain/cam_chain.h"
#include "hw/mem_layout.h"
}

#include "sw/impl/sd/sd.h"
#include "sw/impl/sd/photo.h"
#include "sw/impl/sys/platform.h"
#include "sw/app/appctl.h"
#include "sw/app/fw_version_gen.h"
#include "sw/app/int_app/app_camera.h"
#include "sw/app/int_app/app_files.h"
#include "sw/app/int_app/app_transfer.h"
#include "sw/app/int_app/app_infer.h"
#include "sw/impl/nn/model_mgr.h"
#include "sw/app/ext_app/sdapp.h"
#include "sw/impl/lcd/lcd.h"
#include "sw/impl/lcd/lvgl_port.h"
#include "sw/impl/lcd/vidwin.h"
#include "sw/impl/net/net_server.h"
#include "sw/app/bridge.h"

// -----------------------------------------------------------------------------
// 应用配置

//任务参数(栈深单位是字word=4字节; app_task现承载ncnn推理全路径, 加大余量)
#define APP_TASK_STACK  6144    //24KB(ncnn首次推理曾致全系统挂死, 栈溢出嫌疑加量排查)
#define APP_TASK_PRIO   1

//装配期探测结果(app_init填充, app_task随boot快照发布给GUI)
static u16 s_scr_id = 0, s_scr_w = 0, s_scr_h = 0;
static u16 s_touch_id = 0;
static int s_sd_ok = 0;

// -----------------------------------------------------------------------------
// 应用初始化: 按依赖顺序拉起核心子系统(在main里、调度器启动前执行,
// 单线程环境)——系统核心 = 串口打印+LCD链+触摸+SD; 摄像头+HDMI为可选
// 子系统, 归应用控制。核心部件失败只降级(打印+继续), 仅数据桥初始化
// 失败才致命
static int app_init(void)
{
    //0.GUI数据桥(先于gui_init: 页面刷新依赖快照可用)
    if (gui_model_init() != 0)
        return -5;

    //1.LCD显示链(核心): 先画彩条点亮, LVGL成功后桌面接管;
    //    视频窗口只存LCD尺寸(开窗在相机链路启动时)
    if (lcd_chain_start() != 0 || lcd_colorbar() != 0)
    {
        xil_printf("[lcd] init failed, system runs headless\r\n");
    }
    else
    {
        u32 vstride; u16 vw, vh;
        if (lcd_frame(&vstride, &vw, &vh) != 0)
        {
            if (vidwin_init(vw, vh) != 0)
                xil_printf("[vid] layout unavailable on this panel\r\n");
        }
    }

    //2.LCD触摸(核心): 按识别到的屏型选触摸IC(4.3寸GT9147/7寸FT5206/10.1寸
    //    GT9271, 触摸4线在EMIO[2:5]); 供LVGL indev, 失败只是无触摸输入
    s_scr_id = lcd_panel_detect(&s_scr_w, &s_scr_h);
    s_touch_id = (touch_init(s_scr_id, s_scr_w, s_scr_h) == 0) ? s_scr_id : 0;
    xil_printf("[boot] panel %ux%u id=0x%x touch=0x%x\r\n",
               s_scr_w, s_scr_h, s_scr_id, s_touch_id);

    //3.LVGL桌面(状态栏+图标网格+应用生命周期): 失败保持彩条
    if (gui_init() != 0)
        xil_printf("[gui] init failed, keep colorbar\r\n");

    //4.SD卡(核心, 可降级): 桌面状态栏图标用, 失败只是图标置灰
    s_sd_ok = (sd_mount() == 0);

    return 0;
}

// -----------------------------------------------------------------------------
// 业务任务: 开机快照 + 命令分发(各应用后端都在app_task上下文执行,
// FatFs纪律由这一层统一保证)
static void app_task(void* arg)
{
    (void)arg;

    portTASK_USES_FLOATING_POINT();

    vTaskDelay(pdMS_TO_TICKS(100));   //等gui任务把桌面画起来
    gui_model_pub_boot(s_sd_ok, 0,
                       s_scr_id, s_scr_w, s_scr_h, s_touch_id);
    if (s_sd_ok)
    {
        ext_sdapp_scan();             //SD应用列表随开机发布
        nn_models_scan();             //SD模型包扫描(推理应用模型面板)
        app_infer_pub_models();       //清单发布到数据桥
        //注: nn_selftest 只由模型推送触发(见app_transfer.c scan_run)——
        //开机不自跑, 避免问题mod把系统挂在启动路径上、传输应用无法进入

        //主题壁纸(可选): 0:/theme/wallpaper.bmp, 24位BMP, 尺寸=屏。
        //没有/不符则保持默认渐变背景; 加载成功所有权过户给gui常驻
        u8* wall = photo_load_wallpaper("0:/theme/wallpaper.bmp",
                                        (u32)s_scr_w, (u32)s_scr_h);
        if (wall != 0)
            gui_model_pub_wallpaper(wall);
    }
    xil_printf("[boot] desktop shell ready\r\n");

    AppCmd cmd;
    TickType_t tick_1s = 0;
    while (1)
    {
        //1s节拍: 传输联动重扫的去抖补扫(网络看门狗在net_server自带的
        //独立netwdt任务里, 不随本任务忙而停摆)
        const TickType_t now = xTaskGetTickCount();
        if (now - tick_1s >= pdMS_TO_TICKS(1000))
        {
            tick_1s = now;
            app_transfer_tick();
        }

        //阻塞等命令(1tick节拍); 无命令且推理运行中时回泵一帧(~200-400ms,
        //非阻塞: 停止/切模型等命令在帧间得到响应)
        if (!appctl_take(&cmd, 1))
        {
            if (app_infer_running())
                app_infer_pump();
            continue;
        }

        switch (cmd.type)
        {
        //---- 文件应用(+相机相册复用FS管道) ----
        case APP_CMD_FS_LIST:
        case APP_CMD_FS_DELETE:
        case APP_CMD_FS_COPY:
            app_files_dispatch(&cmd);
            break;
        //---- SD应用(ext_app) ----
        case APP_CMD_APP_RESCAN:
        case APP_CMD_APP_LAUNCH:
            ext_sdapp_dispatch(&cmd);
            break;
        //---- 相机应用 ----
        case APP_CMD_CAM_OPEN:
        case APP_CMD_CAM_CLOSE:
        case APP_CMD_HDMI_SET:
        case APP_CMD_CAPTURE:
        case APP_CMD_PHOTO_LOAD:
        case APP_CMD_PHOTO_UNLOAD:
            app_camera_dispatch(&cmd);
            break;
        //---- 推理应用(HDMI_SET相机的开关复用, 见上) ----
        case APP_CMD_RUN_SET:
        case APP_CMD_RUN_TOGGLE:
        case APP_CMD_MODEL_SELECT:
        case APP_CMD_MODELS_RESCAN:
        case APP_CMD_PHOTO_INFER:
            app_infer_dispatch(&cmd);
            break;
        //---- 文件传输应用(网口) ----
        case APP_CMD_NET_START:
        case APP_CMD_NET_STOP:
        case APP_CMD_NET_FILE:
            app_transfer_dispatch(&cmd);
            break;
        default:
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// main: 上电初始化(单线程环境) -> 命令总线 -> 启动业务任务+网络栈 -> 调度器
//(gui任务在gui_init内创建; 串口仅调试打印, 无接收)
int main()
{
    platform_heap_lock_init();        //malloc多任务锁(先于一切任务)

    //云台舵机(TTC硬件PWM): 波形由硬件自保持, 归中后零CPU开销;
    //失败只降级不阻塞系统启动(未接云台/舵机时系统照常跑)
    if (servo_init() == 0)
    {
        servo_center();
        servo_print_info();
    }
    else
    {
        xil_printf("[servo] init failed, gimbal offline\r\n");
    }

    if (appctl_init() != 0)
    {
        xil_printf("appctl init failed, halt.\r\n");
        while (1) {}
    }

    if (app_init() != 0)
    {
        xil_printf("app init failed, halt.\r\n");
        while (1) {}
    }

    if (xTaskCreate(app_task, "app", APP_TASK_STACK, 0, APP_TASK_PRIO, 0)
        != pdPASS)
    {
        xil_printf("task create failed, halt.\r\n");
        while (1) {}
    }

    //网口栈(lwIP+GEM0, 静态IP; TFTP/UDP 69监听由文件传输应用门控)
    if (net_stack_boot() != 0)
        xil_printf("[net] stack boot failed, transfer app unavailable\r\n");

    vTaskStartScheduler();   //正常情况下不返回

    xil_printf("scheduler returned, halt.\r\n");
    while (1) {}
    return 0;
}
