//****************************************Copyright (c)***********************************//
//原子哥在线教学平台：www.yuanzige.com
//技术支持：www.openedv.com
//淘宝店铺：http://openedv.taobao.com
//关注微信公众平台微信号："正点原子"，免费获取ZYNQ & FPGA & STM32资料。
//版权所有，盗版必究。
//Copyright(C) 正点原子 2018-2028
//All rights reserved.
//----------------------------------------------------------------------------------------
// File name:           ov5640.h (原ov5640_init.h, 迁至dev层并修正包含保护位置)
// Descriptions:        ov5640配置(SCCB访问改走hw/bus/gpio_i2c)
//----------------------------------------------------------------------------------------
//****************************************************************************************//

#ifndef HW_DEV_OV5640_H
#define HW_DEV_OV5640_H

#include "xil_types.h"

//初始化OV5640(内部先就绪SCCB总线再读ID校验, ID不符返回1)
//cmos_h/v_pixel: 输出有效像素; total_h/v_pixel: 包含消隐的总像素
u8 ov5640_init(u16 cmos_h_pixel, u16 cmos_v_pixel,
               u16 total_h_pixel, u16 total_v_pixel);

//传感器待机/唤醒(链路停止后调用sleep=1让DVP停流; sleep=0恢复出流)
//不软复位: 只写0x3008的休眠位, 下次ov5640_init自己会做软复位
void ov5640_standby(int sleep);

#endif /* HW_DEV_OV5640_H */
