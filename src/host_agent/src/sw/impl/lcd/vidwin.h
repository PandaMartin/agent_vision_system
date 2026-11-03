// =============================================================================
// vidwin —— 摄像头画面LCD窗口(方案①: CPU降采样直写帧缓冲)
//
// 独立视频任务把摄像头画面以高帧率降采样进LCD帧缓冲的一个固定窗口区:
//   摄像头环形缓冲(零拷贝取"刚写完"的帧) --最近邻降采样--> LCD帧缓冲窗口洞
// 与LVGL的分工约定: 应用布局给窗口让出"洞"(区域内无任何LVGL控件),
// LVGL的局部渲染只刷脏区, 稳态不会触碰洞。
//
// 窗口几何: 720p半分辨率(640x360, 16:9), 水平居中、与应用屏内容区顶部平齐
// (相机应用的底栏控制区在洞之下, 互不重叠)。
//
// 生命周期(相机应用控制):
//   开机        vidwin_init只存LCD尺寸(布局参数, 不建任务不碰相机);
//   进相机应用  on_open -> APP_CMD_CAM_OPEN -> cam_chain_start成功后
//               vidwin_task_start_centered(开窗+建任务);
//   出相机应用  on_close -> vidwin_enable(0)+vidwin_task_stop(先停笔,
//               保证退场动画期间不再有写帧) -> APP_CMD_CAM_CLOSE停链路。
//
// 显示开关: 应用内切换到相册/查看器时vidwin_enable(0)停笔(零CPU),
// 回实时画面enable(1)下一帧自动恢复。
// =============================================================================
#pragma once

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

//视频窗尺寸(720p的3/8缩放480x270, 16:9; 相机应用布局与此镜像, 洞内不放控件)
//窗下留出更高的控制栏(推理/相机/云台应用共用此几何)
#define VIDWIN_VID_W  480
#define VIDWIN_VID_H  270

//开机存LCD尺寸(app_init中、LCD链路就绪后调用, 不依赖相机链路)
//返回0成功; -1参数非法(屏小于窗口)
int vidwin_init(u16 lcd_w, u16 lcd_h);

//取视频洞的LCD屏坐标(未开窗时无效, 返回-1)
int vidwin_hole_rect(u16* x, u16* y, u16* w, u16* h);

//开窗(内容区水平居中+顶部平齐)+建预览任务(cam_chain_start成功后由
//app_task调用, 幂等: 任务已存在只重开窗)
void vidwin_task_start_centered(void);

//开窗水平对齐: 0=居中(默认) 1=靠左(洞贴内容区左边; 布局需右栏的应用用)
//下次开窗生效; 应用on_open置位/on_close还原
void vidwin_align_left(int on);

//HDMI输出是否叠加检测框(默认开; 推理应用三态按钮: 关/开无框/开带框)
//gui任务直调, 下一个视频帧节拍生效
void vidwin_hdmi_ovl_set(int on);
int  vidwin_hdmi_ovl_enabled(void);

//检测框跟手速度(每视频帧向推理目标靠近的百分比, 10~100, 默认60):
//gui滑块直调, 实时生效——值大跟手(跳变明显), 值小平滑(拖影明显)
void vidwin_ovl_smooth_set(int pct);

//停预览任务并等待退出(≤一个轮询周期; 可从gui任务on_close直接调,
//保证应用退场动画开始后不再有写帧)
void vidwin_task_stop(void);

//显示开关(1=写视频 0=完全停笔零CPU; 重开自动恢复)
void vidwin_enable(int on);
int  vidwin_enabled(void);

//---- 检测框叠加 ----
//LCD: vid任务每新帧做IoU配对+插值平滑后把框+标签画进视频洞(低推理
//     帧率下框连续滑动不跳变)
//HDMI: 同一组平滑框+标签由vid任务描进"刚写完"的帧环帧(摄像头VDMA
//     直通HDMI), 两屏内容一致。genlock锁相下MM2S与描框竞速, HDMI侧
//     框有残影/闪烁属通路固有限制(12工程同), 彻底消除需PL侧叠加

//加速模式回放(预览关时): 把"刚推理完"的一帧(CPU缓冲, 顶行在先BGR序)
//标注检测框后降采样上屏; 调用方app_task(每次推理后), 返回0成功
int  vidwin_show_annotated(const u8* frame);

//洞涂应用底色(预览关时遮残影, gui任务直调; 未开窗时无操作)
void vidwin_paint_bg(void);

//实测统计: fps=窗口实际刷新帧率, us=单帧降采样+刷cache耗时(微秒)
void vidwin_stats(u32* fps, u32* us);

#ifdef __cplusplus
}
#endif
