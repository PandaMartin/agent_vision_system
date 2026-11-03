// =============================================================================
// lcd_chain —— LCD显示链路子系统实现
//
// 链路编排自原sw/lcd_vdma/lcd_vdma.cpp的lcd_vdma_init逐行平移:
// 原文件一分为二, 链路部分归本文件(hw层), 帧缓存绘制原语归impl/lcd
// =============================================================================

#include "lcd_chain.h"

#include "xil_printf.h"
#include "xparameters.h"
#include "xaxivdma.h"

#include "hw/dev/lcd_panel.h"
#include "hw/ip/vdma.h"
#include "hw/ip/clk_wiz.h"
#include "hw/ip/vtc_api.h"

#include "hw/mem_layout.h"   //MEM_LCD_BUFFER_BASE(链路配置用固定地址)

//SDT流程下xparameters.h不再定义DEVICE_ID，LookupConfig/Initialize改用基地址宏
//LCD链路固定使用VDMA0(读)/VTC0/clk_wiz_0
#define LCD_VDMA_BASEADDR    XPAR_XAXIVDMA_0_BASEADDR   //VDMA0(LCD读)基地址
#define LCD_VTC_BASEADDR     XPAR_XVTC_0_BASEADDR       //VTC0(LCD时序)基地址
#define LCD_CLK_WIZ_BASEADDR XPAR_XCLK_WIZ_0_BASEADDR   //时钟IP核(LCD像素时钟)

//本模块私有的外设实例与状态
static XAxiVdma          s_vdma;
static VtcDev            s_disp;
static const VideoMode*  s_mode = 0;   //启动时识别到的显示模式

int lcd_chain_start(void)
{
    //---- 1.自识别LCD型号, 得到显示时序 ----
    u16 w = 0, h = 0;
    lcd_panel_detect(&w, &h);
    s_mode = lcd_find_mode(w, h);
    if (s_mode == 0)
    {
        xil_printf("[lcd] no VideoMode for %ux%u\r\n", w, h);
        return -1;
    }

    //---- 2.VDMA0只读通道, 单帧缓冲 ----
    //显式不带GenlockEn: VDMA0无配对主设备, 单缓冲自由跑即为正确形态
    //(该总开关是配置期全局, 相机链会在自己的start里设1, 这里显式归零防串扰)
    vdma_genlock(0);
    if (run_vdma_frame_buffer(&s_vdma, LCD_VDMA_BASEADDR,
                              (int)s_mode->width, (int)s_mode->height,
                              (int)MEM_LCD_BUFFER_BASE, 0, 0, ONLY_READ) != 0)
    {
        xil_printf("[lcd] VDMA0 setup failed\r\n");
        return -2;
    }

    //---- 3.LCD像素时钟 + VTC0, 链路自由跑 ----
    clk_wiz_cfg(LCD_CLK_WIZ_BASEADDR, s_mode->freq);
    vtc_dev_init(&s_disp, LCD_VTC_BASEADDR);
    vtc_dev_set_mode(&s_disp, s_mode);
    if (vtc_dev_start(&s_disp) != 0)
    {
        xil_printf("[lcd] display start failed\r\n");
        return -3;
    }
    return 0;
}

void lcd_chain_stop(void)
{
    if (s_mode == 0)
        return;
    vtc_dev_stop(&s_disp);
    s_mode = 0;
}

const VideoMode* lcd_chain_mode(void)
{
    return s_mode;
}
