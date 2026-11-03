// =============================================================================
// model_mgr —— SD卡模型管理实现
//
// 全部用C库(malloc/sscanf/f_gets), 无C++ STL(ncnn SIMPLESTL约定);
// 解析 defensive: 坏行跳过, 必填字段缺失/文件缺失返回失败并打印原因
// =============================================================================

#include "sw/nn/model_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "xil_printf.h"
#include "ff.h"
}

#include "sw/nn/nn_runner.h"
#include "sw/storage/sd_io.h"

#define MODELS_ROOT   "/models"
#define CFG_NAME      "model.cfg"
#define PARAM_NAME    "model.param.bin"
#define WEIGHT_NAME   "model.bin"
#define LABELS_NAME   "labels.txt"
#define CFG_LINE_MAX  128
#define LABEL_MAX     1024      //类别数上限

//扫描到的模型目录项
static struct {
    char folder[48];    //文件夹名(=模型ID)
    char name[32];      //cfg里的显示名
} s_models[NN_MAX_MODELS];
static int s_count  = 0;
static int s_active = -1;

//当前激活模型的运行期档案(nn_runner/post_process直接使用)
static NnModelProfile s_profile;
static char*  s_label_buf  = 0;    //labels.txt整块文本(malloc)
static char** s_label_ptrs = 0;   //每行一个指针(malloc)
static int    s_label_cnt  = 0;

// -----------------------------------------------------------------------------
// cfg 解析
static char* trim(char* s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        e--;
    *e = 0;
    return s;
}

static void parse_3f(const char* val, float out[3])
{
    if (sscanf(val, "%f,%f,%f", &out[0], &out[1], &out[2]) != 3)
        xil_printf("[models] bad 3-value field: %s\r\n", val);
}

static NnPostType parse_post(const char* val)
{
    if (strcmp(val, "retinaface") == 0)
        return NN_POST_DETECT_RETINA;
    if (strcmp(val, "detect") == 0)
        return NN_POST_DETECT_SSD;
    return NN_POST_CLASSIFY;
}

//解析 /models/<folder>/model.cfg 到 prof, 返回0成功
static int load_cfg(const char* folder, NnModelProfile* prof)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s/%s", MODELS_ROOT, folder, CFG_NAME);

    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK)
        return -1;

    //字段默认值
    memset(prof, 0, sizeof(*prof));
    strncpy(prof->name, folder, sizeof(prof->name) - 1);
    prof->pixel_bgr  = 1;
    prof->pixel_bgr2rgb = 0;
    prof->norm[0] = prof->norm[1] = prof->norm[2] = 1.0f;
    prof->topk       = 3;
    prof->score_th   = 0.50f;
    prof->nms_iou_th = 0.45f;
    prof->max_keep   = 32;
    prof->retina_strides[0] = 8;
    prof->retina_strides[1] = 16;
    prof->retina_strides[2] = 32;
    for (int i = 0; i < 3; i++)
    {
        prof->blob_cls[i]  = -1;
        prof->blob_bbox[i] = -1;
        prof->blob_lmk[i]  = -1;
    }

    char line[CFG_LINE_MAX];
    while (f_gets(line, sizeof(line), &fp) != 0)   //BSP已开FF_USE_STRFUNC
    {
        char* s = trim(line);
        if (*s == 0 || *s == '#' || *s == '[')
            continue;                       //空行/注释/节名
        char* eq = strchr(s, '=');
        if (eq == 0)
            continue;
        *eq  = 0;
        char* key = trim(s);
        char* val = trim(eq + 1);

        if      (strcmp(key, "name") == 0)       { strncpy(prof->name, val, sizeof(prof->name) - 1); }
        else if (strcmp(key, "input_size") == 0) prof->input_size = (u16)atoi(val);
        else if (strcmp(key, "pixel") == 0)
        {
            if (strcmp(val, "bgr2rgb") == 0)
            {
                prof->pixel_bgr = 1;
                prof->pixel_bgr2rgb = 1;
            }
            else
            {
                prof->pixel_bgr = (u8)(strcmp(val, "rgb") != 0);
                prof->pixel_bgr2rgb = 0;
            }
        }
        else if (strcmp(key, "blob_in") == 0)    prof->blob_in = atoi(val);
        else if (strcmp(key, "blob_out") == 0)   prof->blob_out = atoi(val);
        else if (strcmp(key, "mean") == 0)       parse_3f(val, prof->mean);
        else if (strcmp(key, "norm") == 0)       parse_3f(val, prof->norm);
        else if (strcmp(key, "post") == 0)       prof->post = parse_post(val);
        else if (strcmp(key, "topk") == 0)       prof->topk = atoi(val);
        else if (strcmp(key, "score_th") == 0)   prof->score_th = (float)atof(val);
        else if (strcmp(key, "nms_iou_th") == 0) prof->nms_iou_th = (float)atof(val);
        else if (strcmp(key, "max_keep") == 0)   prof->max_keep = (u16)atoi(val);
        else if (strcmp(key, "blob_cls8") == 0)  prof->blob_cls[0] = atoi(val);
        else if (strcmp(key, "blob_cls16") == 0) prof->blob_cls[1] = atoi(val);
        else if (strcmp(key, "blob_cls32") == 0) prof->blob_cls[2] = atoi(val);
        else if (strcmp(key, "blob_bbox8") == 0) prof->blob_bbox[0] = atoi(val);
        else if (strcmp(key, "blob_bbox16") == 0) prof->blob_bbox[1] = atoi(val);
        else if (strcmp(key, "blob_bbox32") == 0) prof->blob_bbox[2] = atoi(val);
        else if (strcmp(key, "blob_lmk8") == 0)  prof->blob_lmk[0] = atoi(val);
        else if (strcmp(key, "blob_lmk16") == 0) prof->blob_lmk[1] = atoi(val);
        else if (strcmp(key, "blob_lmk32") == 0) prof->blob_lmk[2] = atoi(val);
        //未知键忽略(向前兼容)
    }
    f_close(&fp);

    if (prof->input_size == 0)               //唯一必填字段
    {
        xil_printf("[models] %s: input_size missing in model.cfg\r\n", folder);
        return -2;
    }
    if (prof->post == NN_POST_DETECT_RETINA)
    {
        for (int i = 0; i < 3; i++)
        {
            if (prof->blob_cls[i] < 0 || prof->blob_bbox[i] < 0)
            {
                xil_printf("[models] %s: retinaface blob_cls/bbox missing\r\n", folder);
                return -2;
            }
        }
    }
    return 0;
}

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

// -----------------------------------------------------------------------------
int nn_models_scan(void)
{
    s_count = 0;

    DIR dir;
    if (f_opendir(&dir, MODELS_ROOT) != FR_OK)
    {
        xil_printf("[models] /models not found on SD, copy model folders\r\n");
        return 0;
    }

    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0])
    {
        if (!(fno.fattrib & AM_DIR) || fno.fname[0] == '.')
            continue;
        if (s_count >= NN_MAX_MODELS)
        {
            xil_printf("[models] only first %d kept\r\n", NN_MAX_MODELS);
            break;
        }
        //必须有合法model.cfg才收录(顺带校验了档案可读性)
        NnModelProfile tmp;
        if (load_cfg(fno.fname, &tmp) != 0)
        {
            xil_printf("[models] skip %s\r\n", fno.fname);
            continue;
        }
        strncpy(s_models[s_count].folder, fno.fname, sizeof(s_models[s_count].folder) - 1);
        strncpy(s_models[s_count].name, tmp.name, sizeof(s_models[s_count].name) - 1);
        s_count++;
    }
    f_closedir(&dir);

    xil_printf("[models] %d model(s) in /models:\r\n", s_count);
    for (int i = 0; i < s_count; i++)
        xil_printf("   [%d] %s\r\n", i, s_models[i].name);
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
    return s_models[index].name;
}

int nn_model_activate(int index)
{
    if (index < 0 || index >= s_count)
        return -1;

    free_labels();

    //1.解析档案cfg
    if (load_cfg(s_models[index].folder, &s_profile) != 0)
        return -2;

    //2.标签表(labels.txt为必需文件)
    if (load_labels(s_models[index].folder) != 0)
    {
        xil_printf("[models] %s: labels.txt missing/empty\r\n", s_models[index].folder);
        return -3;
    }
    s_profile.labels     = (const char* const*)s_label_ptrs;
    s_profile.num_classes = s_label_cnt;

    //3.权重路径并加载(nn_load重复调用会先卸载上一个模型)
    snprintf(s_profile.param_file, sizeof(s_profile.param_file),
             "%s/%s/%s", MODELS_ROOT, s_models[index].folder, PARAM_NAME);
    snprintf(s_profile.bin_file, sizeof(s_profile.bin_file),
             "%s/%s/%s", MODELS_ROOT, s_models[index].folder, WEIGHT_NAME);
    if (nn_load(&s_profile) != 0)
    {
        free_labels();
        return -4;
    }

    s_active = index;
    xil_printf("[models] active: [%d] %s (%d classes)\r\n",
               index, s_profile.name, s_profile.num_classes);
    return 0;
}

int nn_active_index(void)
{
    return s_active;
}
