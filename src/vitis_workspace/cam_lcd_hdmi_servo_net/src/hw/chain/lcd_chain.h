// =============================================================================
// lcd_chain —— LCD显示链路子系统(链路编排层)
//
// 一条链拉起整个LCD显示: 屏型识别(dev/lcd_panel) -> 查VideoMode(ip/lcd_modes)
// -> VDMA0只读单帧缓冲(ip/vdma) -> clk_wiz_0配像素时钟(ip/clk_wiz)
// -> VTC0启动(ip/vtc_api); 之后硬件自由跑, CPU只往帧缓存写数据+刷Cache
//
// 属系统核心外设(开机启动, 常驻); 摄像头/HDMI链见cam_chain(可选子系统)
// =============================================================================

#ifndef HW_CHAIN_LCD_CHAIN_H
#define HW_CHAIN_LCD_CHAIN_H

#include "xil_types.h"

#include "hw/ip/lcd_modes.h"

//拉起LCD链路(屏识别+VDMA0+像素时钟+VTC0)
//返回0成功; -1屏型无VideoMode; -2 VDMA0配置失败; -3显示启动失败
int lcd_chain_start(void);

//停止LCD链路(关VTC发生器; 帧缓冲内容保留)
void lcd_chain_stop(void);

//当前显示模式(NULL=未启动); 绘制层据此取宽高/时序
const VideoMode* lcd_chain_mode(void);

#endif /* HW_CHAIN_LCD_CHAIN_H */
