// =============================================================================
// lcd_panel —— LCD屏型识别与尺寸决策(板级)
//
// 把"识别当前插的LCD是哪个"这一板级策略从显示/摄像头链路模块中剥离:
// chain层先在这里(或用常量)确定显示尺寸, 再配置链路; 触摸dev也由
// 调用方把探测结果传入(解反向依赖)
//
// LCD ID来自排线电阻拉位(PG6/PI2/PI7), 经PL端AXI GPIO读3位编码,
// 与I2C无关; ID编码见lcd_panel.c实现
// =============================================================================

#ifndef HW_DEV_LCD_PANEL_H
#define HW_DEV_LCD_PANEL_H

#include "xil_types.h"

//读LCD ID(AXI GPIO)并给出建议显示尺寸, 识别不了的ID回退800x480
//返回原始ID值(调试用, touch_init也用它选触摸IC)
u16 lcd_panel_detect(u16* width, u16* height);

#endif /* HW_DEV_LCD_PANEL_H */
