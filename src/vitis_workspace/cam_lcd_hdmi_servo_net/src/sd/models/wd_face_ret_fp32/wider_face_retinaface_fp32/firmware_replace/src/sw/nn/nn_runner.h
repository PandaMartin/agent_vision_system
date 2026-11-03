// =============================================================================
// nn_runner —— 通用ncnn网络运行器(不绑定任何具体模型)
//
// 所有模型差异都来自 NnModelProfile(model_profile.h), 本模块只做:
//   nn_load               按档案从SD加载(重复调用=卸载旧的换新的)
//   nn_current            当前档案(取labels/后处理类型等)
//   nn_forward_frame      一帧 -> 预处理 -> 前向 -> 单个输出blob(classify/SSD)
//   nn_forward_retinaface 一帧 letterbox + 抽 9 个头(RetinaFace)
// =============================================================================
#pragma once

#include "mat.h"

#include "sw/nn/model_profile.h"

//按档案从SD加载模型; 重复调用会先卸载上一个模型再加载新的
//返回0成功; 负数失败(详情已打印)
int nn_load(const NnModelProfile* prof);

//当前已加载的档案(未加载过为NULL)
const NnModelProfile* nn_current(void);

//对一帧图像做完整前向(按档案缩放/减均值/归一化)
//frame: 帧图像(W*H*3, 字节序由档案pixel_bgr决定)
//out  : 原始输出blob, 交给post_process按档案解码
//返回0成功
int nn_forward_frame(const u8* frame, int W, int H, ncnn::Mat& out);

//RetinaFace: 320 letterbox + PIXEL_BGR2RGB, 抽出 cls/bbox/lmk 三个尺度
int nn_forward_retinaface(const u8* frame, int W, int H,
                          ncnn::Mat cls[3], ncnn::Mat bbox[3], ncnn::Mat lmk[3],
                          NnLetterbox* geom);
