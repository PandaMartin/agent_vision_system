// =============================================================================
// nn_pipeline —— 推理平台契约(固件与SD模型模块的唯一接口头, 按ABI版本冻结)
//
// 固件是通用推理运行时(装载/调度/ncnn前向/发布), 模型的全部私有知识——预处理、
// 输出清单、解码、NMS、阈值——都在SD卡模型包的 model.mod 程序模块里。
// 本头是两边唯一见面处: 改动任何签名/结构 = 升 NN_PIPELINE_ABI_VER =
// 所有在野模块失配拒载。详细设计与纪律见 src/docs/模型平台化设计.md。
//
// 模块侧约定(与 tools/sdapp 的SD应用同构):
//   链接地址 = 加载地址(MEM_MODEL_MOD_BASE), 零重定位;
//   .mod_entry_first 节的 nn_mod_boot() 恒在镜像偏移0, 返回 NnModelDef*;
//   函数表里的两个钩子必须齐全, 无固件缺省实现。
// =============================================================================
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//契约版本: 固件与模块必须一致, 不一致拒载
#define NN_PIPELINE_ABI_VER  1u

//单帧输出声明上限(锚框检测 3尺度x3头=9 已顶格; 更多的头再议契约升级)
#define NN_OUT_MAX   9
//结果单容量(终稿; 检测框上限 / Top-K上限)
#define NN_BOX_MAX   32
#define NN_TOPK_MAX  5

//---- 原始图像通道序(调用方必须明确声明, 禁止"恰好一致"式假设) ----
typedef enum {
    NN_PIX_RGB = 0,         //内存字节序 R,G,B
    NN_PIX_BGR = 1,         //内存字节序 B,G,R(摄像头帧与BMP照片均此序)
} NnPixFmt;

//---- 输出角色标注(模块声明每块输出是什么, 固件原样带给解码钩子) ----
typedef enum {
    NN_ROLE_PROB = 0,       //分类概率向量(单输出)
    NN_ROLE_DET,            //网内 DetectionOutput 的 N×6 行
    NN_ROLE_CLS,            //锚框检测: 某尺度分类头
    NN_ROLE_BBX,            //锚框检测: 某尺度框回归头
    NN_ROLE_LMK,            //锚框检测: 某尺度关键点头(可无)
} NnRole;

//---- 阶段间流动的数据(全部纯C, 刻意不含ncnn类型) ----

//张量: 裸浮点 + 形状(CHW)。preprocess 的输出缓冲由模块自备(建议模块bss,
//加载器已清零); 前向以外部内存零拷贝包成ncnn::Mat, 只读
typedef struct {
    float* data;
    int    w, h, c;
} NnTensor;

//原始图像: 输入源自描述(摄像头帧/照片缓冲的尺寸与字节序随调用传入)
typedef struct {
    const unsigned char* pix;
    int      w, h;
    NnPixFmt fmt;
} NnImage;

//letterbox几何: preprocess产出, 供decode把网络坐标映回原图;
//拉伸(stretch)模式下为恒等(scale=1, pad=0)
typedef struct {
    float scale;            //网络输入空间 / 原图
    int   pad_x, pad_y;     //letterbox 左/上补边(网络输入像素)
    int   src_w, src_h;     //原图尺寸
} NnGeom;

//检测框(像素坐标, NnImage坐标系; label与labels.txt行号对齐, 0=background)
typedef struct {
    int   label;
    float score;            //0~1
    int   x0, y0, x1, y1;
} NnDetBox;

//统一结果单(解码钩子填终稿, 固件原样发布, 不做任何再加工):
//  检测型填 box(框像素坐标), 分类型填 topk; 发布形态由内容驱动(box_n/topk_n)
typedef struct {
    int box_n;
    NnDetBox box[NN_BOX_MAX];
    int   topk_n;
    int   topk_label[NN_TOPK_MAX];
    float topk_score[NN_TOPK_MAX];
} NnResult;

//---- 模块交给固件的模型定义(nn_mod_boot 返回它) ----
typedef struct {
    uint32_t abi_ver;       //必须 = NN_PIPELINE_ABI_VER

    const char* name;       //模块名(串口日志)
    const char* tag;        //类别标签, 界面原样显示("分类"/"检测"/"分割"…):
                            //模型类型由模块自述, 固件不解释不枚举;
                            //结果卡形态由结果内容驱动(box_n/topk_n), 不依赖此字段

    //输出声明: 模块声明, 固件前向照此执行(抽到的块按序带回, role原样附上)
    int     blob_in;                        //输入blob id(ncnn2mem *_id.h)
    uint8_t out_n;                          //有效输出数(<=NN_OUT_MAX)
    int     out_blob[NN_OUT_MAX];           //输出blob id
    NnRole  out_role[NN_OUT_MAX];           //该块是什么头

    //两个固定函数(必备, 返回0成功, 负数=本帧失败, 固件统一按失败处理)
    //  preprocess: 图像 -> 网络输入张量(+letterbox几何); 通道序/缩放/均值等
    //              全部在本函数内实现, 固件不解释任何预处理参数
    int (*preprocess)(const NnImage* img, NnTensor* in, NnGeom* geom);
    //  decode: 网络输出 -> 结果终稿; 置信度过滤/NMS/坐标裁剪都在本函数内完成
    int (*decode)(const NnTensor* outs, int out_n, const NnGeom* geom,
                  NnResult* res);
} NnModelDef;

//---- 模块入口(镜像偏移0处, 放 .mod_entry_first 节) ----
//const NnModelDef* nn_mod_boot(void);

#ifdef __cplusplus
}
#endif
