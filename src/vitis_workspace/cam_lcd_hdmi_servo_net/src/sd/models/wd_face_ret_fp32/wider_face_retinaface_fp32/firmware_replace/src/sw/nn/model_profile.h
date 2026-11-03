// =============================================================================
// model_profile —— 模型档案: 一个ncnn模型的全部"私有知识"（数据, 不是代码）
//
// 档案是运行期结构, 由 model_mgr 从 SD卡 /models/<文件夹>/model.cfg 解析填充
// (约定见各 model.cfg 文件头注释)。运行器(nn_runner)和后处理(post_process)
// 只认本结构, 不关心模型是什么 —— 添加新模型=拷一个新文件夹到SD卡, 零代码
// =============================================================================
#pragma once

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

//后处理类型(决定输出怎么解码)
typedef enum {
    NN_POST_CLASSIFY = 0,   //分类: 输出为softmax概率, 取Top-K
    NN_POST_DETECT_SSD,     //检测: 输出为SSD detection_out行, 解码+跨类别NMS
    NN_POST_DETECT_RETINA   //人脸: RetinaFace 三尺度 cls/bbox, 无 DetectionOutput
} NnPostType;

typedef struct {
    char name[32];          //显示名(cfg的name, 缺省=文件夹名)
    char param_file[96];    //param.bin完整路径(激活时填充)
    char bin_file[96];      //权重bin完整路径(激活时填充)
    u16 input_size;         //正方形输入边长(如227/300/320)
    u8  pixel_bgr;          //1=输入帧为BGR字节序, 0=RGB
    u8  pixel_bgr2rgb;      //1=PIXEL_BGR2RGB(摄像头BGR, 网络要RGB)
    int blob_in;            //输入blob编号(来自ncnn2mem生成的 *_id.h)
    int blob_out;           //输出blob编号(classify/SSD)
    float mean[3];          //预处理: 先减均值
    float norm[3];          //预处理: 再乘缩放({1,1,1}=不缩放)
    NnPostType post;        //后处理类型
    int topk;               //分类: 打印/返回的Top-K数
    const char* const* labels;  //类别名表(labels.txt逐行, 检测含background在0号)
    int num_classes;        //类别数(=labels.txt行数)
    //以下 detect / retinaface 共用
    float score_th;         //置信度阈值
    float nms_iou_th;       //NMS的IoU抑制阈值(0~1)
    u16  max_keep;          //最多保留的检测框数
    //以下仅 post=NN_POST_DETECT_RETINA
    int blob_cls[3];        //stride 8/16/32 分类 blob, -1=无
    int blob_bbox[3];
    int blob_lmk[3];        //不画关键点可全 -1
    u8  retina_strides[3];  //建议 {8,16,32}
} NnModelProfile;

//RetinaFace letterbox 几何(前向填, 后处理映回摄像头)
typedef struct {
    float scale;
    int pad_x;
    int pad_y;
    int src_w, src_h;
} NnLetterbox;

#ifdef __cplusplus
}
#endif
