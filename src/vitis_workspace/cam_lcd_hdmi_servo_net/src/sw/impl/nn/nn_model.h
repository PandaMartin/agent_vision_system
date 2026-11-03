// =============================================================================
// nn_model —— 模型记录(运行期, model_mgr 扫描/激活时填充)
//
// 旧 NnModelProfile "模型档案"已废除: 模型的全部私有知识(预处理/输出清单/
// 解码/阈值/锚参数)都在 model.mod 里, 固件侧只留调度必需的这点状态。
// =============================================================================
#pragma once

#include "sw/impl/nn/nn_pipeline.h"

typedef struct {
    char folder[48];            //模型文件夹名(=模型身份, SD:/models 下)
    char param_file[96];        //model.param.bin 完整路径(扫描时填)
    char bin_file[96];          //model.bin 完整路径(扫描时填)
    char tag[16];               //类别标签(模块 def->tag 自述, 扫描期短暂加载读取)
    //激活后有效:
    const NnModelDef* def;      //模块定义(nn_mod_load)
    const char* const* labels;  //labels.txt 逐行(存储归 model_mgr)
    int num_classes;            //类别数(=labels行数)
} NnModel;
