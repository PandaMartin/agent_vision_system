/* =============================================================================
 * app.ld —— SD应用链接脚本(M2)
 *
 * 应用链接在固定地址(APP_LOAD_BASE=0x20000000, 链接地址=加载地址, 零重定位):
 *   gcc -nostdlib -nostartfiles -Wl,-T,app.ld -Wl,-T,fw_exports.ld -lgcc ...
 * 第一函数必须是app_boot(放.app_entry_first节, 恒在ORIGIN=入口0偏移):
 *   __attribute__((section(".app_entry_first"))) void app_boot(void){ app_main(); }
 * bin布局: [AppHeader 64B(构建工具封包)][text+data]; bss由加载器清零。
 * 固件函数直接调用: 符号来自fw_exports.ld(全仓库自动导出)。
 * ============================================================================= */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(app_boot)

MEMORY
{
    APP (rwx) : ORIGIN = 0x20000000, LENGTH = 0x01000000
}

SECTIONS
{
    .text : {
        KEEP(*(.app_entry_first))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > APP

    .data : ALIGN(8) {
        *(.data .data.*)
    } > APP

    .bss (NOLOAD) : ALIGN(8) {
        *(.bss .bss.*)
        *(COMMON)
    } > APP

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.ARM.exidx .ARM.exidx.*)
        *(.ARM.attributes)
    }
}

ASSERT(ADDR(.bss) + SIZEOF(.bss) <= ORIGIN(APP) + LENGTH(APP), "APP region overflow")
