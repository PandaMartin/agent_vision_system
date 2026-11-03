// =============================================================================
// gt9271 —— 10.1寸电容触摸屏GT9271驱动(1280x800屏)
//
// 移植自正点原子36_touch_draw_lcd的TOUCH/gt9271(原头文件中Altera平台
// 的PIO宏未使用, 不再保留)
// =============================================================================

#ifndef TOUCH_GT9271_H
#define TOUCH_GT9271_H

#include "xil_types.h"

//I2C读写命令
#define GT9271_CMD_WR       0X28        //写命令
#define GT9271_CMD_RD       0X29        //读命令

//GT9271 部分寄存器定义
#define GT9271_CTRL_REG     0X8040      //GT9271控制寄存器
#define GT9271_CFGS_REG     0X8047      //GT9271配置起始地址寄存器
#define GT9271_CHECK_REG    0X80FF      //GT9271校验和寄存器
#define GT9271_PID_REG      0X8140      //GT9271产品ID寄存器

#define GT9271_GSTID_REG    0X814E      //GT9271当前检测到的触摸情况
#define GT9271_TP1_REG      0X8150      //第一个触摸点数据地址
#define GT9271_TP2_REG      0X8158      //第二个触摸点数据地址
#define GT9271_TP3_REG      0X8160      //第三个触摸点数据地址
#define GT9271_TP4_REG      0X8168      //第四个触摸点数据地址
#define GT9271_TP5_REG      0X8170      //第五个触摸点数据地址
#define GT9271_TP6_REG      0X8178      //第六个触摸点数据地址
#define GT9271_TP7_REG      0X8180      //第七个触摸点数据地址
#define GT9271_TP8_REG      0X8188      //第八个触摸点数据地址
#define GT9271_TP9_REG      0X8190      //第九个触摸点数据地址
#define GT9271_TP10_REG     0X8198      //第十个触摸点数据地址

u8 GT9271_Send_Cfg(u8 mode);                  //发送GT9271配置参数
//WR/RD寄存器访问复用gt9147.h的GT9147_WR_Reg/GT9147_RD_Reg(协议相同)
u8 GT9271_Init(void);                         //初始化GT9271触摸屏
u8 GT9271_Scan(u8 mode);                      //扫描触摸屏(采用查询方式)

#endif /* TOUCH_GT9271_H */
