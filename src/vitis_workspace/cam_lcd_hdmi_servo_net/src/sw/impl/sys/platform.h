// =============================================================================
// sys_platform —— 裸机平台服务(堆扩展/计时)
// =============================================================================
#pragma once

#include "xiltimer.h"

#ifdef __cplusplus
extern "C" {
#endif

//两个 XTime 时刻差的毫秒数(返回值 unsigned int, 可直接配 xil_printf 的 %u)
unsigned int time_diff_ms(XTime t0, XTime t1);

//毫秒级忙等延时
void delay_ms(unsigned int ms);

//newlib malloc多任务锁初始化(main最前、建任何任务之前调用一次;
//网络任务与app_task并发malloc所需, 见platform.cpp)
void platform_heap_lock_init(void);

#ifdef __cplusplus
}
#endif
