// =============================================================================
// nn_module —— 模型程序模块加载器实现
// (加载序列与 ext_app/sdapp.c 的SD应用启动同构: 校验/整读/搬运/清bss/同步cache)
// =============================================================================

#include "sw/impl/nn/nn_module.h"

#include <stdlib.h>
#include <string.h>

#include "xil_printf.h"

#include "hw/mem_layout.h"
#include "sw/app/fw_version_gen.h"
#include "sw/impl/port/port.h"
#include "sw/impl/sd/sd.h"

typedef const NnModelDef* (*NnModBootFn)(void);

static const NnModelDef* s_def;

//头部尺寸字段自洽(不涉版本)
static int hdr_valid(const NnModHeader* h)
{
    if (memcmp(h->magic, "NMOD", 4) != 0 || h->hdr_size != 64)
        return 0;
    if (h->load_size == 0 || h->load_size > MEM_MODEL_MOD_SIZE)
        return 0;
    if (h->bss_off < h->load_size)
        return 0;
    if (h->bss_off + h->bss_size > MEM_MODEL_MOD_SIZE)
        return 0;
    if (h->total_size != 64u + h->load_size || h->entry_off != 0)
        return 0;
    return 1;
}

const NnModelDef* nn_mod_load(const char* path)
{
    nn_mod_unload();

    NnModHeader h;
    if (sd_read_bytes(path, 0, &h, sizeof(h)) != 0)
    {
        xil_printf("[mod] %s: 读头失败\r\n", path);
        return 0;
    }
    if (!hdr_valid(&h))
    {
        xil_printf("[mod] %s: 头非法(不是nnmod.py封包的model.mod?)\r\n", path);
        return 0;
    }
    if (h.abi_ver != NN_PIPELINE_ABI_VER)
    {
        xil_printf("[mod] %s: 契约版本 v%u != 固件 v%u, 用当前固件重新 nnmod build\r\n",
                   path, h.abi_ver, NN_PIPELINE_ABI_VER);
        return 0;
    }
    if (h.fw_version != FW_EXPORTS_VERSION)
    {
        xil_printf("[mod] %s: 固件导出版本 0x%08x != 0x%08x, "
                   "重编固件并 sdapp export 后重新 nnmod build\r\n",
                   path, h.fw_version, FW_EXPORTS_VERSION);
        return 0;
    }

    unsigned long len = 0;
    unsigned char* bin = sd_read_file(path, &len);
    if (bin == 0 || len != h.total_size)
    {
        xil_printf("[mod] %s: 整读失败或长度不符\r\n", path);
        if (bin)
            free(bin);
        return 0;
    }

    uint8_t* base = (uint8_t*)(uintptr_t)MEM_MODEL_MOD_BASE;
    memcpy(base, bin + sizeof(NnModHeader), h.load_size);
    memset(base + h.bss_off, 0, h.bss_size);
    free(bin);
    port_cache_flush(base, h.bss_off + h.bss_size);
    port_icache_invalidate(base, h.bss_off + h.bss_size);

    const NnModelDef* d = ((NnModBootFn)(void*)base)();
    if (d == 0 || d->abi_ver != NN_PIPELINE_ABI_VER
        || d->preprocess == 0 || d->decode == 0
        || d->out_n == 0 || d->out_n > NN_OUT_MAX)
    {
        xil_printf("[mod] %s: nn_mod_boot 返回无效定义\r\n", path);
        return 0;
    }

    s_def = d;
    xil_printf("[mod] loaded %s (%s) abi=v%u outs=%u @0x%08x\r\n",
               d->name ? d->name : "?", d->tag ? d->tag : "?",
               (unsigned)d->abi_ver, (unsigned)d->out_n,
               MEM_MODEL_MOD_BASE);
    return s_def;
}

void nn_mod_unload(void)
{
    s_def = 0;      //模块区无需擦除, 下次load整块覆盖
}

const NnModelDef* nn_mod_current(void)
{
    return s_def;
}
