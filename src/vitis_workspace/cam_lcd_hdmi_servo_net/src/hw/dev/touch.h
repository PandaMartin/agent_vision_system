// =============================================================================
// touch —— LCD电容触摸屏顶层(按屏型分发触摸IC驱动)
//
// 分发关系:
//   4.3寸(0x4342/0x4384) -> GT9147
//   7寸(0x7084/0x7016)   -> FT5206(内部兼容GT911)
//   10.1寸(0x1018)       -> GT9271
// tp_dev沿用原框架(各IC驱动写入坐标/状态, sta低10位为触点有效位图);
// 应用层一般只用touch_init/touch_scan, 需要多点时可直接读tp_dev
//
// 屏型信息由调用方(chain层)探测后传入, 本包不再反向依赖lcd_panel
// (原touch.c include lcd_detect.h的跨包耦合已解)
//
// 坐标系约定: 触摸屏相对横屏是竖装贴的, 各IC驱动输出的
//   tp_dev.x = 行方向(自底向上), tp_dev.y = 列方向;
// touch_scan已换算到LCD帧缓存坐标系(x=列0..w-1, y=行0..h-1)
// =============================================================================

#ifndef HW_DEV_TOUCH_H
#define HW_DEV_TOUCH_H

#include "xil_types.h"

#include "hw/bus/gpio_i2c.h"
#include "gt9147.h"
#include "gt9271.h"
#include "ft5206.h"

#define TP_PRES_DOWN 0x8000  //触屏被按下
#define TP_CATH_PRES 0x4000  //有按键按下了
#define CT_MAX_TOUCH 10      //电容屏支持的点数,固定为10点

//触摸屏控制器(原框架, 各IC驱动写入)
typedef struct
{
    u8 (*init)(void);           //初始化触摸屏控制器
    u8 (*scan)(u8 mode);        //扫描触摸屏.0,屏幕扫描;1,物理坐标
    u16 x[CT_MAX_TOUCH];        //当前坐标(行方向, 自底向上)
    u16 y[CT_MAX_TOUCH];        //当前坐标(列方向)
    //电容屏最多有10组坐标: x[0],y[0]代表此次扫描触点0的坐标,
    //x[9],y[9]存储(GT9147为x[4])第一次按下时的坐标
    u16 sta;                    //笔的状态
                                //b15:按下1/松开0
                                //b14:0,没有按键按下;1,有按键按下
                                //b9~b0:电容触摸屏按下的点数位图
    u8 touchtype;               //b7:0,电阻屏;1,电容屏
                                //b0:0,竖屏;1,横屏
}_m_tp_dev;

extern _m_tp_dev tp_dev;   //触屏控制器在touch.c里面定义

//包内共享的LCD屏型信息(由touch_init填入, IC驱动用于坐标换算/合法性判断)
extern u16 touch_lcd_id;   //LCD ID(与dev/lcd_panel的探测同一编码)
extern u16 touch_lcd_w;    //LCD宽(列数)
extern u16 touch_lcd_h;    //LCD高(行数)

//触摸I2C总线(SCL=56/SDA=57即EMIO[2:3], 节拍2us)与引脚助手
//引脚定义集中于此, 各IC驱动经这些接口操作复位/中断脚
extern const gpio_i2c_t g_touch_i2c;
void touch_rst(u8 x);          //触摸芯片复位脚电平(EMIO[4])
void touch_int_dir(u8 x);      //INT脚方向(1=输出/0=输入, EMIO[5])
void touch_int(u8 x);          //INT脚输出电平

//初始化触摸子系统: 屏型信息由调用方探测后传入 -> 按屏型选IC初始化
//返回0成功; -1屏型不在支持列表(触摸不可用)
int touch_init(u16 lcd_id, u16 lcd_w, u16 lcd_h);

//扫描触摸(单点, 即触点0)
//返回1=有触摸, 坐标写入*x/*y(LCD帧缓存坐标系: x=列, y=行);
//返回0=无触摸; -1=触摸未初始化
int touch_scan(u16* x, u16* y);

#endif /* HW_DEV_TOUCH_H */
