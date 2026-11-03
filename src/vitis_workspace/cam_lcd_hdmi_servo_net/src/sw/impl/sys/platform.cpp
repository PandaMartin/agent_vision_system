// =============================================================================
// sys_platform —— 平台服务实现(FreeRTOS版)
//
// _sbrk: malloc 落到 lscript.ld 定义的大堆(移植自 fpga_ncnn p6_real, 带边界检查),
//        ncnn(SIMPLESTL 的 operator new)和 FatFs 的分配最终都走这里
// 计时:  xiltimer 的 XTime_GetTime(读64位全局定时器, 单调递增, 与FreeRTOS的
//        SCU tick定时器互不影响), COUNTS_PER_SECOND 在 xtimer_config.h
//        注意 xil_printf 在 A9 上开了 SUPPORT_64BIT_PRINT, %l 会被当成 64 位
//        读参数, 所以时间打印只用 %u 配 unsigned int
// 延时:  调度器启动后(任务上下文)用 vTaskDelay 让出CPU;
//        调度器未启动(上电初始化阶段)退化为全局定时器忙等
// =============================================================================

#include "sw/impl/sys/platform.h"

#include <stddef.h>
#include <errno.h>

extern "C" {
#include "xil_printf.h"
#include "xtimer_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
}

//堆边界由 lscript.ld 导出
extern char HeapBase;
extern char HeapLimit;

// -----------------------------------------------------------------------------
// newlib malloc多任务锁: 网络任务(net_server收文件)与app_task(sd_read_file/
// 拍照/照片查看)现在并发malloc, newlib默认的__malloc_lock是空操作——不加锁
// 堆链表会被并发改坏。锁用FreeRTOS递归互斥量(pvPortMalloc分配, 自身不经
// newlib堆, 无递归); 调度器未启动阶段单线程, 直接跳过加锁。
// platform_heap_lock_init在main最前(建任何任务之前)调用一次。
static SemaphoreHandle_t s_malloc_mtx;

extern "C" void platform_heap_lock_init(void)
{
    if (s_malloc_mtx == 0)
        s_malloc_mtx = xSemaphoreCreateRecursiveMutex();
}

extern "C" void __malloc_lock(struct _reent* r)
{
    (void)r;
    if (s_malloc_mtx == 0)
        return;                             //未初始化=调度器前, 单线程无竞态
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
        xSemaphoreTakeRecursive(s_malloc_mtx, portMAX_DELAY);
}

extern "C" void __malloc_unlock(struct _reent* r)
{
    (void)r;
    if (s_malloc_mtx != 0
        && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
        xSemaphoreGiveRecursive(s_malloc_mtx);
}

extern "C" void* _sbrk(ptrdiff_t incr)
{
    static char* cur = 0;
    if (!cur)
        cur = &HeapBase;

    char* next = cur + incr;
    if (next < &HeapBase || next > (char*)&HeapLimit)
    {
        xil_printf("[FAIL] _sbrk out of heap: incr=%d\r\n", (int)incr);
        errno = ENOMEM;
        return (void*)-1;
    }
    char* prev = cur;
    cur = next;
    return prev;
}

unsigned int time_diff_ms(XTime t0, XTime t1)
{
    return (unsigned int)((((unsigned long long)(t1 - t0)) * 1000ULL
                          / (unsigned long long)COUNTS_PER_SECOND) & 0xFFFFFFFFULL);
}

void delay_ms(unsigned int ms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        //任务上下文: 让出CPU(tick=100Hz, 粒度10ms, 向上取整保证至少延时ms)
        vTaskDelay((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
        return;
    }

    //初始化阶段(调度器未启动): 全局定时器忙等
    XTime t0;
    XTime_GetTime(&t0);
    while (1)
    {
        XTime t;
        XTime_GetTime(&t);
        if (time_diff_ms(t0, t) >= ms)
            break;
    }
}
