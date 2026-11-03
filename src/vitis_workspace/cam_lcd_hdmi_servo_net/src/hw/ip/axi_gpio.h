// =============================================================================
// axi_gpio —— PL端AXI GPIO IP薄包装
//
// 供dev层读取板上的GPIO编码(当前唯一用户: lcd_panel的屏ID拨码读取)。
// 原lcd_detect.c直接操作XGpio属越层(dev碰ip), 收敛于此
// =============================================================================

#ifndef HW_IP_AXI_GPIO_H
#define HW_IP_AXI_GPIO_H

#include "xil_types.h"

//初始化指定基地址的AXI GPIO(内部静态实例, 幂等)
//返回0成功; -1失败
int axi_gpio_open(u32 baseaddr);

//读通道离散输入
u32 axi_gpio_read(u32 channel);

//设置通道方向掩码(1=输入 / 0=输出, 与XGpio语义一致)
void axi_gpio_set_dir(u32 channel, u32 dirmask);

#endif /* HW_IP_AXI_GPIO_H */
