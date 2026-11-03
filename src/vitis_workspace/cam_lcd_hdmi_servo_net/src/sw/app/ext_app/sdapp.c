// =============================================================================
// ext_sdapp —— SD应用扫描/启动实现(自17版main.cpp迁入, 逻辑不变)
//
// 两级扫描: /apps/*.bin 与 /apps/<目录>/*.bin(应用可带资源目录形态);
// 启动: 头校验(格式+固件版本) -> 整读 -> 拷入应用区(链接=加载地址, 零重定位)
// -> bss清零 -> cache同步 -> bridge置READY(gui侧launcher调entry)。
// =============================================================================

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <stdint.h>

#include "xil_types.h"
#include "xil_printf.h"

#include "sw/impl/sd/sd.h"
#include "sw/app/bridge.h"
#include "sw/app/ext_app/sdapp.h"
#include "sw/app/ext_app/loader.h"
#include "sw/app/fw_version_gen.h"
#include "sw/impl/port/port.h"

static const char SDAPPS_DIR[] = "0:/apps";

// -----------------------------------------------------------------------------
// 读应用头并校验(仅格式与版本; 搬运前的最终一致性在launch里复核)
// 返回0合法(ver_ok标出版本匹配); 负数=格式非法
static int sdapp_parse_hdr(const char* path, AppHeader* hdr, int* ver_ok)
{
    *ver_ok = 0;
    if (sd_read_bytes(path, 0, hdr, sizeof(AppHeader)) != 0)
        return -1;
    if (memcmp(hdr->magic, APP_MAGIC, 4) != 0 || hdr->hdr_size != APP_HDR_SIZE)
        return -2;
    if (hdr->load_size == 0 || hdr->load_size > APP_REGION_SIZE
        || hdr->bss_off < hdr->load_size
        || (uint64_t)hdr->bss_off + hdr->bss_size > APP_REGION_SIZE
        || hdr->total_size != APP_HDR_SIZE + hdr->load_size)
        return -3;
    *ver_ok = (hdr->fw_version == FW_EXPORTS_VERSION);
    return 0;
}

//把一个校验通过的app收进桌面列表(rel=相对SDAPPS_DIR路径, fallback=无头名时显示)
static void sdapp_collect(const char* rel, const char* fallback,
                          char (*names)[16], char (*files)[32], int* out_n)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SDAPPS_DIR, rel);

    AppHeader hdr;
    int ver_ok = 0;
    if (sdapp_parse_hdr(path, &hdr, &ver_ok) != 0)
        return;

    const int k = *out_n;
    if (hdr.name[0] != 0)
    {
        memcpy(names[k], hdr.name, sizeof(names[k]));
        names[k][sizeof(names[k]) - 1] = 0;
    }
    else
    {
        size_t fl = strnlen(fallback, 28);
        if (fl > 15) fl = 15;
        memcpy(names[k], fallback, fl);
        names[k][fl] = 0;
    }
    strncpy(files[k], rel, sizeof(files[k]) - 1);
    files[k][sizeof(files[k]) - 1] = 0;
    xil_printf("[apps] found '%s' (%s)%s\r\n", names[k], rel,
               ver_ok ? "" : " [版本过期]");
    *out_n = k + 1;
}

void ext_sdapp_scan(void)
{
    static char names[GUI_SDAPP_MAX][16];
    static char files[GUI_SDAPP_MAX][32];

    SdDirEntry ents[GUI_SDAPP_MAX];
    int n = 0, trunc = 0;
    const int err = sd_list_dir(SDAPPS_DIR, ents, GUI_SDAPP_MAX, &n, &trunc);
    int out_n = 0;
    if (err != 0)
    {
        gui_model_pub_sdapps(names, files, 0);
        return;
    }

    for (int i = 0; i < n && out_n < GUI_SDAPP_MAX; i++)
    {
        char rel[40];                        //相对SDAPPS_DIR的路径(补0封顶)
        size_t rel_len;

        if (ents[i].is_dir)
        {
            //子目录: 里面再找一层*.bin
            SdDirEntry sub[GUI_SDAPP_MAX];
            int sn = 0, s_trunc = 0;
            char sub_dir[56];
            snprintf(sub_dir, sizeof(sub_dir), "%s/%s", SDAPPS_DIR,
                     ents[i].name);
            if (sd_list_dir(sub_dir, sub, GUI_SDAPP_MAX, &sn, &s_trunc) != 0)
                continue;
            for (int j = 0; j < sn && out_n < GUI_SDAPP_MAX; j++)
            {
                if (sub[j].is_dir || (rel_len = strnlen(sub[j].name,
                        sizeof(sub[j].name))) < 4
                    || strcasecmp(sub[j].name + rel_len - 4, ".bin") != 0)
                    continue;
                snprintf(rel, sizeof(rel), "%.*s/%s",
                         (int)(sizeof(ents[i].name) - 1), ents[i].name,
                         sub[j].name);
                sdapp_collect(rel, sub[j].name, names, files, &out_n);
            }
            continue;
        }

        rel_len = strnlen(ents[i].name, sizeof(ents[i].name));
        if (rel_len < 4 || strcasecmp(ents[i].name + rel_len - 4, ".bin") != 0)
            continue;
        sdapp_collect(ents[i].name, ents[i].name, names, files, &out_n);
    }
    gui_model_pub_sdapps(names, files, out_n);
    xil_printf("[apps] %d app(s) in %s (listdir=%d)\r\n",
               out_n, SDAPPS_DIR, err);
}

// -----------------------------------------------------------------------------
// 启动: 校验 -> 整读 -> 拷入应用区 -> cache同步 -> READY(gui侧调entry)
static void sdapp_launch(int idx)
{
    GuiSnapshot s;
    gui_model_snapshot(&s);
    if (idx < 0 || idx >= s.sdapp_n)
        return;
    if (s.app_state != APPST_IDLE && s.app_state != APPST_FAILED)
        return;                              //已有一个在跑/在载

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SDAPPS_DIR, s.sdapp_file[idx]);

    AppHeader hdr;
    int ver_ok = 0;
    if (sdapp_parse_hdr(path, &hdr, &ver_ok) != 0)
    {
        gui_model_pub_app_state(APPST_FAILED, "文件损坏");
        return;
    }
    if (!ver_ok)
    {
        gui_model_pub_app_state(APPST_FAILED, "版本过期, 请重装应用");
        return;
    }

    gui_model_pub_app_state(APPST_LOADING, 0);

    //整读(头+代码), 校验后拷入应用区(链接地址=加载地址)
    unsigned long bin_len = 0;
    unsigned char* bin = sd_read_file(path, &bin_len);
    if (bin == 0 || bin_len < sizeof(AppHeader)
        || (unsigned long)hdr.total_size != bin_len
        || memcmp(bin, &hdr, sizeof(AppHeader)) != 0)
    {
        if (bin) free(bin);
        gui_model_pub_app_state(APPST_FAILED, "读取失败");
        return;
    }

    uint8_t* base = (uint8_t*)(uintptr_t)APP_LOAD_BASE;
    memcpy(base, bin + sizeof(AppHeader), hdr.load_size);
    memset(base + hdr.bss_off, 0, hdr.bss_size);
    free(bin);

    //D侧写出+I侧失效+屏障: 此后取指看到的是应用机器码
    port_cache_flush(base, hdr.bss_off + hdr.bss_size);
    port_icache_invalidate(base, hdr.bss_off + hdr.bss_size);
    port_sync_barrier();

    xil_printf("[apps] loaded %s (%lu+%u bytes) @0x%08x\r\n",
               s.sdapp_file[idx], bin_len - sizeof(AppHeader),
               hdr.bss_size, APP_LOAD_BASE);
    gui_model_pub_app_state(APPST_READY, 0);
}

void ext_sdapp_dispatch(const AppCmd* cmd)
{
    switch (cmd->type)
    {
    case APP_CMD_APP_RESCAN:
        ext_sdapp_scan();
        break;
    case APP_CMD_APP_LAUNCH:
        sdapp_launch(cmd->val);
        break;
    default:
        break;
    }
}
