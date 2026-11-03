// =============================================================================
// touch —— LCD电容触摸屏顶层实现
//
// 原touch包三套总线实现(emio_iic_cfg/myiic)与ov5640的emio_sccb_cfg
// 已合并为hw/bus/{emio_gpio,gpio_i2c}: 本文件持有触摸总线实例
// g_touch_i2c(引脚+节拍), 各IC驱动经它收发, 引脚助手集中于此
// =============================================================================

#include "touch.h"

#include "xil_printf.h"

#include "hw/bus/emio_gpio.h"

//触摸总线: SCL=EMIO[2](56), SDA=EMIO[3](57), 半节拍2us(约300kbps)
const gpio_i2c_t g_touch_i2c = { 56, 57, 2 };

//包内共享的屏型信息(touch_init按传入参数填入, 各IC驱动读取)
u16 touch_lcd_id = 0;
u16 touch_lcd_w  = 0;
u16 touch_lcd_h  = 0;

//静态存储零初始化; scan指针/touchtype由touch_init按屏型填入
//(填入前touch_scan以touchtype的电容屏位拦截, 不会走到空指针)
_m_tp_dev tp_dev;

//触摸芯片复位脚(EMIO[4]=58)电平
void touch_rst(u8 x)
{
    emio_gpio_init();
    emio_pin_out(58, x);
}

//INT脚(EMIO[5]=59)方向: 1=输出 / 0=输入
void touch_int_dir(u8 x)
{
    emio_gpio_init();
    if (x)
        emio_pin_out(59, 1);
    else
        emio_pin_in(59);
}

//INT脚输出电平
void touch_int(u8 x)
{
    emio_pin_write(59, x);
}

//初始化触摸子系统: 屏型信息由调用方(dev/lcd_panel探测)传入 -> 按屏型选IC初始化
int touch_init(u16 lcd_id, u16 lcd_w, u16 lcd_h)
{
    touch_lcd_id = lcd_id;
    touch_lcd_w  = lcd_w;
    touch_lcd_h  = lcd_h;

    emio_gpio_init();
    gi2c_init(&g_touch_i2c);   //触摸两线就绪(空闲态拉高)

    switch (touch_lcd_id)
    {
    case 0x4342:   //4.3寸480x272
    case 0x4384:   //4.3寸800x480
        GT9147_Init();
        tp_dev.scan = GT9147_Scan;
        tp_dev.touchtype |= 0X80;   //电容屏
        return 0;

    case 0x7084:   //7寸800x480
    case 0x7016:   //7寸1024x600
        FT5206_Init();              //内部兼容GT911
        tp_dev.scan = FT5206_Scan;
        tp_dev.touchtype |= 0X80;   //电容屏
        return 0;

    case 0x1018:   //10.1寸1280x800
        GT9271_Init();
        tp_dev.scan = GT9271_Scan;
        tp_dev.touchtype |= 0X80;   //电容屏
        return 0;

    default:
        xil_printf("[touch] lcd id 0x%x not supported, touch disabled\r\n",
                   touch_lcd_id);
        return -1;
    }
}

//扫描触摸(单点, 即触点0), 坐标换算到LCD帧缓存坐标系
int touch_scan(u16* x, u16* y)
{
    if ((tp_dev.touchtype & 0X80) == 0)
        return -1;   //未初始化或非电容屏

    if (tp_dev.scan(0) == 0 || (tp_dev.sta & 0x01) == 0)
        return 0;    //无触摸或触点0无效

    //tp坐标 -> 帧缓存坐标: 列=tp_y, 行=高度-tp_x(竖装贴, 行方向翻转)
    u32 col = tp_dev.y[0];
    u32 row = (u32)touch_lcd_h - tp_dev.x[0];
    if (col >= touch_lcd_w) col = touch_lcd_w - 1;
    if (row >= touch_lcd_h) row = touch_lcd_h - 1;
    *x = (u16)col;
    *y = (u16)row;
    return 1;
}
