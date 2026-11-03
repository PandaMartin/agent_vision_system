// =============================================================================
// mem_layout —— DDR 地址布局约定(单一事实来源, 与 lscript.ld 配合)
//
//   0x00100000  程序/数据区起点(见 lscript.ld; __bss_end 必须 <0x01100000,
//                lscript.ld 有 ASSERT 护栏——固件压进帧缓存区只会表现为花屏)
//   0x01100000  VDMA1 三帧缓存区(HDMI链路: 摄像头写入+HDMI读出, 3 x 1280*720*3
//                ≈ 7.9MB, 上界 <0x01A00000)
//   0x01A00000  LCD 彩条单帧缓存(VDMA0读出, 最大1280x800x3 ≈ 2.9MB, 上界 <0x02000000)
//   0x02000000  malloc 堆(_sbrk 用 _heap_start/_heap_end 符号, 224MB, 即
//                0x02000000~0x10000000; 起址由 lscript.ld .heap 节头显式钉住)
//   0x10000000  应用栈区(主栈4MB+异常栈≈6KB, 上界 0x10401800 随栈常量变; 栈尾
//                压过 0x11100000 由 lscript.ld ASSERT 护栏拦截)
//   0x11100000  推理整帧快照工作缓存(1280x720x3 ≈ 2.8MB; 当前无消费者——
//                旧推理应用已删, 新推理应用落地时在此取图复用)
//   0x20000000  SD应用区(loader.h APP_LOAD_BASE, 16MB, 上界 0x21000000)
//   0x21000000  模型程序模块区(model.mod加载基址, 4MB, 上界 0x21400000)
//   0x3FFFFF00  WDT挂数守护scratch(8B: magic+连续挂死计数, nn_wdt.c; DDR真顶部,
//                WDT复位保留, 与固件任何段不相交)
//
// 新增大块内存用途时先在此登记, 避免各模块地址互相踩踏
// =============================================================================
#pragma once

//HDMI链路帧缓存基址(VDMA1: 摄像头S2MM写入 + HDMI MM2S读出, 三缓冲环)
#define MEM_FRAME_BUFFER_BASE  0x01100000u

//LCD彩条帧缓存基址(VDMA0 MM2S读出, 单缓冲)
#define MEM_LCD_BUFFER_BASE    0x01A00000u

//推理/处理用整帧快照工作缓存地址
#define MEM_WORK_BUFFER_ADDR   0x11100000u

//模型程序模块区基址(SD模型包的model.mod加载到此; 见docs/模型平台化设计.md)
#define MEM_MODEL_MOD_BASE     0x21000000u
#define MEM_MODEL_MOD_SIZE     0x00400000u

//WDT挂数守护scratch(nn_wdt.c 的挂死计数, DDR真顶部; WDT复位保留, 上电随机由
//magic识别; 8字节勿扩——与堆/栈/任何固件段保持不相交)
#define MEM_GUARD_SCRATCH_ADDR 0x3FFFFF00u
