// =============================================================================
// ext_sdapp —— SD卡加载应用(ext_app)的支持后端: /apps扫描 + 校验 + 搬运启动
//
// 应用bin契约/格式见loader.h(AppHeader); 本模块跑在app_task上下文
// (FatFs纪律), gui侧(桌面图标/加载状态机)经bridge的sdapp/app组通信。
// 桌面重扫入口: launcher状态栏刷新按钮(APP_CMD_APP_RESCAN)与
// 文件传输应用落盘联动(0:/apps收到新bin即ext_sdapp_scan)。
// =============================================================================
#pragma once

#include "sw/app/appctl.h"

#ifdef __cplusplus
extern "C" {
#endif

//扫描 0:/apps 下两级目录的*.bin, 校验头后发布桌面图标清单(bridge.sdapp组)
void ext_sdapp_scan(void);

//命令分发(APP_CMD_APP_RESCAN/APP_CMD_APP_LAUNCH)
void ext_sdapp_dispatch(const AppCmd* cmd);

#ifdef __cplusplus
}
#endif
