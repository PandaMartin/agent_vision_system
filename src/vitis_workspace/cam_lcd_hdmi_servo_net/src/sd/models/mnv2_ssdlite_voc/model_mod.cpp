// =============================================================================
// mobilenetv2_ssdlite_voc_int8 —— 模型程序模块(model.mod)
//
// 家族: SSD(网内DetectionOutput层已出整理好的检测行)
// 预处理: 摄像头帧/BMP照片(BGR) 拉伸缩放到300x300, 减均值127.5乘1/127.5
// 输出:   blob 210 = detection_out, N×6行[label,score,x0,y0,x1,y1](0~1归一化)
// 解码:   置信度过滤 -> 坐标映回原图并clip -> 贪心NMS -> NnResult终稿
//
// 本模块从零实现 nn_pipeline.h 契约(preprocess/decode两函数); 全部模型常数
// 在此自持(无cfg)。无STL/无全局动态初始化(模块加载器不跑init_array)。
// =============================================================================

#include "sw/impl/nn/nn_pipeline.h"

#include <string.h>

#include "mat.h"      //ncnn::Mat NEON原语(from_pixels_resize等, fw_exports导出)

//固件导出符号(fw_exports.ld链接期解析); xil_printf头无extern "C"保护,
//模块是C++翻译单元, 须自带C链接声明
extern "C" int xil_printf(const char*, ...);

namespace {

// ---- 模型常数(该模型包的私有知识; 换模型=写新模块改这里) ----
const int   IN_SIZE    = 300;        //网络输入边长
const int   BLOB_IN    = 0;          //输入blob
const int   BLOB_OUT   = 210;        //detection_out
const float SCORE_TH   = 0.50f;      //置信度门槛
const float NMS_IOU    = 0.45f;      //NMS抑制阈值
const int   CAND_MAX   = 64;         //候选框上限(超限换最低分)
const int   KEEP_MAX   = 32;         //保留上限(<=NN_BOX_MAX)
const float MEAN[3]    = {127.5f, 127.5f, 127.5f};
const float NORM[3]    = {0.00784314f, 0.00784314f, 0.00784314f}; //1/127.5

// ---- 工作区(模块bss, 加载器已清零) ----
float     s_in[IN_SIZE * IN_SIZE * 3];   //网络输入张量(300*300*3)
NnDetBox  s_cand[CAND_MAX];              //候选框(score<0表示已处理/抑制)
NnDetBox  s_keep[KEEP_MAX];              //NMS保留框

inline void cand_push(NnDetBox* arr, int* n, const NnDetBox& b)
{
    if (*n < CAND_MAX)
    {
        arr[(*n)++] = b;
        return;
    }
    int worst = 0;
    for (int i = 1; i < CAND_MAX; i++)
        if (arr[i].score < arr[worst].score)
            worst = i;
    if (b.score > arr[worst].score)
        arr[worst] = b;
}

inline int clip_i(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

//两像素框IoU(0~1)
inline float box_iou(const NnDetBox& a, const NnDetBox& b)
{
    const int ix0 = a.x0 > b.x0 ? a.x0 : b.x0;
    const int iy0 = a.y0 > b.y0 ? a.y0 : b.y0;
    const int ix1 = a.x1 < b.x1 ? a.x1 : b.x1;
    const int iy1 = a.y1 < b.y1 ? a.y1 : b.y1;
    const int iw = ix1 - ix0;
    const int ih = iy1 - iy0;
    if (iw <= 0 || ih <= 0)
        return 0.f;
    const int inter = iw * ih;
    const int area1 = (a.x1 - a.x0) * (a.y1 - a.y0);
    const int area2 = (b.x1 - b.x0) * (b.y1 - b.y0);
    return (float)inter / (float)(area1 + area2 - inter);
}

// =============================================================================
// ① 预处理: 任意尺寸BGR/RGB输入 -> ncnn原语拉伸300x300 -> 减均值/归一化
// (2026-10-24恢复原语路径: 当年"预处理后fastFree挂死"的真正根因是fw_exports
//  丢Thumb位(newlib的free等符号被veneer按ARM态进入), 已在sdapp.py export
//  修复, NEON缩放重新可用。Mat生命周期在函数内闭环: 缩放/归一化后拷入
//  模块bss再release, 前向只读bss, 模块切换无堆残留)
// =============================================================================
int preprocess(const NnImage* img, NnTensor* in, NnGeom* geom)
{
    if (img == 0 || img->pix == 0 || img->w <= 0 || img->h <= 0)
        return -1;

    xil_printf("[mod] pp: enter %dx%d fmt=%d\r\n",
               img->w, img->h, (int)img->fmt);

    //网络要BGR平面序: 内存BGR(BMP照片/相机帧)直取, 内存RGB则转序
    const int ptype = (img->fmt == NN_PIX_BGR) ? ncnn::Mat::PIXEL_BGR
                                               : ncnn::Mat::PIXEL_RGB2BGR;

    ncnn::Mat m = ncnn::Mat::from_pixels_resize(img->pix, ptype,
                                                img->w, img->h,
                                                IN_SIZE, IN_SIZE);
    m.substract_mean_normalize(MEAN, NORM);
    memcpy(s_in, m.data, (size_t)IN_SIZE * IN_SIZE * 3 * sizeof(float));
    m.release();

    in->data = s_in;
    in->w = IN_SIZE;
    in->h = IN_SIZE;
    in->c = 3;

    //拉伸(非letterbox): 无补边, 几何恒等; 框坐标由decode直接乘原图尺寸
    geom->scale = 1.f;
    geom->pad_x = 0;
    geom->pad_y = 0;
    geom->src_w = img->w;
    geom->src_h = img->h;
    xil_printf("[mod] pp: done\r\n");
    return 0;
}

// =============================================================================
// ③ 解码: detection_out行 -> 过滤/映回原图/NMS -> NnResult终稿
// =============================================================================
int decode(const NnTensor* outs, int out_n, const NnGeom* geom, NnResult* res)
{
    (void)out_n;
    res->box_n = 0;
    res->topk_n = 0;
    if (outs[0].data == 0 || outs[0].w < 6)
        return -1;
    xil_printf("[mod] decode: rows=%dx%d\r\n", outs[0].h, outs[0].w);

    //行布局: w=6(每行float数), h=行数, c=1; 行i首址 = data + i*w
    const float* rows   = outs[0].data;
    const int    stride = outs[0].w;
    const int    rows_n = outs[0].h;
    const float  sw = (float)geom->src_w;
    const float  sh = (float)geom->src_h;

    //1.置信度过滤 + 0~1坐标映回原图像素并clip
    int nb = 0;
    for (int r = 0; r < rows_n; r++)
    {
        const float* row = rows + (size_t)r * stride;
        const int   label = (int)(row[0] + 0.5f);
        const float score = row[1];
        if (label <= 0 || score < SCORE_TH)     //0号=background滤除
            continue;

        NnDetBox b;
        b.label = label;
        b.score = score;
        b.x0 = clip_i((int)(row[2] * sw), 0, (int)sw - 1);
        b.y0 = clip_i((int)(row[3] * sh), 0, (int)sh - 1);
        b.x1 = clip_i((int)(row[4] * sw), 0, (int)sw - 1);
        b.y1 = clip_i((int)(row[5] * sh), 0, (int)sh - 1);
        if (b.x1 - b.x0 < 4 || b.y1 - b.y0 < 4) //退化框
            continue;
        cand_push(s_cand, &nb, b);
    }

    //2.贪心NMS: 反复取最高分保留, 抑制与其IoU超阈的剩余框
    int nk = 0;
    while (nk < KEEP_MAX)
    {
        int best = -1;
        for (int j = 0; j < nb; j++)
        {
            if (s_cand[j].score < 0.f)
                continue;
            if (best < 0 || s_cand[j].score > s_cand[best].score)
                best = j;
        }
        if (best < 0)
            break;
        s_keep[nk] = s_cand[best];
        s_cand[best].score = -1.f;              //标记已取

        const NnDetBox* k = &s_keep[nk];
        for (int j = 0; j < nb; j++)            //抑制重叠的剩余框
        {
            if (s_cand[j].score < 0.f)
                continue;
            if (box_iou(*k, s_cand[j]) > NMS_IOU)
                s_cand[j].score = -1.f;
        }
        nk++;
    }

    //3.填终稿(box坐标已是NnImage坐标系像素)
    for (int i = 0; i < nk; i++)
        res->box[i] = s_keep[i];
    res->box_n = nk;
    return 0;
}

//模型定义(POD常量, 无动态初始化)
const NnModelDef g_def = {
    NN_PIPELINE_ABI_VER,
    "mobilenetv2_ssdlite_voc_int8",
    "检测",                                   //tag: 界面原样显示
    BLOB_IN,
    1, { BLOB_OUT }, { NN_ROLE_DET },
    preprocess,
    decode,
};

} // namespace

//镜像入口(.mod_entry_first节, 恒在模块区基址)
extern "C" __attribute__((section(".mod_entry_first")))
const NnModelDef* nn_mod_boot(void)
{
    return &g_def;
}
