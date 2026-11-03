// =============================================================================
// nn_runner —— 推理引擎(平台侧): 权重装载 + 前向 + 推理调度
//
// 引擎只认识契约(nn_pipeline.h), 不认识任何具体模型:
//   nn_load    装载权重(模型记录来自model_mgr)
//   nn_infer   一次完整推理 = 调模块preprocess(①) -> 引擎前向(②, 按模块的
//              输出声明抽取) -> 调模块decode(③, 出NnResult终稿)
// 结果交调用方发布。ncnn是C++ABI故实现为cpp, 对外保持C链接。
// =============================================================================
#pragma once

#include "sw/impl/nn/nn_model.h"
#include "sw/impl/nn/nn_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

//加载模型权重(model.param.bin/model.bin); 重复调用先卸载旧模型再加载
//返回0成功; 负数失败(原因已串口打印)
int nn_load(const NnModel* m);

//卸载当前模型权重
void nn_unload(void);

//当前模型记录(未加载=0; 记录体归model_mgr所有)
const NnModel* nn_current(void);

//一次完整推理。返回0成功; -1无模型 -2预处理失败 -3前向失败 -4解码失败
//(调用方按失败统一提示; 失败时res内容不定)
int nn_infer(const NnImage* img, NnResult* res);

#ifdef __cplusplus
}
#endif
