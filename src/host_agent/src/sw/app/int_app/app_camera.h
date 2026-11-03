// =============================================================================
// app_camera —— 相机应用后端接口(app_task上下文; UI在app_camera.c内)
// =============================================================================
#pragma once

#include "xil_types.h"
#include "sw/app/appctl.h"

#ifdef __cplusplus
extern "C" {
#endif

//命令分发(APP_CMD_CAM_OPEN/CAM_CLOSE/HDMI_SET/CAPTURE/PHOTO_LOAD/PHOTO_UNLOAD)
void app_camera_dispatch(const AppCmd* cmd);

//当前查看中的照片像素(BGR顶行在先; 无图返回0, 尺寸写0)
//推理应用图片模式经它取输入帧(与摄像头帧同像素序, 零转换)
const void* app_camera_photo(int* w, int* h);

#ifdef __cplusplus
}
#endif
