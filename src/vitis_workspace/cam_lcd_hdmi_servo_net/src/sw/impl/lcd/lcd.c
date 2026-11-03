// =============================================================================
// lcd —— LCD帧缓存绘制原语实现
//
// 自原sw/lcd_vdma/lcd_vdma.cpp拆出(链路部分归hw/chain/lcd_chain):
// 绘制逻辑逐行平移, 固定地址与cache改经port, xil_printf改port_log
// =============================================================================

#include "lcd.h"

#include <string.h>

#include "sw/impl/port/port.h"

#define LCD_BYTES_PIXEL 3   //RGB888占3个字节

int lcd_colorbar(void)
{
    const uint16_t width  = port_fb_w();
    const uint16_t height = port_fb_h();
    if (width == 0)
    {
        port_log("[lcd] not initialized\r\n");
        return -1;
    }

    //---- 在帧缓存画彩虹彩条(画一次即可, VDMA硬件持续刷新) ----
    uint8_t* frame  = (uint8_t*)port_fb_base();
    uint32_t stride = port_fb_stride();
    uint32_t edge   = (uint32_t)width * LCD_BYTES_PIXEL / 7;
    uint32_t y_stride = 0;

    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < stride; x += LCD_BYTES_PIXEL)
        {
            uint8_t r = 0, g = 0, b = 0;
            if      (x < edge)     { r = 0xFF; g = 0;    b = 0;    } //红
            else if (x < edge * 2) { r = 0xFF; g = 0x7F; b = 0;    } //橙
            else if (x < edge * 3) { r = 0xFF; g = 0xFF; b = 0;    } //黄
            else if (x < edge * 4) { r = 0;    g = 0xFF; b = 0;    } //绿
            else if (x < edge * 5) { r = 0;    g = 0xFF; b = 0xFF; } //青
            else if (x < edge * 6) { r = 0;    g = 0;    b = 0xFF; } //蓝
            else                   { r = 0xEE; g = 82;   b = 0xEE; } //紫
            frame[x + y_stride + 0] = b;
            frame[x + y_stride + 1] = g;
            frame[x + y_stride + 2] = r;
        }
        y_stride += stride;
    }
    port_cache_flush(port_fb_base(), stride * height);

    port_log("[lcd] colorbar %ux%u\r\n", width, height);
    return 0;
}

// -----------------------------------------------------------------------------
// 帧缓存绘制原语
// cache策略: 不能全局关D-Cache(会拖垮ncnn推理), 也不整帧flush(太粗),
// 画线只flush笔触触达的脏行区间

//在y行的x0~x1列写一段颜色(坐标已由调用方保证在屏内)
static void span_fill(uint8_t* frame, uint32_t stride, int y, int x0, int x1,
                      uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t* p = frame + (uint32_t)y * stride + (uint32_t)x0 * LCD_BYTES_PIXEL;
    for (int x = x0; x <= x1; x++)
    {
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p += LCD_BYTES_PIXEL;
    }
}

//圆形笔触落点: 以(cx,cy)为圆心画实心圆, 触达行记入脏行区间[y0,y1]
static void pen_stamp(uint8_t* frame, uint32_t stride, int w, int h,
                      int cx, int cy, int rad, uint8_t r, uint8_t g, uint8_t b,
                      int* y0, int* y1)
{
    for (int dy = -rad; dy <= rad; dy++)
    {
        int y = cy + dy;
        if (y < 0 || y >= h)
            continue;
        int rem = rad * rad - dy * dy;   //该行半宽平方的上限
        int hw = 0;
        while ((hw + 1) * (hw + 1) <= rem)
            hw++;
        int x0 = cx - hw;
        int x1 = cx + hw;
        if (x0 < 0)  x0 = 0;
        if (x1 >= w) x1 = w - 1;
        if (x0 > x1)
            continue;
        span_fill(frame, stride, y, x0, x1, r, g, b);
        if (y < *y0) *y0 = y;
        if (y > *y1) *y1 = y;
    }
}

int lcd_clear(uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t height = port_fb_h();
    if (height == 0)
    {
        port_log("[lcd] not initialized\r\n");
        return -1;
    }

    uint8_t* frame  = (uint8_t*)port_fb_base();
    uint32_t stride = port_fb_stride();
    uint32_t y_off  = 0;
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < stride; x += LCD_BYTES_PIXEL)
        {
            frame[x + y_off + 0] = b;
            frame[x + y_off + 1] = g;
            frame[x + y_off + 2] = r;
        }
        y_off += stride;
    }
    port_cache_flush(frame, stride * height);
    return 0;
}

uint8_t* lcd_frame(uint32_t* stride, uint16_t* width, uint16_t* height)
{
    if (port_fb_w() == 0)
        return 0;
    if (stride) *stride = port_fb_stride();
    if (width)  *width  = port_fb_w();
    if (height) *height = port_fb_h();
    return (uint8_t*)port_fb_base();
}

int lcd_draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                  uint8_t size, uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t width  = port_fb_w();
    const uint16_t height = port_fb_h();
    if (width == 0)
    {
        port_log("[lcd] not initialized\r\n");
        return -1;
    }

    uint8_t* frame  = (uint8_t*)port_fb_base();
    uint32_t stride = port_fb_stride();
    const int w = (int)width;
    const int h = (int)height;

    //Bresenham直线(整数误差累积), 每步盖一个圆形笔触
    int ax = (int)x0, ay = (int)y0;
    int xerr = 0, yerr = 0;
    int deltax = (int)x1 - ax;
    int deltay = (int)y1 - ay;
    int incx = (deltax > 0) - (deltax < 0);
    int incy = (deltay > 0) - (deltay < 0);
    deltax = deltax < 0 ? -deltax : deltax;
    deltay = deltay < 0 ? -deltay : deltay;
    int distance = (deltax > deltay) ? deltax : deltay;

    int y_lo = h, y_hi = -1;   //脏行区间
    for (int t = 0; t <= distance + 1; t++)
    {
        pen_stamp(frame, stride, w, h, ax, ay, (int)size, r, g, b, &y_lo, &y_hi);
        xerr += deltax;
        yerr += deltay;
        if (xerr > distance)
        {
            xerr -= distance;
            ax += incx;
        }
        if (yerr > distance)
        {
            yerr -= distance;
            ay += incy;
        }
    }

    if (y_hi >= y_lo)
        port_cache_flush(frame + (uint32_t)y_lo * stride,
                         (uint32_t)(y_hi - y_lo + 1) * stride);
    return 0;
}
