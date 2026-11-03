// =============================================================================
// cam —— 摄像头帧访问实现
//
// 自原sw/cam_vdma/cam_vdma.cpp拆出(链路部分归hw/chain/cam_chain):
// 帧边界逻辑逐行平移, 帧环地址/写指针经cam_chain查询, cache/时间/日志经port
// =============================================================================

#include "cam.h"

#include <string.h>

#include "hw/chain/cam_chain.h"
#include "sw/impl/port/port.h"

int cam_grab_frame(uint8_t* dst, void (*idle)(void))
{
    const int W = cam_chain_width();
    const int H = cam_chain_height();
    if (W == 0)
        return -1;
    const uint32_t fsize = (uint32_t)W * (uint32_t)H * 3;

    //---- 1.等待帧边界: 摄像头写指针一变化, 刚写完的缓存就是完整帧 ----
    uint32_t wnow = cam_chain_write_index();
    uint32_t wcur = wnow;
    while (wcur == wnow)
    {
        if (idle)
            idle();
        wcur = cam_chain_write_index();
    }
    //此刻wnow帧刚写完, 摄像头在写wcur, 下下帧才会重新覆盖wnow, 时间充裕
    uint8_t* snap = cam_chain_frame_addr((int)wnow);
    port_cache_invalidate(snap, fsize);
    memcpy((void*)dst, (void*)snap, fsize);
    return (int)wnow;
}

int cam_snap_latest(const uint8_t** frame)
{
    if (frame == 0 || cam_chain_width() == 0)
        return -1;

    //写指针正在写的缓存是cur; 刚写完的是它的前一个(环形倒退一步)
    const uint32_t n   = cam_chain_frame_count();
    const uint32_t cur = cam_chain_write_index();
    const uint32_t last = (cur + n - 1) % n;

    *frame = cam_chain_frame_addr((int)last);
    return (int)last;
}

int cam_draw_overlays(const CamOverlayRect* rects, int n, unsigned timeout_ms)
{
    const int W = cam_chain_width();
    const int H = cam_chain_height();
    if (rects == 0 || n <= 0 || W == 0)
        return -1;

    //---- 1.等下一个帧边界(写指针翻转), 超时放弃 ----
    uint32_t wnow = cam_chain_write_index();
    uint32_t wcur = wnow;
    uint32_t t0 = port_time_ms();
    while (wcur == wnow)
    {
        if (port_time_ms() - t0 >= timeout_ms)
            return -1;
        wcur = cam_chain_write_index();
    }

    //---- 2.wnow帧刚写完, 显示即将轮到它, 现在描框 ----
    uint8_t* frame = cam_chain_frame_addr((int)wnow);
    const int t = 2;   //线宽(像素)

    for (int i = 0; i < n; i++)
    {
        int x0 = rects[i].x0;
        int y0 = rects[i].y0;
        int x1 = rects[i].x1;
        int y1 = rects[i].y1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= W) x1 = W - 1;
        if (y1 >= H) y1 = H - 1;
        if (x0 > x1 || y0 > y1)
            continue;

        //框覆盖行的cache: 先失效再改再冲刷(与VDMA写入路径隔离)
        uint8_t* reg = frame + (size_t)y0 * W * 3;
        uint32_t len = (uint32_t)(y1 - y0 + 1) * (uint32_t)W * 3;
        port_cache_invalidate(reg, len);

        for (int yy = y0; yy <= y1; yy++)
        {
            for (int xx = x0; xx <= x1; xx++)
            {
                if (yy < y0 + t || yy > y1 - t || xx < x0 + t || xx > x1 - t)
                {
                    uint8_t* p = frame + ((size_t)yy * W + xx) * 3;
                    p[0] = rects[i].r;
                    p[1] = rects[i].g;
                    p[2] = rects[i].b;
                }
            }
        }
        port_cache_flush(reg, len);
    }
    return 0;
}
