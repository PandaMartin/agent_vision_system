// =============================================================================
// photo —— BMP照片存取实现
// =============================================================================

#include "sw/impl/sd/photo.h"

#include <string.h>
#include <stdlib.h>

#include "sw/impl/sd/sd.h"

extern "C" {
#include "xil_printf.h"
#include "ff.h"
}

//字段写入(小端)
static void put16(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8* p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static u32 get32(const u8* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

//54字节标准BMP文件头(信息头40, 24bpp, 2835ppm分辨率)
static void bmp_head_init(u8* h, u32 w, u32 hgt)
{
    memset(h, 0, 54);
    h[0] = 'B'; h[1] = 'M';
    put32(h + 2, 54 + w * hgt * 3);        //文件大小
    put32(h + 10, 54);                     //像素数据偏移
    put32(h + 14, 40);                     //信息头大小
    put32(h + 18, w);                      //宽(正=底行在先)
    put32(h + 22, hgt);                    //高
    put16(h + 26, 1);                      //平面数
    put16(h + 28, 24);                     //bpp
    put32(h + 34, w * hgt * 3);            //像素数据大小
    put32(h + 38, 2835);                   //水平分辨率(72dpi)
    put32(h + 42, 2835);
}

int photo_save_bmp(const char* path, const u8* frame, u32 w, u32 hgt)
{
    //前置: sd_mount已由调用方完成(本函数被调即挂载过, 直查挂载标志的职责
    //在sd层; 这里只对打开失败负责)
    u8 head[54];
    bmp_head_init(head, w, hgt);

    FIL fp;
    FRESULT fr = f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
    {
        xil_printf("[photo] create %s failed (%d)\r\n", path, (int)fr);
        return -2;
    }

    UINT put = 0;
    fr = f_write(&fp, head, sizeof(head), &put);
    if (fr == FR_OK && put != sizeof(head))
        fr = (FRESULT)1;                   //头没写完整按失败计

    //BMP底行在先: 从帧的最后一行往前逐行写(行宽3840@720p, 单次f_write足够)
    const u32 stride = w * 3;
    for (u32 y = hgt; y > 0 && fr == FR_OK; y--)
    {
        fr = f_write(&fp, frame + (y - 1) * stride, stride, &put);
        if (fr == FR_OK && put != stride)
            fr = (FRESULT)1;               //写不完整按失败计
    }
    if (fr == FR_OK)
        fr = f_sync(&fp);
    f_close(&fp);

    if (fr != FR_OK)
    {
        xil_printf("[photo] write %s failed (%d)\r\n", path, (int)fr);
        return -3;
    }
    return 0;
}

int photo_probe_bmp(const u8* file, u32 len, u32* w, u32* h, u32* data_off)
{
    if (len < 54 || file[0] != 'B' || file[1] != 'M')
        return -1;
    const u32 off = get32(file + 10);      //像素数据偏移
    const u32 isz = get32(file + 14);      //信息头大小(40=BITMAPINFOHEADER)
    const u32 wd = get32(file + 18);
    const u32 ht = get32(file + 22);
    const u32 bpp = (u32)file[28] | ((u32)file[29] << 8);
    const u32 comp = get32(file + 30);

    if (isz < 40 || bpp != 24 || comp != 0 || wd == 0 || ht == 0
        || wd > 4096 || ht > 4096 || off < 54 || off >= len)
        return -1;
    if (w) *w = wd;
    if (h) *h = ht;
    if (data_off) *data_off = off;
    return 0;
}

void photo_flip_rows(u8* pix, u32 w, u32 hgt)
{
    const u32 stride = w * 3;
    static u8 tmp[4096];                   //单行交换缓冲(单线程纪律下安全)
    if (stride == 0 || stride > sizeof(tmp))
        return;
    for (u32 a = 0, b = hgt - 1; a < b; a++, b--)
    {
        memcpy(tmp, pix + a * stride, stride);
        memcpy(pix + a * stride, pix + b * stride, stride);
        memcpy(pix + b * stride, tmp, stride);
    }
}

// -----------------------------------------------------------------------------
// 主题壁纸加载(桌面背景美化, 见 photo.h 契约)
u8* photo_load_wallpaper(const char* path, u32 req_w, u32 req_h)
{
    unsigned long len = 0;
    unsigned char* raw = sd_read_file(path, &len);
    if (raw == 0)
        return 0;                          //没有壁纸: 正常(保持渐变背景)

    u32 w = req_w, h = req_h, off = 0;     //预填防probe失败后打印未初始化
    if (photo_probe_bmp(raw, (u32)len, &w, &h, &off) != 0
        || w != req_w || h != req_h)
    {
        xil_printf("[theme] wallpaper rejected: want %ux%u 24bpp, got %ux%u\r\n",
                   (unsigned)req_w, (unsigned)req_h, (unsigned)w, (unsigned)h);
        free(raw);
        return 0;
    }
    photo_flip_rows(raw + off, w, h);      //BMP底行在先 -> 帧序顶行在先
    xil_printf("[theme] wallpaper %ux%u loaded (%lu bytes)\r\n",
               (unsigned)w, (unsigned)h, len);
    return raw + off;
}
