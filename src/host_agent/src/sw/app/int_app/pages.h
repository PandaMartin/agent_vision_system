// =============================================================================
// pages —— 内置应用清单(每个应用一个.c文件, 导出const LauncherApp实例)
//
// sw/app/int_app/ = 内置应用(编译进固件, 图标常驻桌面):
//   app_camera.c   相机: 预览/拍照/相册      (UI+后端)
//   app_infer.c    推理: 实时推理/图片推理/模型管理 (UI+后端)
//   app_gimbal.c   云台: 预览+舵机方向控制    (UI; 舵机直驱见文件头说明)
//   app_files.c    文件: SD目录浏览/删除      (UI+后端)
//   app_transfer.c 文件传输: TFTP(UDP 69)推送落SD (UI+后端, 门控net_server)
//   app_test.c     隔离测试应用               (UI)
// sw/app/ext_app/ = SD卡加载应用的支持框架(sdapp扫描/启动 + loader运行时)
// 新应用: 复制_template.c起手, 在此声明+launcher_init注册。
// =============================================================================
#pragma once

#include "sw/app/launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const LauncherApp app_camera;     //相机应用(int_app/app_camera.c)
extern const LauncherApp app_infer;      //推理应用(int_app/app_infer.c)
extern const LauncherApp app_gimbal;     //云台应用(int_app/app_gimbal.c)
extern const LauncherApp app_files;      //文件应用(int_app/app_files.c)
extern const LauncherApp app_transfer;   //文件传输应用(int_app/app_transfer.c)
extern const LauncherApp app_test;       //隔离测试应用(int_app/app_test.c)

#ifdef __cplusplus
}
#endif
