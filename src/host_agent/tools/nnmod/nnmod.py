#!/usr/bin/env python3
# =============================================================================
# nnmod —— 模型程序模块(model.mod)编译封包工具
#
# 与SD应用工具(tools/sdapp)机制同构但完全独立, 只做一个子命令:
#
#   build   编译封包一个模型模块:
#     python tools/nnmod/nnmod.py build <模块目录> [--name 模块名] [-o 输出]
#     交叉编译 <模块目录>/*.c -> 用 mod.ld + fw_exports.ld 链接到 0x21000000
#     (直接调固件函数真身, 零重定位) -> 封64字节NmodHeader -> 默认产出
#     <模块目录>/model.mod。板上所有模型统一叫 model.mod, 不同模型以
#     SD卡 /models/<文件夹名>/ 的文件夹名区分(拷入时保持该名字)。
#
# 符号导出的唯一事实来源仍是 tools/sdapp 的export产物, 本工具只消费不生产:
#   ../sdapp/fw_exports.ld    固件全符号地址表(链接 -T)
#   ../sdapp/fw_exports.json  固件符号导出版本号(封入头部, 固件侧校验)
# abi_ver 自动从 src/sw/impl/nn/nn_pipeline.h 提取(契约的唯一来源)。
#
# 完整流程: Vitis Build固件 -> sdapp export(至无NOTE) -> nnmod build各模块
# -> 模型包四文件(model.param.bin/model.bin/labels.txt/model.mod)拷入
# SD卡 /models/<文件夹名>/ 。设计见 src/docs/模型平台化设计.md
#
# 产物归属:
#   <模块目录>/model.mod             模块产出
#   <模块目录>/build/                编译中间件(.o/.elf/.map, 可删)
# =============================================================================
import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent     # <固件组件>/tools/nnmod
TOOLS = HERE.parent                        # tools/
APP = TOOLS.parent                         # 固件组件 cam_lcd_hdmi_servo_net
WS = APP.parent                            # Vitis工作区(平台在其下)
SRC = APP / "src"
PIPELINE_H = SRC / "sw" / "impl" / "nn" / "nn_pipeline.h"
SDAPP = TOOLS / "sdapp"                    # 只读其export产物
DEFAULT_TOOLCHAIN = (r"D:/software/AMDDesign/2025.2/Vitis/gnu/aarch32/nt/"
                     r"gcc-arm-none-eabi/bin/arm-none-eabi-")
def _bsp_include() -> str:
    bundled = APP / "bsp" / "include"
    if bundled.is_dir():
        return str(bundled)
    return str(WS / "FreeRtos_Platform" / "export"
               / "FreeRtos_Platform" / "sw"
               / "freertos_ps7_cortexa9_0" / "include")


DEFAULT_BSP_INC = _bsp_include()

MOD_BASE = 0x21000000                      # = MEM_MODEL_MOD_BASE(mem_layout.h)
MOD_SIZE = 0x00400000
MAGIC = b"NMOD"
HDR_SIZE = 64
HDR_FMT = "<4sIIIII16sIII12x"              # 4+4*5+16+4*3+12 = 64
# 字段: magic, hdr_size, abi_ver, fw_version, load_size, bss_size,
#       name[16], entry_off, total_size, bss_off, pad
# 类别标签等模型知识不放头部 —— 全部由模块的 NnModelDef 自述
# (扫描期固件短暂加载模块读 tag 后即卸载, 见 docs/模型平台化设计.md)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit("command failed: %s\n%s\n%s"
                 % (" ".join(str(c) for c in cmd), r.stdout, r.stderr))
    return r.stdout


def libgcc_path(prefix):
    """-nostdlib 不会把 libgcc 目录传给 ld。传入规范化后的绝对路径。"""
    probe = subprocess.run(
        [prefix + "gcc", "-mcpu=cortex-a9", "-mfpu=neon-vfpv3", "-mfloat-abi=hard",
         "-print-libgcc-file-name"],
        capture_output=True,
    )
    raw = (probe.stdout or b"").decode("utf-8", errors="replace").strip()
    if not raw or "\ufffd" in raw:
        raw = (probe.stdout or b"").decode("gbk", errors="replace").strip()
    if raw:
        path = Path(raw).resolve()
        if path.is_file():
            return str(path)
    return "-lgcc"


def read_abi_ver():
    if not PIPELINE_H.exists():
        sys.exit("契约头不存在: %s" % PIPELINE_H)
    m = re.search(r"#define\s+NN_PIPELINE_ABI_VER\s+(\d+)",
                  PIPELINE_H.read_text(encoding="utf-8"))
    if not m:
        sys.exit("契约头里找不到 NN_PIPELINE_ABI_VER: %s" % PIPELINE_H)
    return int(m.group(1))


def cmd_build(args):
    moddir = Path(args.moddir).resolve()
    if not moddir.is_dir():
        sys.exit("模块目录不存在: %s" % moddir)

    exports_ld = SDAPP / "fw_exports.ld"
    exports_json = SDAPP / "fw_exports.json"
    if not exports_ld.exists() or not exports_json.exists():
        sys.exit("缺少 %s —— 先跑: python tools/sdapp/sdapp.py export"
                 % exports_json)
    fw_ver = json.loads(exports_json.read_text(encoding="utf-8"))["version"]
    abi_ver = read_abi_ver()

    sources = sorted(moddir.glob("*.c")) + sorted(moddir.glob("*.cpp"))
    if not sources:
        sys.exit("no .c/.cpp files in %s" % moddir)

    #产物固定叫 model.mod(板上模型包的约定名); 需要别名时才用-o覆盖
    out = Path(args.out) if args.out else moddir / "model.mod"
    build = moddir / "build"
    build.mkdir(exist_ok=True)
    prefix = args.toolchain

    common = ["-mcpu=cortex-a9", "-mfpu=neon-vfpv3", "-mfloat-abi=hard",
              "-O2", "-Wall", "-ffreestanding",
              "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
              "-I" + str(SRC),
              "-isystem", args.bsp_include]
    #C++模块(调ncnn原语)与固件C++同旗标: 无STL/异常/RTTI, 头走cxxshim
    cpp_extra = ["-nostdinc++", "-fno-exceptions", "-fno-rtti", "-fpermissive",
                 "-I" + str(SRC / "sw" / "impl" / "nn" / "ncnn_inc"),
                 "-I" + str(SRC / "sw" / "impl" / "nn" / "cxxshim")]

    objs = []
    for src in sources:
        is_cpp = src.suffix == ".cpp"
        obj = build / (src.stem + ".o")
        run([prefix + ("g++" if is_cpp else "gcc")]
            + common + (cpp_extra if is_cpp else [])
            + ["-c", str(src), "-o", str(obj)])
        objs.append(obj)

    elf = build / "model.elf"
    run([prefix + "g++",
         "-mcpu=cortex-a9", "-mfpu=neon-vfpv3", "-mfloat-abi=hard",
         "-nostdlib", "-nostartfiles",
         "-Wl,-T," + str(HERE / "mod.ld"),
         "-Wl,-T," + str(exports_ld),
         "-Wl,--gc-sections", "-Wl,-Map," + str(build / "model.map"),
         libgcc_path(prefix),
         "-o", str(elf)] + [str(o) for o in objs])

    # bss的VMA与大小(节表): 加载器按VMA偏移清零(不假设紧贴数据尾)
    hdr_txt = run([prefix + "objdump", "-h", str(elf)])
    bss_size = 0
    bss_off = 0
    last_end = MOD_BASE
    for ln in hdr_txt.splitlines():
        parts = ln.split()
        # idx name size vma lma filesz align
        if len(parts) >= 7 and parts[1] in (".text", ".rodata", ".data"):
            last_end = max(last_end, int(parts[4], 16) + int(parts[2], 16))
        if len(parts) >= 7 and parts[1] == ".bss":
            bss_size = int(parts[2], 16)
            bss_off = int(parts[3], 16) - MOD_BASE
    if bss_size == 0:
        print("note: no .bss section")

    # 入口地址必须是MOD_BASE(nn_mod_boot在.mod_entry_first)
    nm = run([prefix + "nm", str(elf)])
    entry = None
    for ln in nm.splitlines():
        parts = ln.split()
        if len(parts) == 3 and parts[2] == "nn_mod_boot":
            entry = int(parts[0], 16)
    if entry != MOD_BASE:
        sys.exit("nn_mod_boot @0x%x, must be 0x%x "
                 "(section .mod_entry_first missing?)"
                 % (entry or 0, MOD_BASE))

    if bss_off < (last_end - MOD_BASE):
        sys.exit(".bss VMA overlaps loadable sections? bss@0x%x" % bss_off)
    if last_end - MOD_BASE > MOD_SIZE or bss_off + bss_size > MOD_SIZE:
        sys.exit("module exceeds MOD region (%d bytes)" % MOD_SIZE)

    raw = build / "model.raw"
    run([prefix + "objcopy", "-O", "binary", "--strip-unneeded",
         str(elf), str(raw)])
    body = raw.read_bytes()

    name = args.name.encode("utf-8")[:15]
    total = HDR_SIZE + len(body)
    hdr = struct.pack(HDR_FMT, MAGIC, HDR_SIZE, abi_ver, fw_ver & 0xFFFFFFFF,
                      len(body), bss_size, name, 0, total, bss_off)
    out.write_bytes(hdr + body)

    print("model.mod: %s" % out)
    print("  name=%s abi=v%u fw_version=0x%08x code=%d bss=%d@off+%d total=%d"
          % (args.name, abi_ver, fw_ver & 0xFFFFFFFF, len(body), bss_size,
             bss_off, total))
    print("  -> 拷入SD模型包: /models/<模型文件夹名>/model.mod")


# =============================================================================
def main():
    ap = argparse.ArgumentParser(
        description="模型程序模块(model.mod)编译封包工具")
    sub = ap.add_subparsers(dest="cmd", required=True)

    pb = sub.add_parser("build", help="编译封包一个模型模块 -> model.mod")
    pb.add_argument("moddir", help="模块源码目录(如 src/sd/models/<模型名>)")
    pb.add_argument("--name", default=None,
                    help="头部模块名(<=15字节UTF-8, 日志用; 默认取目录名)")
    pb.add_argument("-o", "--out", default=None,
                    help="输出文件(默认<模块目录>/model.mod)")
    pb.add_argument("--toolchain", default=DEFAULT_TOOLCHAIN)
    pb.add_argument("--bsp-include", default=DEFAULT_BSP_INC)
    pb.set_defaults(func=cmd_build)

    args = ap.parse_args()
    if args.name is None:
        args.name = Path(args.moddir).resolve().name
    args.func(args)


if __name__ == "__main__":
    main()
