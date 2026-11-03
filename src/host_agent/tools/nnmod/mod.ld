/* =============================================================================
 * mod.ld —— 模型程序模块(model.mod)链接脚本
 *
 * 模块链接在固定地址(MEM_MODEL_MOD_BASE=0x21000000, 见 src/hw/mem_layout.h;
 * 链接地址=加载地址, 零重定位):
 *   arm-none-eabi-gcc -nostdlib -nostartfiles -Wl,-T,mod.ld -Wl,-T,fw_exports.ld
 * 第一函数必须是 nn_mod_boot(放 .mod_entry_first 节, 恒在 ORIGIN=入口0偏移),
 * 返回 const NnModelDef*(契约见 src/sw/impl/nn/nn_pipeline.h)。
 * bin布局: [NmodHeader 64B(构建工具nnmod.py封包)][text+data]; bss由加载器清零。
 * 固件函数直接调用: 符号来自 fw_exports.ld(tools/sdapp export 生成的全仓库地址表)。
 * ============================================================================= */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(nn_mod_boot)

MEMORY
{
    MOD (rwx) : ORIGIN = 0x21000000, LENGTH = 0x00400000
}

SECTIONS
{
    .text : {
        KEEP(*(.mod_entry_first))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > MOD

    .data : ALIGN(8) {
        *(.data .data.*)
    } > MOD

    .bss (NOLOAD) : ALIGN(8) {
        *(.bss .bss.*)
        *(COMMON)
    } > MOD

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.ARM.exidx .ARM.exidx.*)
        *(.ARM.attributes)
    }
}

ASSERT(ADDR(.bss) + SIZEOF(.bss) <= ORIGIN(MOD) + LENGTH(MOD), "MOD region overflow")
