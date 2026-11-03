// =============================================================================
// model_mgr —— SD卡模型管理(运行期)
//
// SD卡目录约定(加新模型=拷文件夹, 不改代码):
//   /models/<文件夹名>/
//     model.cfg        档案配置(key=value, 格式见cfg文件头注释)
//     model.param.bin  ncnn二进制网络结构
//     model.bin        ncnn权重
//     labels.txt       类别名每行一个, 行号=类别号(检测0号=background)
//
// post 可为 classify | detect | retinaface
// =============================================================================
#pragma once

#include "sw/nn/model_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

//模型目录上限
#define NN_MAX_MODELS  8

//扫描SD卡 /models/ 下所有含合法 model.cfg 的子目录, 打印清单
//返回模型数(0=没有可用模型)
int nn_models_scan(void);

//扫描到的模型数
int nn_models_count(void);

//第index个模型的显示名(越界返回NULL)
const char* nn_model_name(int index);

//激活(加载)第index个模型: 解析cfg+labels.txt+权重
//可重复调用 = 切换模型(旧模型先卸载); 返回0成功
int nn_model_activate(int index);

//当前激活的模型索引(未激活=-1)
int nn_active_index(void);

#ifdef __cplusplus
}
#endif
