// =============================================================================
// nn_selftest —— 神经网络平台链路开机自检(实现, 见 nn_selftest.h)
//
// 全部跑在app_task上下文(FatFs纪律, 开机扫描之后、命令循环之前)。
// 无STL(ncnn SIMPLESTL约定), 资源用完即free。
// =============================================================================

extern "C" {
#include "xil_types.h"
#include "xil_printf.h"
#include "xiltimer.h"       //XTime计时(与nn_runner同源)
}

#include <stdlib.h>

#include "sw/impl/nn/nn_selftest.h"
#include "sw/impl/nn/model_mgr.h"
#include "sw/impl/nn/nn_runner.h"
#include "sw/impl/nn/nn_wdt.h"
#include "sw/impl/sd/sd.h"
#include "sw/impl/sd/photo.h"
#include "sw/impl/sys/platform.h"

#define SELFTEST_IMG  "0:/nn_selftest.bmp"
#define RUNS          3     //每模型连跑次数(取稳定耗时)

// -----------------------------------------------------------------------------
//结果终稿打印(检测框/Top-K, 类别名经labels.txt翻译; 越界/超表打"?")
static void print_result(const NnModel* m, const NnResult* res)
{
    for (int i = 0; i < res->box_n; i++)
    {
        const NnDetBox* b = &res->box[i];
        const char* name = (b->label >= 0 && b->label < m->num_classes
                            && m->labels != 0) ? m->labels[b->label] : "?";
        xil_printf("  box[%d] %s %u%% (%d,%d)-(%d,%d)\r\n",
                   i, name, (unsigned)(b->score * 100.0f + 0.5f),
                   b->x0, b->y0, b->x1, b->y1);
    }
    for (int i = 0; i < res->topk_n; i++)
    {
        const char* name = (res->topk_label[i] >= 0
                            && res->topk_label[i] < m->num_classes
                            && m->labels != 0)
                           ? m->labels[res->topk_label[i]] : "?";
        xil_printf("  top%d %s %u%%\r\n",
                   i + 1, name,
                   (unsigned)(res->topk_score[i] * 100.0f + 0.5f));
    }
    if (res->box_n == 0 && res->topk_n == 0)
        xil_printf("  (空结果: decode通过但无过阈输出)\r\n");
}

// -----------------------------------------------------------------------------
//定点读小端u32(与photo.cpp的get32同义; probe失败时诊断打印用)
static u32 sf_u32(const unsigned char* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16)
           | ((u32)p[3] << 24);
}

//probe失败时打印BMP头部关键域, 避免盲改图再烧一轮(被拒原因无非:
//非BM魔数/bpp!=24/压缩!=0/信息头<40/尺寸超限/像素偏移越界)
static void sf_dump_bmp_head(const unsigned char* f, unsigned long len)
{
    if (len < 54)
    {
        xil_printf("  len=%u (<54, 不是完整BMP头)\r\n", (unsigned)len);
        return;
    }
    xil_printf("  magic=%c%c len=%u hdr=%u %ux%u bpp=%u comp=%u off=%u\r\n",
               f[0], f[1], (unsigned)len, (unsigned)sf_u32(f + 14),
               (unsigned)sf_u32(f + 18), (unsigned)sf_u32(f + 22),
               (unsigned)((u32)f[28] | ((u32)f[29] << 8)),
               (unsigned)sf_u32(f + 30), (unsigned)sf_u32(f + 10));
    xil_printf("  要求: BM/24bpp/comp=0/尺寸1~4096/off在文件内, 宽为4的倍数\r\n");
}

// -----------------------------------------------------------------------------
//自检看门狗会话(构造上狗/析构关狗, 覆盖全部退出路径)
struct SelftestWdt
{
    SelftestWdt() { nn_wdt_arm(); }
    ~SelftestWdt() { nn_wdt_disarm(); }
};

// -----------------------------------------------------------------------------
void nn_selftest_run(void)
{
    SelftestWdt wdt;                        //推理全程上狗: 模块/ncnn挂死→硬件复位
    unsigned long len = 0;
    unsigned char* raw = sd_read_file(SELFTEST_IMG, &len);
    if (raw == 0)
    {
        xil_printf("[nn-selftest] 无 %s, 跳过自检(放一张24位BMP即测)\r\n",
                   SELFTEST_IMG);
        return;
    }

    u32 w = 0, h = 0, off = 0;             //预填防probe失败后打印未初始化
    if (photo_probe_bmp(raw, (u32)len, &w, &h, &off) != 0)
    {
        xil_printf("[nn-selftest] 测试图被拒, BMP头部实况:\r\n");
        sf_dump_bmp_head(raw, len);
        free(raw);
        return;
    }
    //photo_flip_rows按w*3紧密行距翻转, BMP行4字节对齐 => 宽须4的倍数
    if (w == 0 || h == 0 || (w % 4) != 0)
    {
        xil_printf("[nn-selftest] 测试图被拒: 宽%u须为4的倍数(高%u)\r\n",
                   (unsigned)w, (unsigned)h);
        free(raw);
        return;
    }

    photo_flip_rows(raw + off, w, h);      //BMP底行在先 -> 帧序顶行在先
    const NnImage img = { raw + off, (int)w, (int)h, NN_PIX_BGR };
    xil_printf("[nn-selftest] 测试图 %ux%u, %d 个模型\r\n",
               (unsigned)w, (unsigned)h, nn_models_count());

    for (int i = 0; i < nn_models_count(); i++)
    {
        xil_printf("[nn-selftest] === [%d] %s (%s) ===\r\n",
                   i, nn_model_name(i), nn_model_tag(i));
        if (nn_model_activate(i) != 0)     //失败原因已由各层串口打印
        {
            xil_printf("[nn-selftest] 激活失败, 跳过\r\n");
            continue;
        }

        NnResult res;
        int ok_runs = 0;
        for (int r = 0; r < RUNS; r++)
        {
            xil_printf("[nn-selftest] 第%d次推理中(preprocess->前向->decode)...\r\n",
                       r + 1);
            nn_wdt_kick();                  //每次推理前喂狗(单帧挂死≤2拍复位)
            XTime t0, t1;
            XTime_GetTime(&t0);
            const int rc = nn_infer(&img, &res);
            XTime_GetTime(&t1);
            if (rc != 0)
            {
                xil_printf("[nn-selftest] 推理失败 rc=%d"
                           "(-2预处理 -3前向 -4解码)\r\n", rc);
                break;
            }
            ok_runs++;
            xil_printf("[nn-selftest] 第%d次 %u ms\r\n",
                       r + 1, time_diff_ms(t0, t1));
        }
        if (ok_runs > 0)
            print_result(nn_active_model(), &res);
    }

    free(raw);
    xil_printf("[nn-selftest] 自检结束\r\n");
}
