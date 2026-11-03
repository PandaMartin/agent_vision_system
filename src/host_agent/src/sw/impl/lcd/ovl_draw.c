// =============================================================================
// ovl_draw —— RGB888缓冲直写叠加渲染实现(见ovl_draw.h顶注)
// =============================================================================

#include "sw/impl/lcd/ovl_draw.h"

#include <string.h>

#include "lvgl.h"
#include "src/font/lv_font_fmt_txt.h"

//类别色板: 鲜艳8色循环(同类同色); 与infer.c的LVGL侧共用本表
static const u32 s_pal[8] = {
    0xFF453A,   //红
    0x32D74B,   //绿
    0x00E5FF,   //青
    0xFFD60A,   //黄
    0xFF375F,   //品红
    0xFF9F0A,   //橙
    0x7D7AFF,   //紫
    0x66D4CF,   //水鸭
};

u32 ovl_cls_color(int cls)
{
    return s_pal[cls & 7];
}

// -----------------------------------------------------------------------------
// 基础图元(全部自动裁剪; 纯写不读旧像素, cache纪律归调用方)

void ovl_fill(u8* buf, int W, int H,
              int x0, int y0, int x1, int y1, u32 rgb)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    if (x0 > x1 || y0 > y1)
        return;
    for (int y = y0; y <= y1; y++)
    {
        u8* p = buf + ((size_t)y * W + x0) * 3;
        for (int x = x0; x <= x1; x++)
        {
            p[0] = (u8)(rgb);
            p[1] = (u8)(rgb >> 8);
            p[2] = (u8)(rgb >> 16);
            p += 3;
        }
    }
}

void ovl_draw_box(u8* buf, int W, int H,
                  int x0, int y0, int x1, int y1, int t, u32 rgb)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (t > (x1 - x0 + 1) / 2 || t > (y1 - y0 + 1) / 2)
        t = 1;                       //极小框退化为1px

    ovl_fill(buf, W, H, x0, y0, x1, y0 + t - 1, rgb);           //上边
    ovl_fill(buf, W, H, x0, y1 - t + 1, x1, y1, rgb);           //下边
    ovl_fill(buf, W, H, x0, y0 + t, x0 + t - 1, y1 - t, rgb);   //左边
    ovl_fill(buf, W, H, x1 - t + 1, y0 + t, x1, y1 - t, rgb);   //右边
}

// -----------------------------------------------------------------------------
// 文字: 直接读font_cn_16的4bpp字形位图(只读const数据, 无LVGL状态介入)

//UTF-8解码一个码点, 返回消耗字节数
static int utf8_decode(const char* p, u32* cp)
{
    const u8 c = (u8)p[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
    { *cp = ((u32)(c & 0x1F) << 6) | (u32)(p[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    { *cp = ((u32)(c & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6)
          | (u32)(p[2] & 0x3F); return 3; }
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80
                          && (p[3] & 0xC0) == 0x80)
    { *cp = ((u32)(c & 0x07) << 18) | ((u32)(p[1] & 0x3F) << 12)
          | ((u32)(p[2] & 0x3F) << 6) | (u32)(p[3] & 0x3F); return 4; }
    *cp = '?';
    return 1;
}

int ovl_draw_label(u8* buf, int W, int H, int x, int y,
                   const char* text, u32 bg_rgb, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;

    //---- 1.量宽(码点步进累加, 上限20字符/200px, 防超长标签) ----
    int tw = 0;
    int nch = 0;
    for (const char* p = text; *p != 0 && nch < 20; nch++)
    {
        u32 cp;
        p += utf8_decode(p, &cp);
        lv_font_glyph_dsc_t d;
        if (lv_font_get_glyph_dsc(&font_cn_16, &d, cp, 0))
            tw += d.adv_w;
        else
            tw += 8;                       //缺字按半角补位
    }
    if (tw > 200)
        tw = 200;

    //---- 2.底色条 ----
    const int pad = 2 * scale;
    const int th = 16 * scale + pad * 2;   //条高
    ovl_fill(buf, W, H, x, y, x + tw * scale + pad * 2 - 1, y + th - 1, bg_rgb);

    //---- 3.逐字形alpha混合到条上(读的是自己刚写的底色, 单写者安全) ----
    const lv_font_t* f = &font_cn_16;
    const lv_font_fmt_txt_dsc_t* fd = (const lv_font_fmt_txt_dsc_t*)f->dsc;
    const int baseline = f->line_height - f->base_line;   //基线距条顶(pad起)
    int px = x + pad;
    nch = 0;
    for (const char* p = text; *p != 0 && nch < 20 && px < x + tw * scale + pad;
         nch++)
    {
        u32 cp;
        p += utf8_decode(p, &cp);
        lv_font_glyph_dsc_t d;
        if (!lv_font_get_glyph_dsc(f, &d, cp, 0) || d.gid.index == 0)
        {
            px += 8 * scale;                   //缺字(notdef)按半角补位
            continue;
        }
        const lv_font_fmt_txt_glyph_dsc_t* g = &fd->glyph_dsc[d.gid.index];
        const u8* bmp = fd->glyph_bitmap + g->bitmap_index;
        const int gw = d.box_w, gh = d.box_h;
        //位图为跨行连续的4bpp半字节流(行间不按字节对齐, 奇数宽度字形
        //两行共享一个字节) —— 与LVGL自解码一致, 按全局半字节序号索引
        const int gy = y + pad + (baseline - gh + d.ofs_y) * scale;

        for (int r = 0; r < gh; r++)
        {
            for (int c = 0; c < gw; c++)
            {
                const int n = r * gw + c;
                const u8 byte = bmp[n >> 1];
                const int a = (n & 1) ? (byte & 0xF) : (byte >> 4);
                if (a == 0)
                    continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                    {
                        const int dx = px + d.ofs_x * scale + c * scale + sx;
                        const int dy = gy + r * scale + sy;
                        if (dx < 0 || dy < 0 || dx >= W || dy >= H)
                            continue;
                        u8* q = buf + ((size_t)dy * W + dx) * 3;
                        //白字按alpha混到类别色底上
                        q[0] = (u8)((q[0] * (15 - a) + 0xFF * a) / 15);
                        q[1] = (u8)((q[1] * (15 - a) + 0xFF * a) / 15);
                        q[2] = (u8)((q[2] * (15 - a) + 0xFF * a) / 15);
                    }
            }
        }
        px += d.adv_w * scale;
    }
    return th;
}
