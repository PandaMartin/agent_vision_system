// =============================================================================
// sd_io —— SD 卡(FatFs)基础读写服务实现
//
// FatFs 的 FATFS 对象归本模块所有, 全局只挂载一次
// =============================================================================

#include "sw/impl/sd/sd.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "xil_printf.h"
#include "ff.h"
}

static FATFS s_fatfs;
static int s_mounted = 0;

int sd_mount(void)
{
    if (s_mounted)
        return 0;

    FRESULT fr = f_mount(&s_fatfs, "", 1);
    xil_printf("f_mount returns %d (0=OK)\r\n", (int)fr);
    if (fr != FR_OK)
        return -1;

    s_mounted = 1;
    return 0;
}

int sd_mounted(void)
{
    return s_mounted;
}

//名字比较(不区分大小写, ASCII): a<b返回负
static int sd_name_cmp(const char* a, const char* b)
{
    for (;; a++, b++)
    {
        unsigned char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb || ca == 0)
            return (int)ca - (int)cb;
    }
}

//名字安全拷贝(截断补0)
static void entry_copy_name(char* dst, const char* src)
{
    unsigned i = 0;
    for (; i + 1 < sizeof(((SdDirEntry*)0)->name) && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

int sd_list_dir(const char* path, SdDirEntry* out, int max,
                int* n, int* truncated)
{
    if (n) *n = 0;
    if (truncated) *truncated = 0;
    if (!s_mounted)
        return -1;

    DIR dir;
    if (f_opendir(&dir, path) != FR_OK)
    {
        xil_printf("[sd] opendir %s failed\r\n", path);
        return -2;
    }

    //收集(容量封顶, 超出记截断): 目录与文件分别计数
    int cnt = 0;
    int ndir = 0;
    int total = 0;
    FILINFO fno;
    for (;;)
    {
        FRESULT fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0)
            break;                               //结束或读错(按结束处理)
        total++;
        if (cnt >= max)
        {
            if (truncated) *truncated = 1;
            continue;                            //继续数完总数
        }
        SdDirEntry* e = &out[cnt];
        entry_copy_name(e->name, fno.fname);
        e->is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
        e->size = (uint32_t)fno.fsize;
        if (e->is_dir)
        {
            SdDirEntry t = out[ndir];            //目录挪到前段(稳定交换)
            out[ndir++] = out[cnt];
            out[cnt] = t;
        }
        cnt++;
    }
    f_closedir(&dir);

    //两段各自插入排序(按名, 条数<=64, 足够快); 简单字典序即可
    for (int s = 0; s < 2; s++)
    {
        const int lo = (s == 0) ? 0 : ndir;
        const int hi = (s == 0) ? ndir : cnt;
        for (int i = lo + 1; i < hi; i++)
        {
            SdDirEntry t = out[i];
            int j = i - 1;
            for (; j >= lo && sd_name_cmp(out[j].name, t.name) > 0; j--)
                out[j + 1] = out[j];
            out[j + 1] = t;
        }
    }

    if (n) *n = cnt;
    return 0;
}

int sd_read_bytes(const char* path, uint32_t offset, void* dst, uint32_t len)
{
    if (!s_mounted)
        return -1;
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK)
        return -2;
    FRESULT fr = f_lseek(&fp, offset);
    UINT got = 0;
    if (fr == FR_OK)
        fr = f_read(&fp, dst, len, &got);
    f_close(&fp);
    if (fr != FR_OK || got != len)
        return -3;
    return 0;
}

int sd_write_file(const char* path, const uint8_t* data, uint32_t len)
{
    if (!s_mounted)
        return -1;
    FIL fp;
    if (f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        xil_printf("[sd] create %s failed\r\n", path);
        return -2;
    }
    UINT put = 0;
    FRESULT fr = f_write(&fp, data, len, &put);
    if (fr == FR_OK)
        fr = f_sync(&fp);
    f_close(&fp);
    if (fr != FR_OK || put != len)
    {
        xil_printf("[sd] write %s failed (%d/%u)\r\n", path, (int)put,
                   (unsigned)len);
        return -3;
    }
    return 0;
}

//递归删除深度上限(防异常嵌套兜底)
#define DEL_TREE_DEPTH 8

//递归删除目录(path不带尾部'/'): 先清空其下内容再移除目录本身
//返回0成功; -2删除失败。child路径栈为静态: FatFs单线程纪律下无并发使用者
static int del_tree(const char* path, int depth)
{
    if (depth >= DEL_TREE_DEPTH)
    {
        xil_printf("[sd] rmtree %s: too deep\r\n", path);
        return -2;
    }
    static char child[DEL_TREE_DEPTH][FF_MAX_LFN + 8];

    DIR dir;
    if (f_opendir(&dir, path) != FR_OK)
        return -2;
    int rc = 0;
    FILINFO fno;
    for (;;)
    {
        const FRESULT fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0)
            break;                               //结束或读错(按结束处理)
        char* cp = child[depth];
        const int len = snprintf(cp, sizeof(child[0]), "%s/%s",
                                 path, fno.fname);
        if (len < 0 || len >= (int)sizeof(child[0]))
        {
            rc = -2;                             //路径超长: 跳过该项
            xil_printf("[sd] rmtree: name too long: %s/%s\r\n",
                       path, fno.fname);
            continue;
        }
        if (fno.fattrib & AM_DIR)
        {
            const int r = del_tree(cp, depth + 1);
            if (r != 0)
                rc = r;
        }
        else if (f_unlink(cp) != FR_OK)
            rc = -2;
    }
    f_closedir(&dir);
    if (rc == 0 && f_unlink(path) != FR_OK)
        rc = -2;                                 //目录本身最后移除
    return rc;
}

int sd_delete(const char* path)
{
    if (!s_mounted)
        return -1;
    //目录(含非空): 递归删除; 文件/其余: 直接unlink
    DIR dir;
    if (f_opendir(&dir, path) == FR_OK)
    {
        f_closedir(&dir);
        const int rc = del_tree(path, 0);
        xil_printf("[sd] rmtree %s: %s\r\n", path,
                   rc == 0 ? "ok" : "failed");
        return rc;
    }
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK)
    {
        xil_printf("[sd] unlink %s failed (%d)\r\n", path, (int)fr);
        return -2;
    }
    return 0;
}

//打开目标文件(重名自动换"_N"序号, N=1..99): 成功时fp保持打开, out=实际名
//返回FR_OK; 其他=失败(fp未打开)
static FRESULT copy_open_unique(FIL* fp, const char* dst,
                                char* out, unsigned cap)
{
    FRESULT fr = f_open(fp, dst, FA_WRITE | FA_CREATE_NEW);
    if (fr == FR_OK)
    {
        const unsigned len = (unsigned)strnlen(dst, cap - 1);
        memcpy(out, dst, len);
        out[len] = 0;
        return FR_OK;
    }
    if (fr != FR_EXIST)
        return fr;

    //重名: 拆主名与扩展名, 换"_N"序号再试(name_1.ext)
    char base[64];
    const unsigned blen = (unsigned)strnlen(dst, sizeof(base) - 1);
    memcpy(base, dst, blen);
    base[blen] = 0;
    char* dot = strrchr(base, '.');
    const char* slash = strrchr(base, '/');
    if (dot != 0 && slash != 0 && dot < slash)
        dot = 0;                             //'.','在目录段: 视为无扩展名
    for (int n = 1; n <= 99; n++)
    {
        char cand[64];
        if (dot != 0)
            snprintf(cand, sizeof(cand), "%.*s_%d%s",
                     (int)(dot - base), base, n, dot);
        else
            snprintf(cand, sizeof(cand), "%s_%d", base, n);
        fr = f_open(fp, cand, FA_WRITE | FA_CREATE_NEW);
        if (fr == FR_OK)
        {
            const unsigned len = (unsigned)strnlen(cand, cap - 1);
            memcpy(out, cand, len);
            out[len] = 0;
            return FR_OK;
        }
        if (fr != FR_EXIST)
            return fr;
    }
    return FR_EXIST;
}

int sd_copy_file(const char* src, const char* dst)
{
    if (!s_mounted)
        return -1;
    if (strcmp(src, dst) == 0)
        return -2;                           //原地自拷无意义(同名目录去重由GUI做)

    FIL fsrc;
    if (f_open(&fsrc, src, FA_READ) != FR_OK)
    {
        xil_printf("[sd] copy open src %s failed\r\n", src);
        return -3;
    }
    char real[64];
    FIL fdst;
    const FRESULT fr = copy_open_unique(&fdst, dst, real, sizeof(real));
    if (fr != FR_OK)
    {
        f_close(&fsrc);
        xil_printf("[sd] copy create %s failed (%d)\r\n", dst, (int)fr);
        return -4;
    }

    //分块搬运(静态缓冲: FatFs单线程纪律下无并发使用者)
    static uint8_t chunk[8192];
    UINT got = 0;
    FRESULT rr = FR_OK;
    for (;;)
    {
        rr = f_read(&fsrc, chunk, sizeof(chunk), &got);
        if (rr != FR_OK)
            break;
        if (got == 0)
            break;                           //源尾
        UINT put = 0;
        rr = f_write(&fdst, chunk, got, &put);
        if (rr != FR_OK || put != got)
            break;
    }
    f_close(&fsrc);
    f_close(&fdst);
    if (rr != FR_OK)
        f_unlink(real);                      //半截目标不留垃圾
    xil_printf("[sd] copy %s -> %s: %s\r\n", src, real,
               rr == FR_OK ? "ok" : "failed");
    return rr == FR_OK ? 0 : -5;
}

int sd_mkdir(const char* path)
{
    if (!s_mounted)
        return -1;
    FRESULT fr = f_mkdir(path);
    if (fr == FR_OK || fr == FR_EXIST)
        return 0;
    xil_printf("[sd] mkdir %s failed (%d)\r\n", path, (int)fr);
    return -2;
}

//逐级建目录(path如"0:/models/yolo", 途中已存在的层级跳过;
//网口推送任意路径文件前建父目录用)
int sd_mkdirs(const char* path)
{
    if (!s_mounted)
        return -1;
    char buf[64];
    const unsigned len = (unsigned)strnlen(path, sizeof(buf) - 1);
    memcpy(buf, path, len);
    buf[len] = 0;

    //逐个'/'截断试建: 根卷"0:"之后的第一级起
    for (unsigned i = 0; i < len; i++)
    {
        if (buf[i] != '/' || i == 0)
            continue;
        char save = buf[i];
        buf[i] = 0;                          //此前的一级路径
        FRESULT fr = f_mkdir(buf);
        if (fr != FR_OK && fr != FR_EXIST)
        {
            buf[i] = save;
            xil_printf("[sd] mkdir %s failed (%d)\r\n", buf, (int)fr);
            return -2;
        }
        buf[i] = save;
    }
    //最后一级(路径不以'/'结尾时)
    if (len > 0 && buf[len - 1] != '/')
    {
        FRESULT fr = f_mkdir(buf);
        if (fr != FR_OK && fr != FR_EXIST)
        {
            xil_printf("[sd] mkdir %s failed (%d)\r\n", buf, (int)fr);
            return -2;
        }
    }
    return 0;
}

unsigned char* sd_read_file(const char* path, unsigned long* out_size)
{
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK)
    {
        xil_printf("[sd] open %s failed\r\n", path);
        return 0;
    }
    unsigned long sz = (unsigned long)f_size(&fp);
    unsigned char* buf = (unsigned char*)malloc(sz + 64);
    if (buf == 0)
    {
        xil_printf("[sd] malloc %lu bytes failed\r\n", sz);
        f_close(&fp);
        return 0;
    }
    UINT got = 0;
    FRESULT fr = f_read(&fp, buf, (UINT)sz, &got);
    f_close(&fp);
    if (fr != FR_OK || got != (UINT)sz)
    {
        xil_printf("[sd] read %s failed\r\n", path);
        free(buf);
        return 0;
    }
    *out_size = sz;
    return buf;
}
