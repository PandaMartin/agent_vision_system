// =============================================================================
// app_infer —— 推理应用对外接口(内置应用, UI+后端在app_infer.c一文件内)
// =============================================================================
#pragma once

#include "xil_types.h"
#include "sw/app/appctl.h"

#ifdef __cplusplus
extern "C" {
#endif

//命令分发(app_task上下文, main.cpp的switch转入):
//RUN_SET/RUN_TOGGLE/MODEL_SELECT/MODELS_RESCAN/PHOTO_INFER
void app_infer_dispatch(const AppCmd* cmd);

//推理循环是否在跑(main循环查询; 状态由RUN_SET驱动)
int app_infer_running(void);

//推一帧推理(main循环在无命令且运行中时调用; 一帧=抓拍+前向+发布,
//约200~400ms, 命令在帧间得到响应)
void app_infer_pump(void);

//把model_mgr当前模型清单发布到数据桥(app_task上下文; 开机扫描/
//重扫/传输联动三处共用)
void app_infer_pub_models(void);

//把model_mgr当前激活模型状态发布到数据桥(app_task上下文; 自检装好
//模型/重扫丢弃模型后, 界面与实际激活态对齐)
void app_infer_pub_model_state(void);

#ifdef __cplusplus
}
#endif
