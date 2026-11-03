// =============================================================================
// model_mgr —— SD模型包管理实现
//
// 全部用C库(malloc/f_gets), 无C++ STL(ncnn SIMPLESTL约定);
// 扫描 defensive: 四件缺一/模块不可boot的文件夹跳过并打印原因。
// =============================================================================

#include "sw/impl/nn/model_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "xil_printf.h"
#include "ff.h"
}

#include "sw/impl/nn/nn_module.h"
#include "sw/impl/nn/nn_runner.h"
#include "sw/impl/nn/nn_wdt.h"
#include "sw/impl/sd/sd.h"

//模块子系统内多出口函数的看门狗会话(构造上狗/析构关狗, 免逐出口补关)
struct NnWdtSession
{
    NnWdtSession() { nn_wdt_arm(); }
    ~NnWdtSession() { nn_wdt_disarm(); }
};

#define MODELS_ROOT   "/models"
#define MOD_NAME      "model.mod"
#define PARAM_NAME    "model.param.bin"
#define WEIGHT_NAME   "model.bin"
#define LABELS_NAME   "labels.txt"

//扫描到的模型记录表(记录体里文件夹/路径/tag为扫描期内容, 其余激活期填充)
static NnModel s_recs[NN_MAX_MODELS];
static int s_count  = 0;
static int s_active = -1;

//labels.txt 整块文本与行指针表(激活模型的, 存储归本模块)
static char*  s_label_buf  = 0;
static char** s_label_ptrs = 0;
static int    s_label_cnt  = 0;

#define LABEL_MAX 1024      //类别数上限

// -----------------------------------------------------------------------------
// labels.txt: 整块读入, 原地按行拆0, 建指针表
static void free_labels(void)
{
    if (s_label_ptrs) { free(s_label_ptrs); s_label_ptrs = 0; }
    if (s_label_buf)  { free(s_label_buf);  s_label_buf  = 0; }
    s_label_cnt = 0;
}

static int load_labels(const char* folder)
{
    free_labels();

    char path[96];
    snprintf(path, sizeof(path), "%s/%s/%s", MODELS_ROOT, folder, LABELS_NAME);

    unsigned long sz = 0;
    //sd_read_file会malloc(sz+64), 结尾余量够放终止0
    s_label_buf = (char*)sd_read_file(path, &sz);
    if (s_label_buf == 0 || sz == 0)
        return -1;
    s_label_buf[sz] = 0;

    //数行数('\n'结尾算一行, 末尾无'\n'补一行)
    int n = 0;
    for (unsigned long i = 0; i < sz; i++)
        if (s_label_buf[i] == '\n')
            n++;
    if (s_label_buf[sz - 1] != '\n')
        n++;
    if (n > LABEL_MAX)
    {
        xil_printf("[models] labels > %d, truncated\r\n", LABEL_MAX);
        n = LABEL_MAX;
    }

    s_label_ptrs = (char**)malloc((size_t)n * sizeof(char*));
    if (s_label_ptrs == 0)
    {
        free_labels();
        return -2;
    }

    char* p = s_label_buf;
    for (int i = 0; i < n; i++)
    {
        s_label_ptrs[i] = p;
        char* nl = strchr(p, '\n');
        if (nl == 0)
            break;
        if (nl > p && nl[-1] == '\r')
            nl[-1] = 0;
        *nl = 0;
        p = nl + 1;
    }
    s_label_cnt = n;
    return 0;
}

//文件存在性探测(app_task上下文, FatFs直接开)
static int file_exists(const char* path)
{
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK)
        return 0;
    f_close(&fp);
    return 1;
}

// -----------------------------------------------------------------------------
int nn_models_scan(void)
{
    //连续挂死保护门: 模块代码在扫描路径执行(nn_mod_boot), 坏模块会挂死
    //开机——连续NN_GUARD_MAX次看门狗复位后, 本轮跳过模块执行, 桌面可用,
    //用户经文件应用删除问题model.mod后重推即可自救
    if (nn_guard_take_skip_slot())
    {
        xil_printf("[models] 连续挂死保护: 本轮跳过模型模块执行。\r\n"
                   "         请用\"文件\"应用删除问题model.mod, 再从\"文件传输\""
                   "重推良版。\r\n");
        s_count = 0;
        s_active = -1;
        return 0;
    }

    NnWdtSession wdt;                       //扫描全程上狗, 模块挂死→硬件复位
    //重扫=清单重建: 丢弃当前激活(引擎权重与模块一并卸载, 需重新激活)
    if (s_active >= 0)
        xil_printf("[models] rescan: deactivating current model\r\n");
    xil_printf("[models] scan begin\r\n");
    nn_unload();
    nn_mod_unload();
    free_labels();
    s_active = -1;
    s_count = 0;

    DIR dir;
    if (f_opendir(&dir, MODELS_ROOT) != FR_OK)
    {
        xil_printf("[models] /models not found on SD, copy model folders\r\n");
        nn_guard_clear();
        return 0;
    }

    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0])
    {
        nn_wdt_kick();                      //每个文件夹一轮, 喂狗
        if (!(fno.fattrib & AM_DIR) || fno.fname[0] == '.')
            continue;
        if (s_count >= NN_MAX_MODELS)
        {
            xil_printf("[models] only first %d kept\r\n", NN_MAX_MODELS);
            break;
        }

        NnModel* rec = &s_recs[s_count];
        memset(rec, 0, sizeof(*rec));

        //按目标容量截断拷贝并显式补0(strncpy在源串恰好等长时不写终止符)
        size_t n = strnlen(fno.fname, sizeof(rec->folder) - 1);
        memcpy(rec->folder, fno.fname, n);
        rec->folder[n] = '\0';
        snprintf(rec->param_file, sizeof(rec->param_file),
                 "%s/%s/%s", MODELS_ROOT, rec->folder, PARAM_NAME);
        snprintf(rec->bin_file, sizeof(rec->bin_file),
                 "%s/%s/%s", MODELS_ROOT, rec->folder, WEIGHT_NAME);

        //三个数据文件缺一不可
        char path[96];
        snprintf(path, sizeof(path), "%s/%s/%s",
                 MODELS_ROOT, rec->folder, LABELS_NAME);
        if (!file_exists(rec->param_file) || !file_exists(rec->bin_file)
            || !file_exists(path))
        {
            xil_printf("[models] skip %s: 缺数据文件"
                       "(param.bin/model.bin/labels.txt)\r\n", rec->folder);
            continue;
        }

        //短暂加载模块读tag, 顺带完整校验(头部/版本/boot)——过了这关才算模型
        snprintf(path, sizeof(path), "%s/%s/%s",
                 MODELS_ROOT, rec->folder, MOD_NAME);
        const NnModelDef* d = nn_mod_load(path);
        if (d == 0)
        {
            xil_printf("[models] skip %s: model.mod 不可用(原因见上)\r\n",
                       rec->folder);
            continue;
        }
        if (d->tag)
        {
            n = strnlen(d->tag, sizeof(rec->tag) - 1);
            memcpy(rec->tag, d->tag, n);
            rec->tag[n] = '\0';
        }
        nn_mod_unload();        //扫描只需自检与tag, 立即卸载

        s_count++;
    }
    f_closedir(&dir);

    nn_wdt_disarm();                        //扫描完成关狗(会话析构兜底)
    nn_guard_clear();                       //完整通过: 清挂死计数
    xil_printf("[models] %d model(s) in /models:\r\n", s_count);
    for (int i = 0; i < s_count; i++)
        xil_printf("   [%d] %s (%s)\r\n", i, s_recs[i].folder, s_recs[i].tag);
    return s_count;
}

int nn_models_count(void)
{
    return s_count;
}

const char* nn_model_name(int index)
{
    if (index < 0 || index >= s_count)
        return 0;
    return s_recs[index].folder;
}

const char* nn_model_tag(int index)
{
    if (index < 0 || index >= s_count)
        return "";
    return s_recs[index].tag;
}

const NnModel* nn_model_record(int index)
{
    if (index < 0 || index >= s_count)
        return 0;
    return &s_recs[index];
}

int nn_model_activate(int index)
{
    if (index < 0 || index >= s_count)
        return -1;

    NnWdtSession wdt;                       //模块加载+权重读取全程上狗
    NnModel* rec = &s_recs[index];
    char mod_path[96];
    snprintf(mod_path, sizeof(mod_path), "%s/%s/%s",
             MODELS_ROOT, rec->folder, MOD_NAME);

    free_labels();
    nn_wdt_kick();

    //1.标签表(labels.txt必需, 发布层按label号翻译名字)
    if (load_labels(rec->folder) != 0)
    {
        xil_printf("[models] %s: labels.txt missing/empty\r\n", rec->folder);
        return -2;
    }
    rec->labels = (const char* const*)s_label_ptrs;
    rec->num_classes = s_label_cnt;

    //2.模型程序模块
    nn_wdt_kick();
    if (nn_mod_load(mod_path) == 0)
    {
        free_labels();
        rec->labels = 0;
        return -3;
    }
    rec->def = nn_mod_current();

    //3.权重(nn_load重复调用会先卸载上一个模型)
    nn_wdt_kick();
    if (nn_load(rec) != 0)
    {
        nn_mod_unload();
        rec->def = 0;
        rec->labels = 0;
        free_labels();
        return -4;
    }

    s_active = index;
    xil_printf("[models] active: [%d] %s (%d classes)\r\n",
               index, rec->folder, rec->num_classes);
    return 0;
}

int nn_active_index(void)
{
    return s_active;
}

const NnModel* nn_active_model(void)
{
    if (s_active < 0)
        return 0;
    return &s_recs[s_active];
}
