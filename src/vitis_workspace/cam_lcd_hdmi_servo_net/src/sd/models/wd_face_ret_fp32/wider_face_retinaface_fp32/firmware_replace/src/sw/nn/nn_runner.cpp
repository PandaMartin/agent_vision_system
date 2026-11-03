// =============================================================================
// nn_runner —— 通用ncnn网络运行器实现
//
// 要点(继承自fpga_ncnn工程约定, 见其docs/REPRODUCE.md):
//  - libncnn.a: 单线程/无异常/无RTTI/NCNN_SIMPLESTL => 不得使用真STL,
//    网络对象放命名空间作用域避免__cxa_guard
//  - 内存加载版load_param/load_model返回size_t(消耗字节数), ==0才是失败
//  - 权重在load_model时被各层拷走, 源缓冲可在切换模型时释放
//  - fp16_storage关闭: int8反量化后的blob不再做fp16转换(与量化对拍一致)
// =============================================================================

#include "sw/nn/nn_runner.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "xil_printf.h"
#include "xiltimer.h"
}

#include "net.h"

#include "sw/storage/sd_io.h"
#include "sw/sys/sys_platform.h"

//网络对象(命名空间作用域, 避免函数内static动态初始化需要__cxa_guard)
static ncnn::Net s_net;
static const NnModelProfile* s_prof = 0;
static unsigned char* s_param_mem = 0;   //param.bin内容
static unsigned char* s_model_mem = 0;   //权重内容

static void unload_current(void)
{
    s_net.clear();
    if (s_param_mem) { free(s_param_mem); s_param_mem = 0; }
    if (s_model_mem) { free(s_model_mem); s_model_mem = 0; }
    s_prof = 0;
}

int nn_load(const NnModelProfile* prof)
{
    if (prof == 0)
        return -1;

    //切换模型: 先完整卸载上一个
    unload_current();

    XTime t0, t1;
    XTime_GetTime(&t0);

    s_net.opt.num_threads = 1;
    s_net.opt.use_packing_layout = true;
    s_net.opt.use_fp16_arithmetic = false;
    s_net.opt.use_fp16_storage = false;
    s_net.opt.use_winograd_convolution = true;
    s_net.opt.use_sgemm_convolution = true;

    unsigned long sz = 0;

    s_param_mem = sd_read_file(prof->param_file, &sz);
    if (s_param_mem == 0)
        goto fail_files;
    xil_printf("[nn] %s param %lu bytes\r\n", prof->name, sz);

    //注意: 内存版load_param返回size_t(消耗字节数), ==0才是失败
    if (s_net.load_param(s_param_mem) == 0)
    {
        xil_printf("[nn] load_param failed\r\n");
        return -3;
    }

    s_model_mem = sd_read_file(prof->bin_file, &sz);
    if (s_model_mem == 0)
        goto fail_files;
    xil_printf("[nn] %s weights %lu bytes\r\n", prof->name, sz);

    if (s_net.load_model(s_model_mem) == 0)
    {
        xil_printf("[nn] load_model failed\r\n");
        return -4;
    }

    s_prof = prof;
    XTime_GetTime(&t1);
    xil_printf("[nn] %s ready in %u ms\r\n", prof->name, time_diff_ms(t0, t1));
    return 0;

fail_files:
    xil_printf("[nn] check SD root files:\r\n"
               "  %s\r\n  %s\r\n", prof->param_file, prof->bin_file);
    return -2;
}

const NnModelProfile* nn_current(void)
{
    return s_prof;
}

static int pixel_type(const NnModelProfile* prof)
{
    if (prof->pixel_bgr2rgb)
        return ncnn::Mat::PIXEL_BGR2RGB;
    return prof->pixel_bgr ? ncnn::Mat::PIXEL_BGR : ncnn::Mat::PIXEL_RGB;
}

int nn_forward_frame(const u8* frame, int W, int H, ncnn::Mat& out)
{
    if (s_prof == 0)
        return -1;

    //预处理: 缩放到档案尺寸 + 减均值/归一化(参数与量化校准一致, 勿改)
    const int S = s_prof->input_size;
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(
        frame, pixel_type(s_prof), W, H, S, S);
    in.substract_mean_normalize(s_prof->mean, s_prof->norm);

    ncnn::Extractor ex = s_net.create_extractor();
    if (ex.input(s_prof->blob_in, in) != 0)
        return -2;
    if (ex.extract(s_prof->blob_out, out) != 0)
        return -3;
    return 0;
}

static void copy_letterbox(const ncnn::Mat& resized, ncnn::Mat& canvas, int pad_x, int pad_y)
{
    const int S = canvas.w;
    const int nw = resized.w;
    const int nh = resized.h;
    canvas.fill(0.f);
    for (int c = 0; c < 3; c++)
    {
        float* dst = canvas.channel(c);
        const float* src = resized.channel(c);
        for (int y = 0; y < nh; y++)
        {
            int dy = pad_y + y;
            if (dy < 0 || dy >= S)
                continue;
            memcpy(dst + dy * S + pad_x, src + y * nw, (size_t)nw * sizeof(float));
        }
    }
}

int nn_forward_retinaface(const u8* frame, int W, int H,
                          ncnn::Mat cls[3], ncnn::Mat bbox[3], ncnn::Mat lmk[3],
                          NnLetterbox* geom)
{
    if (s_prof == 0 || geom == 0)
        return -1;

    const int S = s_prof->input_size;
    float scale = (float)S / (float)W;
    float sy = (float)S / (float)H;
    if (sy < scale)
        scale = sy;
    int nw = (int)(W * scale + 0.5f);
    int nh = (int)(H * scale + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > S) nw = S;
    if (nh > S) nh = S;
    int pad_x = (S - nw) / 2;
    int pad_y = (S - nh) / 2;

    geom->scale = scale;
    geom->pad_x = pad_x;
    geom->pad_y = pad_y;
    geom->src_w = W;
    geom->src_h = H;

    ncnn::Mat resized = ncnn::Mat::from_pixels_resize(
        frame, pixel_type(s_prof), W, H, nw, nh);
    ncnn::Mat in(S, S, 3);
    copy_letterbox(resized, in, pad_x, pad_y);
    in.substract_mean_normalize(s_prof->mean, s_prof->norm);

    ncnn::Extractor ex = s_net.create_extractor();
    if (ex.input(s_prof->blob_in, in) != 0)
        return -2;
    for (int i = 0; i < 3; i++)
    {
        if (ex.extract(s_prof->blob_cls[i], cls[i]) != 0)
            return -3;
        if (ex.extract(s_prof->blob_bbox[i], bbox[i]) != 0)
            return -4;
        if (s_prof->blob_lmk[i] >= 0)
        {
            if (ex.extract(s_prof->blob_lmk[i], lmk[i]) != 0)
                return -5;
        }
    }
    return 0;
}
