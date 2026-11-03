// =============================================================================
// nn_module —— 模型程序模块(model.mod)加载器(固件侧)
//
// model.mod 由 tools/nnmod/nnmod.py 封包(64字节NmodHeader + 代码数据), 加载
// 机制与SD应用(ZAPP)同构: 校验 -> 整读 -> 搬运到 MEM_MODEL_MOD_BASE ->
// bss清零 -> cache同步 -> 调镜像偏移0的 nn_mod_boot() 取模型定义。
// 全部函数限 app_task 上下文(FatFs纪律)。契约见 nn_pipeline.h。
// =============================================================================
#pragma once

#include <stdint.h>

#include "sw/impl/nn/nn_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

//NmodHeader —— model.mod 64字节头(与 nnmod.py HDR_FMT 逐字节一致)
typedef struct {
    char     magic[4];      //"NMOD"
    uint32_t hdr_size;      //=64
    uint32_t abi_ver;       //契约版本(=NN_PIPELINE_ABI_VER)
    uint32_t fw_version;    //封包时刻的固件符号导出版本(低32位)
    uint32_t load_size;     //头之后 text+data 字节数(搬运进模块区)
    uint32_t bss_size;      //bss 字节数(加载器清零)
    char     name[16];      //构建名(<=15字节UTF-8, 日志用; 模型自述看 def->name/tag)
    uint32_t entry_off;     //恒0(nn_mod_boot 在区基址)
    uint32_t total_size;    //整个bin字节数(=64+load_size)
    uint32_t bss_off;       //bss相对区基址偏移
    uint8_t  pad[12];
} NnModHeader;
typedef char nnmod_hdr_size_check[(sizeof(NnModHeader) == 64) ? 1 : -1];

//加载模块: 校验(magic/尺寸/abi/固件版本) -> 搬运 -> 清bss -> cache同步 ->
//调 nn_mod_boot 并校验返回的定义。成功返回定义指针(常驻, 下次load前有效);
//失败返回0, 原因已串口打印。重复调用自动卸载上一个。
const NnModelDef* nn_mod_load(const char* path);

//卸载当前模块(定义指针失效; 模块区由下次load整块覆盖, 无需擦除)
void nn_mod_unload(void);

//当前已加载模块的定义(未加载=0)
const NnModelDef* nn_mod_current(void);

#ifdef __cplusplus
}
#endif
