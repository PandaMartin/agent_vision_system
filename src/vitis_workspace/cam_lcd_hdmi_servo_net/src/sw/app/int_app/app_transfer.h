// =============================================================================
// app_transfer —— 文件传输应用接口
//
// UI(gui任务)在int_app/app_transfer.c; 后端(app_task)同文件:
// 进入应用开监听(net_server_start), 退出停监听(net_server_stop),
// 收满的文件经APP_CMD_NET_FILE落SD并按目录联动重扫。
// =============================================================================
#pragma once

#include "sw/app/appctl.h"

#ifdef __cplusplus
extern "C" {
#endif

//命令分发(APP_CMD_NET_START/NET_STOP/NET_FILE)
void app_transfer_dispatch(const AppCmd* cmd);

//1s节拍(main.cpp app_task): 传输相关周期维护(落盘联动重扫的去抖补扫)
void app_transfer_tick(void);

#ifdef __cplusplus
}
#endif
