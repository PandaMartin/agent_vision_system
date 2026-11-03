// =============================================================================
// lcd_panel —— LCD屏型识别与尺寸决策实现
//
// 合并原hw/lcd/lcd_detect.c与lcd.c的lcd_id_read(拨码→ID映射);
// AXI GPIO访问改经hw/ip/axi_gpio包装(dev层不直接操作IP)
// =============================================================================

#include "lcd_panel.h"

#include "xil_printf.h"
#include "xparameters.h"

#include "hw/ip/axi_gpio.h"

//PL端 AXI GPIO(lcd_id)基地址与通道
#define DISP_ID_GPIO_BASEADDR  XPAR_AXI_GPIO_0_BASEADDR
#define DISP_ID_GPIO_CHANNEL   1

//识别结果缓存: 重复调用(显示链路/触摸各需要一次)直接返回,
//避免显示已启动后再次翻转lcd_id引脚方向
static u16 s_id = 0;
static u16 s_w = 0, s_h = 0;

//读取LCD屏ID(3位拨码编码, 正点原子屏线序)
//PG6=R7(M0);PI2=G7(M1);PI7=B7(M2)
//M2:M1:M0
//0 :0 :0	//4.3寸480*272 RGB屏,ID=0X4342
//0 :0 :1	//7寸800*480 RGB屏,ID=0X7084
//0 :1 :0	//7寸1024*600 RGB屏,ID=0X7016
//1 :0 :0	//4.3寸800*480 RGB屏,ID=0X4384
//1 :0 :1	//10.1寸1280*800 RGB屏,ID=0X1018
static u16 lcd_id_read(void)
{
    u32 idx = axi_gpio_read(DISP_ID_GPIO_CHANNEL);
    switch (idx)
    {
        case 0:  return 0x4342;   //4.3寸屏, 480*272分辨率
        case 1:  return 0x7084;   //7寸屏, 800*480分辨率
        case 2:  return 0x7016;   //7寸屏, 1024*600分辨率
        case 4:  return 0x4384;   //4.3寸屏, 800*480分辨率
        case 5:  return 0x1018;   //10.1寸屏, 1280*800分辨率
        default: return 0;
    }
}

u16 lcd_panel_detect(u16* width, u16* height)
{
    if (s_id != 0)
    {
        *width  = s_w;
        *height = s_h;
        return s_id;
    }

    //lcd_id 引脚: 先设输入读ID, 再恢复输出
    if (axi_gpio_open(DISP_ID_GPIO_BASEADDR) != 0)
    {
        xil_printf("[lcd_panel] axi gpio open failed\r\n");
        *width  = 800;
        *height = 480;
        return 0;
    }
    axi_gpio_set_dir(DISP_ID_GPIO_CHANNEL, 0x07);
    u16 id = lcd_id_read();
    axi_gpio_set_dir(DISP_ID_GPIO_CHANNEL, 0x00);
    xil_printf("LCD ID: %x\r\n", id);

    //ID → 建议显示尺寸(来自实验11)
    u32 w, h;
    switch (id)
    {
        case 0x4342: w = 480;  h = 272;  break;
        case 0x4384: w = 800;  h = 480;  break;
        case 0x7084: w = 800;  h = 480;  break;
        case 0x7016: w = 1024; h = 600;  break;
        case 0x1018: w = 1280; h = 800;  break;
        default:     w = 800;  h = 480;  break;
    }
    *width  = (u16)w;
    *height = (u16)h;
    s_id = id;
    s_w  = (u16)w;
    s_h  = (u16)h;
    return id;
}
