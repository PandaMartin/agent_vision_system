// =============================================================================
// port —— 平台后端接口(全系统唯一的硬件/平台边界, GUI最小集)
//
// impl/实现层只准经这些函数触碰平台; 板卡由port_board.c实现,
// PC模拟器由port_pc.c(SDL)实现。接口保持最小: 显示/触摸/时间/日志/cache。
// (摄像头帧环由impl/cam经hw/chain的查询接口访问, SD由impl/sd经FatFs——
//  二者均不进PC构建, 见各模块)
//
// 返回值约定: 显示未就绪时fb_*返回0, 调用方自行降级
// =============================================================================

#ifndef SW_IMPL_PORT_PORT_H
#define SW_IMPL_PORT_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//---- 显示: LCD帧缓冲(RGB888内存序BGR, 硬件自扫描) ----
void* port_fb_base(void);        //帧缓冲基址(NULL/0=LCD未起)
uint32_t port_fb_stride(void);   //行距=宽*3
uint16_t port_fb_w(void);
uint16_t port_fb_h(void);

//帧上屏钩子(板=空操作, VDMA硬件持续扫描; PC=推SDL纹理)
void port_fb_present(void);

//---- cache维护(板=Xil_DCache*, PC=空) ----
void port_cache_flush(void* addr, uint32_t len);
void port_cache_invalidate(void* addr, uint32_t len);

//新代码加载后的CPU同步(M2应用加载): I-Cache失效 + dsb/isb屏障
//(D侧用port_cache_flush写出后调本函数, 此后取指可见新代码)
void port_icache_invalidate(void* addr, uint32_t len);
void port_sync_barrier(void);

//---- 输入: 触摸轮询(板=GT9147系, PC=鼠标) ----
//返回1=按下(坐标写入), 0=无触摸, -1=不可用
int port_touch_scan(uint16_t* x, uint16_t* y);

//---- 时间: 自启动毫秒/微秒 ----
uint32_t port_time_ms(void);
uint64_t port_time_us(void);

//---- 日志 ----
void port_log(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* SW_IMPL_PORT_PORT_H */
