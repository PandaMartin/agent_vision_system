// =============================================================================
// app_files —— 文件应用后端接口(app_task上下文; UI在app_files.c内)
// =============================================================================
#pragma once

#include "sw/app/appctl.h"

#ifdef __cplusplus
extern "C" {
#endif

//命令分发(APP_CMD_FS_LIST/FS_DELETE/FS_COPY); 结果经bridge的fs组回传,
//操作成败经gui_model_pub_fsop累加(GUI按gen_fsop对账做汇总)
void app_files_dispatch(const AppCmd* cmd);

#ifdef __cplusplus
}
#endif
