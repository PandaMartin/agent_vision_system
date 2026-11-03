// =============================================================================
// wd_face_ret_fp32 —— 模型程序模块(model.mod)
//
// 家族: RetinaFace锚框检测(自训WIDER FACE fp32, 输入320x320, 网内无DetectionOutput)
// 预处理: letterbox等比缩放居中入320窗(余边填0) + BGR->RGB + mean=0/norm=1
//         (原值0~255; 训练侧即此配置, 非Caffe的104/117/123, 亦非SSD的127.5)
// 输出:   6块 = 3尺度x{cls,bbox}(blob号来自ncnn2mem *_id.h; lmk头不抽不画):
//   cls 134/137/140 (stride 8/16/32): c=4, Softmax后reshape,
//        ch0/1=background, ch(q+2)=第q锚的face概率
//   bbox 124/127/130: c=8, 每锚4通道 dx,dy,dw,dh
// 解码:   对齐ncnn/examples/retinaface.cpp: 方形锚=base16x{2,1}/{8,4}/{32,16}
//         (stride 8/16/32 -> 边长32,16/128,64/512,256), 锚中心=8+x*stride,
//         variance=1(无0.1/0.2): pb_cx=cx+aw*dx, pb_w=aw*exp(dw) -> 320输入像素
// 终稿:   score过滤 -> letterbox反映射回原图并clip -> 贪心NMS -> NnResult
//
// 本模块从零实现 nn_pipeline.h 契约(preprocess/decode两函数); 全部模型常数
// 在此自持(无cfg)。无STL/无全局动态初始化/无直接malloc; ncnn::Mat局部对象
// 仅在preprocess内闭环(Thumb位修复后原语路径恢复, 缩放用NEON, 用完即release)
// =============================================================================

#include "sw/impl/nn/nn_pipeline.h"

#include <string.h>

#include "mat.h"      //ncnn::Mat NEON原语(from_pixels_resize等, fw_exports导出)

//固件导出符号(fw_exports.ld链接期解析); xil_printf头无extern "C"保护,
//模块是C++翻译单元, 须自带C链接声明
extern "C" {
int  xil_printf(const char*, ...);
float expf(float);                  //libm真身, 平台导出的纯数学原语
}

namespace {

// ---- 模型常数(该模型包的私有知识; 换模型=写新模块改这里) ----
const int   IN_SIZE    = 320;        //网络输入边长
const int   BLOB_IN    = 0;          //输入blob
const float SCORE_TH   = 0.50f;      //置信度门槛(官方demo 0.8, 现场漏检先降0.3)
const float NMS_IOU    = 0.40f;      //NMS抑制阈值
const int   CAND_MAX   = 256;        //候选框上限(超限换最低分)
const int   KEEP_MAX   = 32;         //保留上限(<=NN_BOX_MAX)
const int   LABEL_FACE = 1;          //labels.txt行号: 0=background, 1=face
const int   ANCHOR_N   = 2;          //每位置锚数

//三尺度输出头(输出声明序: cls,bbox交替; decode按下标2s/2s+1取)
const int   S_BLOB_CLS[3]  = { 134, 137, 140 };    //face_rpn_cls_prob_reshape_*
const int   S_BLOB_BBOX[3] = { 124, 127, 130 };    //face_rpn_bbox_pred_*
const int   S_STRIDE[3]    = { 8, 16, 32 };
const float S_EDGE[3][2]   = { { 32.f, 16.f },     //stride 8:  base16x{2,1}
                               { 128.f, 64.f },    //stride 16: base16x{8,4}
                               { 512.f, 256.f } }; //stride 32: base16x{32,16}
const float ANCHOR_BASE_C  = 8.f;    //base16锚的中心(=base_size/2), 逐格平移stride

// ---- 工作区(模块bss, 加载器已清零) ----
float     s_in[IN_SIZE * IN_SIZE * 3];   //网络输入(R/G/B平面CHW, 0~255原值)
NnDetBox  s_cand[CAND_MAX];              //候选框(score<0表示已处理/抑制)
NnDetBox  s_keep[KEEP_MAX];              //NMS保留框

inline int px_clip(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline void cand_add(NnDetBox* arr, int* n, const NnDetBox& b)
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
// ① 预处理: 任意尺寸BGR/RGB输入 -> ncnn NEON原语等比缩放 -> letterbox 320
// 缩放用固件导出的 from_pixels_resize(2026-10-24 Thumb位修复后原语路径恢复);
// letterbox落位在模块bss(余边填0), Mat生命周期函数内闭环。mean=0/norm=1
// (原值0~255; 训练侧即此配置, 非Caffe的104/117/123, 亦非SSD的127.5)
// =============================================================================
int preprocess(const NnImage* img, NnTensor* in, NnGeom* geom)
{
    if (img == 0 || img->pix == 0 || img->w <= 0 || img->h <= 0)
        return -1;

    xil_printf("[mod] pp: enter %dx%d fmt=%d\r\n",
               img->w, img->h, (int)img->fmt);

    const int sw = img->w;
    const int sh = img->h;

    //letterbox几何: scale=内接等比, 窗口居中
    float scale = (float)IN_SIZE / sw;
    if ((float)IN_SIZE / sh < scale)
        scale = (float)IN_SIZE / sh;
    int nw = (int)(sw * scale + 0.5f);
    int nh = (int)(sh * scale + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > IN_SIZE) nw = IN_SIZE;
    if (nh > IN_SIZE) nh = IN_SIZE;
    const int pad_x = (IN_SIZE - nw) / 2;
    const int pad_y = (IN_SIZE - nh) / 2;

    //网络吃RGB平面序: 内存BGR(相机帧/BMP)转序, 内存RGB直取
    const int ptype = (img->fmt == NN_PIX_BGR) ? ncnn::Mat::PIXEL_BGR2RGB
                                               : ncnn::Mat::PIXEL_RGB;

    //NEON等比缩放到nw x nh, 行级落位到bss的320x320 letterbox(余边0)
    ncnn::Mat rs = ncnn::Mat::from_pixels_resize(img->pix, ptype,
                                                 sw, sh, nw, nh);
    memset(s_in, 0, sizeof(s_in));           //余边=0(mean=0下即中性填充)
    for (int c = 0; c < 3; c++)
    {
        const float* sp = (const float*)rs.data + (size_t)c * rs.cstep;
        float* dp = s_in + (size_t)c * IN_SIZE * IN_SIZE
                  + (size_t)pad_y * IN_SIZE + pad_x;
        for (int y = 0; y < nh; y++, sp += nw, dp += IN_SIZE)
            memcpy(dp, sp, (size_t)nw * sizeof(float));
    }
    rs.release();

    in->data = s_in;
    in->w = IN_SIZE;
    in->h = IN_SIZE;
    in->c = 3;

    geom->scale = scale;
    geom->pad_x = pad_x;
    geom->pad_y = pad_y;
    geom->src_w = sw;
    geom->src_h = sh;
    xil_printf("[mod] pp: done win %dx%d pad(%d,%d)\r\n",
               nw, nh, pad_x, pad_y);
    return 0;
}

// =============================================================================
// ③ 解码: 3尺度锚框回归 -> 置信度过滤/映回原图 -> NMS -> NnResult终稿
// =============================================================================
int decode(const NnTensor* outs, int out_n, const NnGeom* geom, NnResult* res)
{
    res->box_n = 0;
    res->topk_n = 0;
    if (out_n < 6)
        return -1;

    int nb = 0;
    for (int s = 0; s < 3; s++)
    {
        const NnTensor* cls = &outs[2 * s];        //声明序: cls,bbox交替
        const NnTensor* bbx = &outs[2 * s + 1];
        if (cls->data == 0 || bbx->data == 0 || cls->w <= 0 || cls->h <= 0)
            return -2;
        //通道布局防御: cls=2锚x2类, bbox=2锚x4偏移; 两头同尺度
        if (cls->c < 2 * ANCHOR_N || bbx->c < 4 * ANCHOR_N)
            return -3;

        const int    w  = cls->w;
        const int    h  = cls->h;
        const size_t wh = (size_t)w * h;
        const float  stride = (float)S_STRIDE[s];

        for (int q = 0; q < ANCHOR_N; q++)
        {
            const float* prob = cls->data + (size_t)(q + ANCHOR_N) * wh;
            const float* p_dx = bbx->data + (size_t)(q * 4 + 0) * wh;
            const float* p_dy = bbx->data + (size_t)(q * 4 + 1) * wh;
            const float* p_dw = bbx->data + (size_t)(q * 4 + 2) * wh;
            const float* p_dh = bbx->data + (size_t)(q * 4 + 3) * wh;
            const float  edge = S_EDGE[s][q];

            for (int y = 0; y < h; y++)
            {
                const float acy = ANCHOR_BASE_C + (float)y * stride;
                for (int x = 0; x < w; x++)
                {
                    const int   i     = y * w + x;
                    const float score = prob[i];
                    if (score < SCORE_TH)
                        continue;

                    //锚框回归(官方example公式, variance=1无缩放因子)
                    const float acx = ANCHOR_BASE_C + (float)x * stride;
                    const float pcx = acx + edge * p_dx[i];
                    const float pcy = acy + edge * p_dy[i];
                    const float pw  = edge * expf(p_dw[i]);
                    const float ph  = edge * expf(p_dh[i]);

                    //320输入像素 -> 原图像素(letterbox反映射), clip
                    NnDetBox b;
                    b.label = LABEL_FACE;
                    b.score = score;
                    b.x0 = px_clip((int)((pcx - pw * 0.5f - geom->pad_x)
                                         / geom->scale + 0.5f),
                                   0, geom->src_w - 1);
                    b.y0 = px_clip((int)((pcy - ph * 0.5f - geom->pad_y)
                                         / geom->scale + 0.5f),
                                   0, geom->src_h - 1);
                    b.x1 = px_clip((int)((pcx + pw * 0.5f - geom->pad_x)
                                         / geom->scale + 0.5f),
                                   0, geom->src_w - 1);
                    b.y1 = px_clip((int)((pcy + ph * 0.5f - geom->pad_y)
                                         / geom->scale + 0.5f),
                                   0, geom->src_h - 1);
                    if (b.x1 - b.x0 < 4 || b.y1 - b.y0 < 4)  //退化/窗外框
                        continue;
                    cand_add(s_cand, &nb, b);
                }
            }
        }
    }

    xil_printf("[mod] decode: %d cands -> nms\r\n", nb);

    //贪心NMS: 反复取最高分保留, 抑制与其IoU超阈的剩余框
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
        for (int j = 0; j < nb; j++)
        {
            if (s_cand[j].score < 0.f)
                continue;
            if (box_iou(*k, s_cand[j]) > NMS_IOU)
                s_cand[j].score = -1.f;
        }
        nk++;
    }

    //填终稿(box坐标已是NnImage坐标系像素)
    for (int i = 0; i < nk; i++)
        res->box[i] = s_keep[i];
    res->box_n = nk;
    return 0;
}

//模型定义(POD常量, 无动态初始化)
const NnModelDef g_def = {
    NN_PIPELINE_ABI_VER,
    "wd_face_ret_fp32",
    "检测",                                   //tag: 界面原样显示
    BLOB_IN,
    6, { S_BLOB_CLS[0], S_BLOB_BBOX[0],
         S_BLOB_CLS[1], S_BLOB_BBOX[1],
         S_BLOB_CLS[2], S_BLOB_BBOX[2] },
    { NN_ROLE_CLS, NN_ROLE_BBX,
      NN_ROLE_CLS, NN_ROLE_BBX,
      NN_ROLE_CLS, NN_ROLE_BBX },
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
