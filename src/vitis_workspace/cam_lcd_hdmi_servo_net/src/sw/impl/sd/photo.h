// =============================================================================
// photo —— BMP照片存取(相机应用)
//
// 像素格式全链路一致: 摄像头帧/LCD帧缓存/LVGL渲染缓冲/BMP文件都是
// RGB888且内存序B,G,R —— 保存零转换, 读回零转换(只差BMP的行序约定)。
//
// FatFs单线程纪律: 本模块全部函数只允许app_task调用(与sd.h同规)。
// =============================================================================
#pragma once

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

//把一帧图像存成24位BMP(帧序: 顶行在先, 即帧缓冲的自然行序; BMP约定
//底行在先, 本函数内部逐行倒序写出, 不动调用方的缓冲)
//path: 全路径(如 "0:/photos/IMG_0001.bmp", 目录须已存在)
//返回0成功; -1未挂载; -2创建失败; -3写失败
int photo_save_bmp(const char* path, const u8* frame, u32 w, u32 h);

//校验内存中的BMP是否为本模块可读的格式(BM/24bit/无压缩/尺寸合理)
//成功填 w/h/data_off(像素数据在文件内的偏移) 返回0; 格式不符-1
int photo_probe_bmp(const u8* file, u32 len, u32* w, u32* h, u32* data_off);

//行序原地翻转(底行在先<->顶行在先; 行缓冲区 4字节对齐时走字交换快路径)
void photo_flip_rows(u8* pix, u32 w, u32 h);

//加载主题壁纸(桌面背景美化): 24位BMP且尺寸须恰为 req_w x req_h(与LCD一致),
//像素内存序B,G,R与LVGL渲染缓冲同格式, 行序已翻成顶行在先。
//成功返回像素指针(sd_read_file缓冲内偏移, 常驻不释放——壁纸与固件同生命周期,
//这也是返回偏移指针而非整缓冲的原因: 免去gui侧记录free基址);
//文件不存在(正常)/格式或尺寸不符返回0。仅限app_task调用(FatFs纪律)
u8* photo_load_wallpaper(const char* path, u32 req_w, u32 req_h);

#ifdef __cplusplus
}
#endif
