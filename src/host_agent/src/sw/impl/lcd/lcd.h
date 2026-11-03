// =============================================================================
// lcd —— LCD帧缓存绘制原语(实现层, 可移植)
//
// 链路拉起归hw/chain/lcd_chain; 本模块只做"往帧缓冲写像素"的纯逻辑,
// 帧缓冲地址/几何/cache维护全部经port, PC构建同样可用
//
// 坐标系: x=列0..w-1, y=行0..h-1, RGB888
// =============================================================================

#ifndef SW_IMPL_LCD_LCD_H
#define SW_IMPL_LCD_LCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//画彩虹彩条(画一次即可, VDMA硬件持续刷新) 返回0成功/-1未初始化
int lcd_colorbar(void);

//整屏清色 返回0成功/-1未初始化
int lcd_clear(uint8_t r, uint8_t g, uint8_t b);

//帧缓冲信息(未起返回NULL), 供LVGL移植层/vidwin直写帧缓冲
uint8_t* lcd_frame(uint32_t* stride, uint16_t* width, uint16_t* height);

//粗线/触摸轨迹(实心圆笔触Bresenham直线, 只flush脏行区间)
//返回0成功/-1未初始化
int lcd_draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  uint8_t size, uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif /* SW_IMPL_LCD_LCD_H */
