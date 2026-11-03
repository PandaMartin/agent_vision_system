// =============================================================================
// ovl_draw —— RGB888缓冲直写叠加渲染(检测框/标签条, 不经LVGL)
//
// 服务对象: vidwin把检测框画进LCD帧缓冲"视频洞"与HDMI摄像头帧环帧,
// 两处都是DMA/直写区域, LVGL控件进不去; 本模块提供像素级图元:
//   四角括号框 / 标签底色条+文字 / 实心矩形
//
// 像素格式: 与全链路一致的RGB888, 内存序B,G,R(见photo.h顶注);
// 文字直接读取font_cn_16的4bpp字形位图做alpha混合(只读const字体数据,
// 任意任务可调), 比内置点阵字库省一份字体且支持字库内全部字符。
//
// 本模块不做cache操作: 写"DMA在写的缓冲"(帧环)时的失效/冲刷纪律
// 由调用方(vidwin)按其绘制区域负责。
// =============================================================================
#pragma once

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

//类别色板(与推理应用LVGL侧共用同一张表, 同类同色)
//返回0xRRGGBB
u32 ovl_cls_color(int cls);

//实心矩形(自动裁剪到缓冲范围)
void ovl_fill(u8* buf, int W, int H,
              int x0, int y0, int x1, int y1, u32 rgb);

//四边全框(线宽t, 自动裁剪到缓冲范围)
void ovl_draw_box(u8* buf, int W, int H,
                  int x0, int y0, int x1, int y1, int t, u32 rgb);

//标签条: 类别色底条 + 白字(font_cn_16, 4bpp抗锯齿), scale=1/2整数放大
//(x,y)=条左上角锚点; 返回条总高(像素), 供调用方做越界翻转
int ovl_draw_label(u8* buf, int W, int H, int x, int y,
                   const char* text, u32 bg_rgb, int scale);

#ifdef __cplusplus
}
#endif
