// =============================================================================
// lvgl_port —— LVGL移植层实现(自gui.c改名, 平台调用改经port)
//
// 三件事:
//   tick:   lv_tick_set_cb -> port_time_ms(FreeRTOS调度tick折算)
//   显示:   LVGL渲染进部分重绘缓冲(RGB888, 与LCD帧缓存同格式同字节序),
//           flush_cb逐行memcpy到帧缓存, 再按脏行区间刷D-Cache(经port)
//           (不能整帧刷/全局关Cache: LCD帧缓存最大2.9MB, 且ncnn依赖D-Cache)
//   输入:   触摸indev轮询port_touch_scan(已含面板->屏坐标变换)
// =============================================================================

#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"

#include "sw/impl/port/port.h"
#include "lcd.h"
#include "lvgl_port.h"
#include "sw/app/launcher.h"

//任务参数(栈深单位是字word=4字节)
//注意: 任务栈出自FreeRTOS堆(configTOTAL_HEAP_SIZE=64KB), 当前app32K+gui16K
//+vid8K已接近顶满; M2加载SD应用需更大gui栈时改静态栈(xTaskCreateStatic)
#define GUI_TASK_STACK  4096    //16KB
#define GUI_TASK_PRIO   1       //与app任务同级, 时间片轮转

//部分重绘缓冲: 每块约150KB(行数随屏宽自适应, 800宽=60行, 1280宽=40行),
//双缓冲让LVGL渲染下一块时上一块还在flush, malloc自DDR的sbrk堆
#define GUI_BUF_TARGET_BYTES  (150 * 1024)
#define GUI_BUF_LINES_MAX     60
#define GUI_BUF_LINES_MIN     16

//LCD帧缓存信息(port提供, init时缓存到本模块)
static uint8_t* s_frame  = 0;
static uint32_t s_stride = 0;
static uint16_t s_width  = 0;
static uint16_t s_height = 0;

//触摸状态镜像(indev回调写, 界面状态栏读; 单字访问天然原子, 无需加锁)
static volatile int s_touched = 0;
static volatile uint16_t s_tx = 0, s_ty = 0;

//帧率统计: flush一次=一帧上屏, gui任务每秒结算
static volatile uint32_t s_flush_cnt = 0;
static volatile uint32_t s_fps = 0;

// -----------------------------------------------------------------------------
// tick: LVGL内部计时统一取自调度tick(100Hz), 折算成毫秒
static uint32_t gui_tick_cb(void)
{
    return port_time_ms();
}

// -----------------------------------------------------------------------------
// flush: 把LVGL渲染好的一块(RGB888, 内存序BGR, 与帧缓存一致)拷到LCD帧缓存
// area为脏区, px_map按行紧凑排列(STRIDE_ALIGN=1, 行距=区宽*3)
static void gui_flush_cb(lv_display_t* disp, const lv_area_t* area,
                         uint8_t* px_map)
{
    //防御性裁剪: 脏区必须完全落在屏内(阴影外扩等极端情况下LVGL给过界
    //坐标时, 负数经无符号换算会变成野地址, 刷Cache直接总线错误)
    int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t x2 = area->x2 >= (int32_t)s_width  ? (int32_t)s_width  - 1 : area->x2;
    int32_t y2 = area->y2 >= (int32_t)s_height ? (int32_t)s_height - 1 : area->y2;
    if (x1 > x2 || y1 > y2)     //整块在屏外(理论上不会): 直接完成
    {
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t w = x2 - x1 + 1;
    const int32_t h = y2 - y1 + 1;

    uint8_t* dst = s_frame + (uint32_t)y1 * s_stride + (uint32_t)x1 * 3;
    const uint8_t* src = px_map + (uint32_t)(y1 - area->y1) * ((uint32_t)(area->x2 - area->x1 + 1)) * 3
                         + (uint32_t)(x1 - area->x1) * 3;
    for (int32_t y = 0; y < h; y++)
    {
        memcpy(dst, src, (size_t)w * 3);
        dst += s_stride;
        src += (uint32_t)(area->x2 - area->x1 + 1) * 3;
    }

    //脏区列段逐行刷出D-Cache(VDMA0从DDR直读, 不经过Cache)。
    //必须只刷脏区宽度: 刷整行屏宽会把同行视频洞里"写缓存中"的半成品帧
    //提前clean+invalidate上屏(撕裂/闪烁, 2026-10-24板上定位: 推理9.5Hz
    //GUI更新与洞写窗口高频相撞)
    {
        uint8_t* row = s_frame + (uint32_t)y1 * s_stride + (uint32_t)x1 * 3;
        for (int32_t y = 0; y < h; y++)
        {
            port_cache_flush(row, (uint32_t)w * 3);
            row += s_stride;
        }
    }
    s_flush_cnt++;

    lv_display_flush_ready(disp);
}

// -----------------------------------------------------------------------------
// 触摸indev: port_touch_scan已把面板坐标换成屏坐标(列/行), 直接喂给LVGL
static void gui_touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    LV_UNUSED(indev);
    uint16_t x, y;
    if (port_touch_scan(&x, &y) == 1)
    {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        s_touched = 1;
        s_tx = x;
        s_ty = y;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
        s_touched = 0;
    }
}

// -----------------------------------------------------------------------------
// gui任务: 唯一的LVGL驱动者; lv_timer_handler处理到期定时器(刷新/动画/输入),
// 返回距下次到期的时间, 配合vTaskDelay让出CPU(100Hz tick, 最小睡1个tick=10ms)
static void gui_task(void* arg)
{
    (void)arg;
    uint32_t last = xTaskGetTickCount();
    uint32_t cnt_last = 0;
    uint32_t sec = 0;

    //LVGL的NEON渲染路径会执行FPU/NEON指令: configUSE_TASK_FPU_SUPPORT=1时
    //任务必须先登记FPU上下文, 否则切换出去后d0-d31被别的任务冲掉(渲染花屏)
    portTASK_USES_FLOATING_POINT();

    while (1)
    {
        lv_timer_handler();
        vTaskDelay(1);

        //每秒结算一次帧率(flush计数差值); 每5秒报一次心跳,
        //板上诊断用: 区分"gui任务死了"和"活着但没上屏"
        if (xTaskGetTickCount() - last >= configTICK_RATE_HZ)
        {
            last += configTICK_RATE_HZ;
            s_fps = s_flush_cnt - cnt_last;
            cnt_last = s_flush_cnt;
            if (++sec % 5 == 0)
                port_log("[gui] alive fps=%u\r\n", (unsigned)s_fps);
        }
    }
}

// -----------------------------------------------------------------------------
int gui_init(void)
{
    //---- 1.LCD帧缓存信息(必须先拉起lcd链路) ----
    s_frame = lcd_frame(&s_stride, &s_width, &s_height);
    if (s_frame == 0)
    {
        port_log("[gui] lcd not ready\r\n");
        return -1;
    }

    //---- 2.LVGL核心 + tick源 ----
    lv_init();
    lv_tick_set_cb(gui_tick_cb);

    //---- 3.显示设备: RGB888双缓冲部分重绘 ----
    lv_display_t* disp = lv_display_create(s_width, s_height);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);
    lv_display_set_flush_cb(disp, gui_flush_cb);

    uint32_t lines = GUI_BUF_TARGET_BYTES / s_stride;
    if (lines > GUI_BUF_LINES_MAX) lines = GUI_BUF_LINES_MAX;
    if (lines < GUI_BUF_LINES_MIN) lines = GUI_BUF_LINES_MIN;
    uint32_t buf_size = lines * s_stride;

    uint8_t* buf1 = (uint8_t*)malloc(buf_size);
    uint8_t* buf2 = (uint8_t*)malloc(buf_size);
    if (buf1 == 0 || buf2 == 0)
    {
        port_log("[gui] draw buffer alloc failed (%u x %u lines)\r\n",
                 (unsigned)lines, buf_size);
        free(buf1);
        free(buf2);
        return -2;
    }
    lv_display_set_buffers(disp, buf1, buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    //---- 4.触摸输入(不可用只是没交互, 不影响渲染) ----
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, gui_touch_read_cb);
    lv_indev_set_display(indev, disp);

    //---- 5.深色主题 + 桌面启动器(状态栏/图标网格/应用生命周期/toast) ----
    lv_theme_t* th = lv_theme_default_init(
        disp, lv_palette_main(LV_PALETTE_CYAN), lv_palette_main(LV_PALETTE_PINK),
        true, &font_cn_16);
    lv_display_set_theme(disp, th);
    launcher_init();

    //---- 6.gui任务 ----
    if (xTaskCreate(gui_task, "gui", GUI_TASK_STACK, 0, GUI_TASK_PRIO, 0)
        != pdPASS)
    {
        port_log("[gui] task create failed\r\n");
        return -3;
    }

    port_log("[gui] lvgl %ux%u RGB888, %u lines x2 buffers\r\n",
             s_width, s_height, (unsigned)lines);
    return 0;
}

uint32_t gui_fps(void)
{
    return s_fps;
}

void gui_touch_info(int* pressed, uint16_t* x, uint16_t* y)
{
    if (pressed) *pressed = s_touched;
    if (x) *x = s_tx;
    if (y) *y = s_ty;
}
