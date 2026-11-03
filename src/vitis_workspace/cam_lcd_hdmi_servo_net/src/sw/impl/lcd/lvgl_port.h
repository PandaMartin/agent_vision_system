// =============================================================================
// gui —— LVGL 9.4 图形界面包(LCD子系统上的现代UI)
//
// 组成:
//   lv_conf.h     LVGL裁剪配置(RGB888软件渲染+NEON加速+暗色主题)
//   lvgl/         LVGL 9.4.0 官方源码(未启用的子系统编译为空翻译单元)
//   fonts/        自制中文子集字体(font_cn_16/20, 黑体, 界面文案字符集)
//   gui.c         移植层: tick挂FreeRTOS / 显示flush直拷VDMA0帧缓存+刷Cache /
//                 触摸indev(复用hw/touch) / gui任务(lv_timer_handler循环)
//   gui_model     数据桥(app_task发布快照, gui任务拉取; 含相机缩略图双缓冲)
//   gui_app       页面框架: 左导航壳+页面注册表+toast
//   pages/        页面: 监控(主)/模型/系统; 新功能=新文件+一行注册
//
// 线程模型: 所有LVGL API只在gui任务上下文执行(app_init建好界面后,
// 其他任务不再触碰LVGL), 因此LV_OS_NONE下无需加锁
// =============================================================================
#pragma once

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

//初始化LVGL并拉起gui任务(在app_init里、调度器启动前调用, 单线程环境安全):
//  1.lv_init + tick回调挂FreeRTOS tick(100Hz, 折算毫秒)
//  2.显示设备: RGB888, 双部分重绘缓冲, flush逐行memcpy到LCD帧缓存后刷D-Cache
//  3.触摸indev(触摸不可用时只是无输入, 界面照常渲染)
//  4.暗色默认主题 + 演示仪表盘界面
//  5.创建gui任务(16KB栈)循环驱动lv_timer_handler
//返回0成功; -1 LCD链路未初始化; -2绘制缓冲分配失败; -3任务创建失败
int gui_init(void);

//最近一秒的界面刷新帧数(gui任务统计, 供诊断/演示)
u32 gui_fps(void);

//当前触摸状态(演示界面状态栏显示用; 未按下时pressed=0)
void gui_touch_info(int* pressed, u16* x, u16* y);

#ifdef __cplusplus
}
#endif
