// =============================================================================
// post_process —— 网络输出后处理实现
//
// SSD解码/NMS逻辑移植自本项目旧main.cpp(fp32检测版, p6_real板上实测通过)
// RetinaFace 对齐 ncnn/examples/retinaface.cpp (variance=1)
// =============================================================================

#include "sw/nn/post_process.h"

#include <math.h>

void nn_top_k(const float* p, int n, int k, int* idx, float* val)
{
    for (int j = 0; j < k; j++)
    {
        idx[j] = -1;
        val[j] = -1e30f;
    }
    for (int i = 0; i < n; i++)
    {
        float v = p[i];
        int pos = k;
        for (int j = 0; j < k; j++)
        {
            if (v > val[j])
            {
                pos = j;
                break;
            }
        }
        if (pos < k)
        {
            for (int j = k - 1; j > pos; j--)
            {
                val[j] = val[j - 1];
                idx[j] = idx[j - 1];
            }
            val[pos] = v;
            idx[pos] = i;
        }
    }
}

int nn_post_classify(const ncnn::Mat& prob, int k, int idx[], float val[])
{
    if (prob.empty())
        return -1;
    nn_top_k((const float*)prob.data, (int)prob.total(), k, idx, val);
    return 0;
}

//两个像素框的IoU(0~1)
static float box_iou(int ax0, int ay0, int ax1, int ay1,
                     int bx0, int by0, int bx1, int by1)
{
    int ix0 = ax0 > bx0 ? ax0 : bx0;
    int iy0 = ay0 > by0 ? ay0 : by0;
    int ix1 = ax1 < bx1 ? ax1 : bx1;
    int iy1 = ay1 < by1 ? ay1 : by1;
    int iw = ix1 - ix0;
    int ih = iy1 - iy0;
    if (iw <= 0 || ih <= 0)
        return 0.0f;
    int inter = iw * ih;
    int area1 = (ax1 - ax0) * (ay1 - ay0);
    int area2 = (bx1 - bx0) * (by1 - by0);
    return (float)inter / (float)(area1 + area2 - inter);
}

int nn_post_detect(const ncnn::Mat& det, int W, int H, const NnModelProfile* prof,
                   NnDetBox boxes[], int max_boxes, int* out_n)
{
    *out_n = 0;
    if (det.empty())
        return -1;

    //---- 1.收集达标框(类别合法+置信度足够+非退化), 坐标放大到像素 ----
    NnDetBox cand[NN_DET_MAX];
    int nb = 0;
    const int n = (int)det.h;
    for (int i = 0; i < n && nb < NN_DET_MAX; i++)
    {
        const float* row = det.row(i);
        int   lab   = (int)(row[0] + 0.5f);
        float score = row[1];
        if (lab <= 0 || lab >= prof->num_classes || score < prof->score_th)
            continue;
        NnDetBox b;
        b.label = lab;
        b.score = score;
        b.x0 = (int)(row[2] * (float)W);
        b.y0 = (int)(row[3] * (float)H);
        b.x1 = (int)(row[4] * (float)W);
        b.y1 = (int)(row[5] * (float)H);
        if (b.x1 - b.x0 < 4 || b.y1 - b.y0 < 4)   //跳过退化框
            continue;
        cand[nb++] = b;
    }

    //---- 2.贪心NMS(跨类别抑制, 处理过的框用score=-1标记) ----
    const int keep = prof->max_keep < max_boxes ? prof->max_keep : max_boxes;
    while (*out_n < keep)
    {
        //挑剩余中得分最高的
        int best = -1;
        for (int j = 0; j < nb; j++)
        {
            if (cand[j].score < 0.0f)
                continue;
            if (best < 0 || cand[j].score > cand[best].score)
                best = j;
        }
        if (best < 0)
            break;
        boxes[(*out_n)++] = cand[best];
        cand[best].score = -1.0f;

        //抑制与保留框重叠过大的剩余框
        for (int j = 0; j < nb; j++)
        {
            if (cand[j].score < 0.0f)
                continue;
            const NnDetBox* k = &boxes[*out_n - 1];
            if (box_iou(k->x0, k->y0, k->x1, k->y1,
                        cand[j].x0, cand[j].y0, cand[j].x1, cand[j].y1) > prof->nms_iou_th)
                cand[j].score = -1.0f;
        }
    }
    return 0;
}

//官方 example: ratio=1, base_size=16, 每尺度 2 个 scale
static const float k_retina_scales[3][2] = {
    {2.f, 1.f},     // stride 8
    {8.f, 4.f},     // stride 16
    {32.f, 16.f}    // stride 32
};

static void push_cand(NnDetBox cand[], int* nb, NnDetBox b)
{
    if (*nb < NN_RETINA_CAND_MAX)
    {
        cand[(*nb)++] = b;
        return;
    }
    int worst = 0;
    for (int i = 1; i < NN_RETINA_CAND_MAX; i++)
    {
        if (cand[i].score < cand[worst].score)
            worst = i;
    }
    if (b.score > cand[worst].score)
        cand[worst] = b;
}

static int clip_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int nn_post_retinaface(const ncnn::Mat cls[3], const ncnn::Mat bbox[3],
                       const ncnn::Mat lmk[3], const NnLetterbox* geom,
                       const NnModelProfile* prof,
                       NnDetBox boxes[], int max_boxes, int* out_n)
{
    (void)lmk;
    *out_n = 0;
    if (prof == 0 || geom == 0)
        return -1;

    const float thresh = prof->score_th;
    const float inv = 1.0f / (geom->scale > 1e-6f ? geom->scale : 1.f);
    const int cam_w = geom->src_w;
    const int cam_h = geom->src_h;
    const int num_anchors = 2;
    const float base = 16.f;

    NnDetBox cand[NN_RETINA_CAND_MAX];
    int nb = 0;

    for (int s = 0; s < 3; s++)
    {
        if (cls[s].empty() || bbox[s].empty())
            continue;
        const int feat_w = cls[s].w;
        const int feat_h = cls[s].h;
        const int stride = prof->retina_strides[s] ? (int)prof->retina_strides[s] : (8 << s);

        for (int q = 0; q < num_anchors; q++)
        {
            const float rs = base * k_retina_scales[s][q];
            const float ax0 = base * 0.5f - rs * 0.5f;
            const float ay0 = base * 0.5f - rs * 0.5f;
            const ncnn::Mat score = cls[s].channel(q + num_anchors);
            const ncnn::Mat dx = bbox[s].channel(q * 4 + 0);
            const ncnn::Mat dy = bbox[s].channel(q * 4 + 1);
            const ncnn::Mat dw = bbox[s].channel(q * 4 + 2);
            const ncnn::Mat dh = bbox[s].channel(q * 4 + 3);

            for (int i = 0; i < feat_h; i++)
            {
                for (int j = 0; j < feat_w; j++)
                {
                    const int index = i * feat_w + j;
                    const float prob = score[index];
                    if (prob < thresh)
                        continue;

                    float ddw = dw[index];
                    float ddh = dh[index];
                    if (ddw > 8.f) ddw = 8.f;
                    if (ddw < -8.f) ddw = -8.f;
                    if (ddh > 8.f) ddh = 8.f;
                    if (ddh < -8.f) ddh = -8.f;

                    const float anchor_x = ax0 + (float)j * (float)stride;
                    const float anchor_y = ay0 + (float)i * (float)stride;
                    const float cx = anchor_x + rs * 0.5f;
                    const float cy = anchor_y + rs * 0.5f;
                    const float pb_cx = cx + rs * dx[index];
                    const float pb_cy = cy + rs * dy[index];
                    const float pb_w = rs * expf(ddw);
                    const float pb_h = rs * expf(ddh);
                    float x0 = pb_cx - pb_w * 0.5f;
                    float y0 = pb_cy - pb_h * 0.5f;
                    float x1 = pb_cx + pb_w * 0.5f;
                    float y1 = pb_cy + pb_h * 0.5f;

                    x0 = (x0 - (float)geom->pad_x) * inv;
                    y0 = (y0 - (float)geom->pad_y) * inv;
                    x1 = (x1 - (float)geom->pad_x) * inv;
                    y1 = (y1 - (float)geom->pad_y) * inv;
                    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
                    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }

                    NnDetBox b;
                    b.label = 1;
                    b.score = prob;
                    b.x0 = clip_i((int)x0, 0, cam_w - 1);
                    b.y0 = clip_i((int)y0, 0, cam_h - 1);
                    b.x1 = clip_i((int)x1, 0, cam_w - 1);
                    b.y1 = clip_i((int)y1, 0, cam_h - 1);
                    if (b.x1 - b.x0 < 4 || b.y1 - b.y0 < 4)
                        continue;
                    push_cand(cand, &nb, b);
                }
            }
        }
    }

    const int keep = prof->max_keep < max_boxes ? prof->max_keep : max_boxes;
    while (*out_n < keep)
    {
        int best = -1;
        for (int j = 0; j < nb; j++)
        {
            if (cand[j].score < 0.0f)
                continue;
            if (best < 0 || cand[j].score > cand[best].score)
                best = j;
        }
        if (best < 0)
            break;
        boxes[(*out_n)++] = cand[best];
        cand[best].score = -1.0f;
        for (int j = 0; j < nb; j++)
        {
            if (cand[j].score < 0.0f)
                continue;
            const NnDetBox* k = &boxes[*out_n - 1];
            if (box_iou(k->x0, k->y0, k->x1, k->y1,
                        cand[j].x0, cand[j].y0, cand[j].x1, cand[j].y1) > prof->nms_iou_th)
                cand[j].score = -1.0f;
        }
    }
    return 0;
}
