// =============================================================================
// nn_runner —— 推理引擎实现
//
// 要点(继承自fpga_ncnn工程约定, 见其docs/REPRODUCE.md):
//  - libncnn.a: 单线程/无异常/无RTTI/NCNN_SIMPLESTL => 不得使用真STL,
//    网络对象放命名空间作用域避免__cxa_guard
//  - 内存加载版load_param/load_model返回size_t(消耗字节数), ==0才是失败
//  - 权重在load_model时被各层拷走, 源缓冲可在切换模型时释放
//  - fp16_storage关闭: int8反量化后的blob不再做fp16转换(与量化对拍一致)
//
// 预处理与解码都不在这里——那是模块(NnModelDef两钩子)的事; 引擎只做②前向:
// 模块产出的输入张量按外部内存零拷贝包成ncnn::Mat, 按模块的输出声明抽取。
// =============================================================================

#include "sw/impl/nn/nn_runner.h"

#include <stddef.h>
#include <stdlib.h>

extern "C" {
#include "xil_printf.h"
#include "xiltimer.h"
}

#include "net.h"

#include "sw/impl/sd/sd.h"
#include "sw/impl/sys/platform.h"

//网络对象(命名空间作用域, 避免函数内static动态初始化需要__cxa_guard)
static ncnn::Net s_net;
static const NnModel* s_cur = 0;
static unsigned char* s_param_mem = 0;   //param.bin内容
static unsigned char* s_model_mem = 0;   //权重内容

//抽取输出常驻位(模块decode在同帧内读; 下次前向整体覆盖; 赋值即释放旧引用)
static ncnn::Mat s_out_mat[NN_OUT_MAX];

// -----------------------------------------------------------------------------
// 平台原语驻留锚 —— 引擎本体已不做预处理, 但模型模块(model.mod)经 fw_exports
// 调用这些ncnn预处理原语; 若固件无人引用, 链接器会把它们从libncnn裁掉,
// fw.map/fw_exports 不再导出, 模块即无法链接。此处显式取地址驻留(外部链接
// 保证发射)。承诺清单新增原语时在此登记(见docs/模型平台化设计.md)。
typedef ncnn::Mat (*NnFromPixelsFn)(const unsigned char*, int, int, int,
                                    ncnn::Allocator*);
typedef ncnn::Mat (*NnFromPixelsResizeFn)(const unsigned char*, int, int, int,
                                          int, int, ncnn::Allocator*);
// volatile: const命名空间对象是内部链接, 无人引用会被整个丢弃;
// 指针本体volatile+extern "C"保证发射
extern "C" void* const volatile nn_primitive_keep[] = {
    (void*)(NnFromPixelsFn)&ncnn::Mat::from_pixels,
    (void*)(NnFromPixelsResizeFn)&ncnn::Mat::from_pixels_resize,
};

static void unload_current(void)
{
    s_net.clear();
    if (s_param_mem) { free(s_param_mem); s_param_mem = 0; }
    if (s_model_mem) { free(s_model_mem); s_model_mem = 0; }
    s_cur = 0;
}

int nn_load(const NnModel* m)
{
    if (m == 0)
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

    s_param_mem = sd_read_file(m->param_file, &sz);
    if (s_param_mem == 0)
        goto fail_files;
    xil_printf("[nn] %s param %u bytes\r\n", m->folder, (unsigned)sz);

    //注意: 内存版load_param返回size_t(消耗字节数), ==0才是失败
    if (s_net.load_param(s_param_mem) == 0)
    {
        xil_printf("[nn] load_param failed\r\n");
        return -3;
    }

    s_model_mem = sd_read_file(m->bin_file, &sz);
    if (s_model_mem == 0)
        goto fail_files;
    xil_printf("[nn] %s weights %u bytes\r\n", m->folder, (unsigned)sz);

    if (s_net.load_model(s_model_mem) == 0)
    {
        xil_printf("[nn] load_model failed\r\n");
        return -4;
    }

    s_cur = m;
    XTime_GetTime(&t1);
    xil_printf("[nn] %s ready in %u ms\r\n", m->folder, time_diff_ms(t0, t1));
    return 0;

fail_files:
    xil_printf("[nn] check SD files:\r\n"
               "  %s\r\n  %s\r\n", m->param_file, m->bin_file);
    return -2;
}

void nn_unload(void)
{
    unload_current();
}

const NnModel* nn_current(void)
{
    return s_cur;
}

//②前向: 模块输出声明 -> 循环抽取; outs[i]是s_out_mat[i]的裸指针视图
//(声明表由模块boot时给出, 运行期只读, 无需加锁——全部调用在app_task)
static int forward(const NnTensor* in, const NnModelDef* d,
                   NnTensor* outs, int* out_n)
{
    //外部内存零拷贝: 模块preprocess产出的CHW浮点块直接作网络输入
    ncnn::Mat in_m(in->w, in->h, in->c, in->data);

    ncnn::Extractor ex = s_net.create_extractor();
    if (ex.input(d->blob_in, in_m) != 0)
        return -2;

    *out_n = 0;
    for (int i = 0; i < d->out_n; i++)
    {
        xil_printf("[nn] extract blob %d\r\n", d->out_blob[i]);   //临时: 崩点定位
        if (ex.extract(d->out_blob[i], s_out_mat[i]) != 0)
            return -3;
        outs[i].data = (float*)s_out_mat[i].data;
        outs[i].w = s_out_mat[i].w;
        outs[i].h = s_out_mat[i].h;
        outs[i].c = s_out_mat[i].c;
        (*out_n)++;
    }
    return 0;
}

int nn_infer(const NnImage* img, NnResult* res)
{
    if (s_cur == 0 || s_cur->def == 0)
        return -1;
    const NnModelDef* d = s_cur->def;

    //临时: 崩点定位打印(首次上板推理曾致全系统挂死, 按最后一条打印归段)
    xil_printf("[nn] stage: preprocess\r\n");
    NnTensor in;
    NnGeom geom;
    if (d->preprocess(img, &in, &geom) != 0)        //①模块预处理
        return -2;

    xil_printf("[nn] stage: forward (blob_in=%d)\r\n", d->blob_in);
    NnTensor outs[NN_OUT_MAX];
    int out_n = 0;
    if (forward(&in, d, outs, &out_n) != 0)         //②前向抽取
        return -3;

    xil_printf("[nn] stage: decode\r\n");
    if (d->decode(outs, out_n, &geom, res) != 0)    //③模块解码(终稿)
        return -4;
    return 0;
}
