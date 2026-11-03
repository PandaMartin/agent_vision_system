// =============================================================================
// model_mgr —— SD模型包管理(运行期)
//
// SD目录约定(一个文件夹=一个完整模型, 文件夹名=模型名):
//   /models/<名>/
//     model.param.bin  ncnn网络结构(ncnn2mem产物)
//     model.bin        权重
//     labels.txt       类别名每行一个(行号=label号, 发布层翻译用)
//     model.mod        模型程序模块(nnmod.py产出, 见nn_pipeline.h契约)
// 没有 model.cfg —— 参数全部是模块内常量。
// 扫描时对每个候选"短暂加载模块读tag再卸载", 顺带完整校验模块可boot;
// 重扫会丢弃当前激活模型(清单重建, 需重新激活)。
// 全部函数限 app_task 上下文(FatFs纪律)。
// =============================================================================
#pragma once

#include "sw/impl/nn/nn_model.h"

#ifdef __cplusplus
extern "C" {
#endif

//模型目录上限
#define NN_MAX_MODELS  8

//扫描 /models 下合法模型文件夹(四件齐全+模块可boot), 返回模型数
int nn_models_scan(void);

//扫描到的模型数
int nn_models_count(void);

//第index个模型的文件夹名(越界返回0)
const char* nn_model_name(int index);

//第index个模型的类别标签(模块自述, 越界返回"")
const char* nn_model_tag(int index);

//第index个模型的记录(越界返回0; 激活后def/labels有效)
const NnModel* nn_model_record(int index);

//激活第index个模型: 加载labels+模块+权重(重复调用=切换); 返回0成功
int nn_model_activate(int index);

//当前激活的模型索引(未激活=-1)
int nn_active_index(void);

//当前激活的模型记录(未激活=0)
const NnModel* nn_active_model(void);

#ifdef __cplusplus
}
#endif
