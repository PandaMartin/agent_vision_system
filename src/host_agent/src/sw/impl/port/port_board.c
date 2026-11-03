// =============================================================================
// port_board —— 平台后端的板卡实现
//
// 全系统唯一允许直接触碰Xilinx BSP与hw/链路的sw文件; 帧缓冲几何经
// hw/chain/lcd_chain查询, 触摸经hw/dev/touch, 时间取FreeRTOS调度tick
// =============================================================================

#include "port.h"

#include <stdarg.h>

#include "FreeRTOS.h"
#include "task.h"

#include "xil_cache.h"
#include "xil_printf.h"
#include "xiltimer.h"
#include "xtimer_config.h"

#include "hw/chain/lcd_chain.h"
#include "hw/dev/touch.h"
#include "hw/mem_layout.h"

//---- 显示 ----

void* port_fb_base(void)
{
    return (void*)MEM_LCD_BUFFER_BASE;
}

uint32_t port_fb_stride(void)
{
    const VideoMode* m = lcd_chain_mode();
    return m ? (uint32_t)m->width * 3 : 0;
}

uint16_t port_fb_w(void)
{
    const VideoMode* m = lcd_chain_mode();
    return m ? (uint16_t)m->width : 0;
}

uint16_t port_fb_h(void)
{
    const VideoMode* m = lcd_chain_mode();
    return m ? (uint16_t)m->height : 0;
}

void port_fb_present(void)
{
    //板上是空操作: VDMA0硬件从DDR持续扫描帧缓冲, 写入+刷Cache即上屏
}

//---- cache ----

void port_cache_flush(void* addr, uint32_t len)
{
    Xil_DCacheFlushRange((UINTPTR)addr, len);
}

void port_cache_invalidate(void* addr, uint32_t len)
{
    Xil_DCacheInvalidateRange((UINTPTR)addr, len);
}

void port_icache_invalidate(void* addr, uint32_t len)
{
    Xil_ICacheInvalidateRange((UINTPTR)addr, len);
}

void port_sync_barrier(void)
{
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

//---- 输入 ----

int port_touch_scan(uint16_t* x, uint16_t* y)
{
    return touch_scan(x, y);
}

//---- 时间 ----

uint32_t port_time_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

uint64_t port_time_us(void)
{
    XTime t;
    XTime_GetTime(&t);
    return (uint64_t)t * 1000000ULL / (uint64_t)COUNTS_PER_SECOND;
}

//---- 日志 ----

void port_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xil_vprintf(fmt, ap);   //串口输出
    va_end(ap);
}
