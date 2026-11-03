// =============================================================================
// nn_probe —— 写坏者探测模块(临时排查工具, 不做真实推理)
//
// 悬案: 内存被间歇涂写(堆写坏致mallinfo/fastFree冻结; 上一症状为模块区被
// 涂致wd_face在preprocess内冻结)。本模块住进同一个模块区(0x21000000)当
// 诱饵, 把写坏者当场按住:
//   1) 代码段CRC: 装载后代码/只读数据被改任何一个字都立即报告;
//   2) bss金丝雀: 输入缓冲之后的~896KB分带金丝雀(32KB槽距x8KB金丝雀,
//      槽间留白), 报告区偏移->能描出写者的落点规律;
//   3) 首帧武装前先扫"非零残留"(加载器清过零, 非零=装载~首帧间被写过);
//   4) preprocess(前向前)与decode(前向后)各查一遍: 同帧两查夹住前向,
//      写发生在前向期间还是空闲期可分辨。
// 报告纪律: 干净时每25帧一行心跳; 发现篡改立即全量报告(区偏移+期望+现值
// +代数), 已报告的字恢复期望值——同一地址再次触发=写者在重复写。
// 前向复用mnv2权重包(param/bin/labels与本文件夹同拷), 输出声明blob 210,
// decode恒空结果——维持帧级成功让回泵持续采样(~4fps)。
// 安全性: 无堆/无ncnn对象, 只读扫描+打印+写自身bss。
// =============================================================================

#include "sw/impl/nn/nn_pipeline.h"

#include <string.h>

//固件导出符号(fw_exports.ld链接期解析)
extern "C" int xil_printf(const char*, ...);

namespace {

// ---- 模型形态(照抄mnv2, 仅为让前向跑通维持采样) ----
const int IN_SIZE  = 300;
const int BLOB_IN  = 0;
const int BLOB_OUT = 210;

//模块区加载基址(=mod.ld ORIGIN, 链接期保证, 见mem_layout.h)
const unsigned MOD_BASE = 0x21000000u;

// ---- 探测区布局(全在模块bss, 加载器已清零) ----
float s_in[IN_SIZE * IN_SIZE * 3];           //前向输入(内容无意义, 兼作诱饵)

const unsigned CAN_STRIDE_U32 = 32768u / 4u;    //32KB槽距(u32计)
const unsigned CAN_WORDS      = 8192u / 4u;     //每槽头部金丝雀8KB
const unsigned CAN_SLOTS      = 28u;            //28槽=896KB金丝雀/留白相间
unsigned s_zone[CAN_SLOTS * CAN_STRIDE_U32];

// ---- 状态(自身bss, 不属金丝雀; 涂改它们由代码CRC间接暴露) ----
unsigned s_armed;                //金丝雀已武装
unsigned s_gen;                  //帧代数(每次preprocess自增)
unsigned s_hits_total;           //累计被改字数(只增)
unsigned s_crc_ref;              //武装时记录的代码CRC基准

//代码段字节数(构建时二次烤入: 首遍build后取"bss@off+N"的N回填, 留裕量)
const unsigned PROBE_CODE_BYTES = 1588u;    /*BAKE(=实际code字节数, 勿越bss_off)*/

//金丝雀期望值: 只与"距模块基址的u32偏移"有关(报告可换算区偏移)
inline unsigned can_expect(unsigned region_off_u32)
{
    return 0xC0DE0000u ^ (region_off_u32 * 2654435761u);
}

inline unsigned code_crc(void)
{
    const unsigned* p = (const unsigned*)MOD_BASE;
    const unsigned n = PROBE_CODE_BYTES / 4;
    unsigned h = 2166136261u;
    for (unsigned i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

// -----------------------------------------------------------------------------
// 全量检查: 代码CRC + 金丝雀逐字. 返回本次新发现的被改字数.
// 每个被改字: 报告后恢复期望值(同一地址再触发=写者重复写).
static unsigned check_all(const char* stage)
{
    unsigned found = 0;

    const unsigned crc = code_crc();
    if (s_armed && crc != s_crc_ref)
    {
        //代码被改: 系统随时失稳, 此报告本身就是"模块区代码被涂"的铁证
        xil_printf("[probe] !! CODE CORRUPT gen=%u %s: crc %08x != %08x\r\n",
                   s_gen, stage, crc, s_crc_ref);
        found++;
    }

    const unsigned zone_off0 = ((unsigned)((uintptr_t)s_zone - MOD_BASE)) / 4u;
    for (unsigned s = 0; s < CAN_SLOTS; s++)
    {
        unsigned* w = s_zone + (size_t)s * CAN_STRIDE_U32;
        const unsigned base_off = zone_off0 + s * CAN_STRIDE_U32;
        for (unsigned i = 0; i < CAN_WORDS; i++)
        {
            const unsigned exp = can_expect(base_off + i);
            const unsigned got = w[i];
            if (got == exp)
                continue;
            found++;
            s_hits_total++;
            if (found <= 8)         //每次报告至多8条, 防刷屏
                xil_printf("[probe] !! hit gen=%u %s off=+0x%x "
                           "exp=%08x got=%08x\r\n",
                           s_gen, stage,
                           (unsigned)((uintptr_t)(w + i) - MOD_BASE),
                           exp, got);
            w[i] = exp;             //报告后恢复(再触发=重复写)
        }
    }
    return found;
}

// -----------------------------------------------------------------------------
// ① 预处理: 首帧武装(先扫非零残留), 每帧查一遍(前向前), 心跳低频打印
int preprocess(const NnImage* img, NnTensor* in, NnGeom* geom)
{
    (void)img;
    s_gen++;

    if (!s_armed)
    {
        //装载~首帧之间: bss本应为全零, 非零=这段时间被写过
        unsigned residue = 0;
        for (unsigned i = 0; i < sizeof(s_zone) / 4; i++)
            if (s_zone[i] != 0 && ++residue <= 8)
                xil_printf("[probe] residue zone +%u = %08x\r\n", i * 4,
                           s_zone[i]);
        const unsigned* zin = (const unsigned*)s_in;
        for (unsigned i = 0; i < 16384u; i++)      //s_in抽查头64KB
            if (zin[i] != 0 && ++residue <= 8)
                xil_printf("[probe] residue s_in +%u = %08x\r\n", i * 4,
                           zin[i]);
        xil_printf("[probe] residue_hits=%u (load->first frame)\r\n", residue);

        const unsigned zone_off0 =
            ((unsigned)((uintptr_t)s_zone - MOD_BASE)) / 4u;
        for (unsigned s = 0; s < CAN_SLOTS; s++)
        {
            unsigned* w = s_zone + (size_t)s * CAN_STRIDE_U32;
            const unsigned base_off = zone_off0 + s * CAN_STRIDE_U32;
            for (unsigned i = 0; i < CAN_WORDS; i++)
                w[i] = can_expect(base_off + i);
        }
        s_crc_ref = code_crc();
        s_armed = 1;
        xil_printf("[probe] armed: slots=%u canary=%uKB code_bytes=%u "
                   "crc_ref=%08x gen=%u\r\n",
                   CAN_SLOTS, CAN_SLOTS * (CAN_WORDS * 4) / 1024,
                   PROBE_CODE_BYTES, s_crc_ref, s_gen);
    }
    else if (check_all("pre") != 0 || s_gen % 25u == 0u)
    {
        xil_printf("[probe] gen=%u pre ok hits_total=%u crc=%08x\r\n",
                   s_gen, s_hits_total, code_crc());
    }

    in->data = s_in;
    in->w = IN_SIZE;
    in->h = IN_SIZE;
    in->c = 3;
    geom->scale = 1.f;
    geom->pad_x = 0;
    geom->pad_y = 0;
    geom->src_w = img ? img->w : 0;
    geom->src_h = img ? img->h : 0;
    return 0;
}

// -----------------------------------------------------------------------------
// ③ 解码: 恒空结果 + 前向后复查(与前向前的检查夹住前向窗口)
int decode(const NnTensor* outs, int out_n, const NnGeom* geom, NnResult* res)
{
    (void)outs; (void)out_n; (void)geom;
    res->box_n = 0;
    res->topk_n = 0;
    check_all("post");
    return 0;
}

const NnModelDef g_def = {
    NN_PIPELINE_ABI_VER,
    "nn_probe",
    "检测",
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
