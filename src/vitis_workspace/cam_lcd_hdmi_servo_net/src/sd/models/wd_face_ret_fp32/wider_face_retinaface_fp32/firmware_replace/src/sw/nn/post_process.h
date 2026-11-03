// =============================================================================
// post_process —— 网络输出后处理(按档案的post类型分派)
//
//   NN_POST_CLASSIFY      -> nn_post_classify
//   NN_POST_DETECT_SSD    -> nn_post_detect
//   NN_POST_DETECT_RETINA -> nn_post_retinaface
// =============================================================================
#pragma once

#include "mat.h"

#include "sw/nn/model_profile.h"

//Top-K结果数组上限
#define NN_TOP_K_MAX  5
//单次检测输出框数上限
#define NN_DET_MAX    32
//RetinaFace 阈值后候选上限(NMS 前)
#define NN_RETINA_CAND_MAX  256

//在n个概率里取前k个(降序), idx/val长度>=k; 移植自fpga_ncnn p6_top5
void nn_top_k(const float* p, int n, int k, int* idx, float* val);

//分类后处理: prob为nn_forward_frame的输出
//k: 要的Top-K个数(<=NN_TOP_K_MAX); 结果写入idx/val
//返回0成功; -1输出为空
int nn_post_classify(const ncnn::Mat& prob, int k, int idx[], float val[]);

//检测框(像素坐标)
typedef struct {
    int   label;   //类别号(与labels表对齐)
    float score;   //置信度0~1
    int   x0, y0, x1, y1;
} NnDetBox;

//SSD检测后处理: det为nn_forward_frame的输出, 每行[label,score,x0,y0,x1,y1]
//(label从1起, 坐标0~1归一化); 按档案过滤置信度+跨类别NMS, 坐标放大到像素
//boxes/max_boxes: 输出缓冲; *out_n: 实际框数
//返回0成功; -1输出为空
int nn_post_detect(const ncnn::Mat& det, int W, int H, const NnModelProfile* prof,
                   NnDetBox boxes[], int max_boxes, int* out_n);

//RetinaFace: 对齐 ncnn/examples/retinaface.cpp (variance=1, 无 0.1/0.2)
//坐标先在 320 输入像素, 再按 letterbox 映回摄像头 W×H
int nn_post_retinaface(const ncnn::Mat cls[3], const ncnn::Mat bbox[3],
                       const ncnn::Mat lmk[3], const NnLetterbox* geom,
                       const NnModelProfile* prof,
                       NnDetBox boxes[], int max_boxes, int* out_n);
