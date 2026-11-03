#!/usr/bin/env python3
# =============================================================================
# sdapp —— SD应用开发套件(合并原 fw_exports_gen.py + build_app.py)
#
# 两个子命令, 对应SD应用开发流程的两步(在固件组件根
# vitis/cam_lcd_hdmi_servo_net/ 下运行):
#
#   ① export  固件符号导出 —— 固件每次Build之后跑:
#       python tools/sdapp/sdapp.py export [--map <fw.map>]
#       解析 build/fw.map -> 重写本目录 fw_exports.ld(全符号=地址表) +
#       fw_exports.json(版本号) + src/sw/app/fw_version_gen.h(固件内嵌版本)。
#       打印 "NOTE: version changed" = 版本未收敛: 再Build一次固件、再跑
#       export, 直到无NOTE(定宽字面量两轮必收敛)。
#
#   ② build   编译封包一个SD应用:
#       python tools/sdapp/sdapp.py build src/sd/apps/<名> --name "图标名"
#       交叉编译 <应用目录>/*.c -> 用 app.ld + fw_exports.ld 链接到
#       0x20000000(直接调固件函数真身, 零重定位) -> 封64字节AppHeader ->
#       <应用目录>/<目录名>.bin。产物用 tools/netpush/net_push.py 推进板卡 /apps。
#
# 完整流程(见 src/docs/应用开发指南 场景A/B):
#   Vitis Build固件 -> sdapp export(至无NOTE) -> sdapp build各应用
#   -> tools/netpush/net_push.py 推送安装
#
# 产物归属:
#   fw_exports.ld / fw_exports.json   本目录, export重写(固件Build后必变)
#   src/sd/apps/<名>/<名>.bin            应用目录, build产出
#   src/sd/apps/<名>/build/              应用编译中间件(.o/.elf/.map, 可删)
# =============================================================================
import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent     # <固件组件>/tools/sdapp
TOOLS = HERE.parent                        # tools/
APP = TOOLS.parent                         # 固件组件 cam_lcd_hdmi_servo_net
WS = APP.parent                            # Vitis工作区(平台在其下)
SRC = APP / "src"
DEFAULT_MAP = APP / "build" / "fw.map"
DEFAULT_TOOLCHAIN = (r"D:/software/AMDDesign/2025.2/Vitis/gnu/aarch32/nt/"
                     r"gcc-arm-none-eabi/bin/arm-none-eabi-")

#lv_conf.h的LV_ASSERT_HANDLER_INCLUDE指向BSP头: 只需编译期可见, 不链BSP
#默认取本工程平台(FreeRtos_Platform)的BSP导出(随工程走, 不写死盘符路径)
DEFAULT_BSP_INC = str(WS / "FreeRtos_Platform" / "export"
                      / "FreeRtos_Platform" / "sw"
                      / "freertos_ps7_cortexa9_0" / "include")


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit("command failed: %s\n%s\n%s"
                 % (" ".join(str(c) for c in cmd), r.stdout, r.stderr))
    return r.stdout


# =============================================================================
# ① export —— 固件符号自动导出(原fw_exports_gen.py)
# =============================================================================
SYMBOL_RE = re.compile(r"^\s+0x([0-9a-fA-F]{8,16})\s+([A-Za-z_][A-Za-z0-9_.]*)\s*$")
#固件有效地址域(DDR 1GB; 排除绝对小地址的杂项)
ADDR_MIN, ADDR_MAX = 0x00100000, 0x40000000


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_map(map_path: Path):
    symbols = {}          #name -> addr
    conflicts = []
    with map_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = SYMBOL_RE.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            name = m.group(2)
            if not (ADDR_MIN <= addr < ADDR_MAX):
                continue
            if name in symbols:
                if symbols[name] != addr:
                    conflicts.append(name)   #同名多址(局部静态重命名): 保留首见
                continue
            symbols[name] = addr
    return symbols, conflicts


#ELF符号表行(readelf -sW): Num: Value Size Type Bind Vis Ndx Name
RE_SYMTAB = re.compile(
    r"^\s*\d+:\s+([0-9a-fA-F]{8,16})\s+(\d+)\s+(\S+)\s+\S+\s+\S+\s+(\S+)\s+(\S.*)$")


def apply_elf_modes(symbols: dict, map_path: Path, elf_path):
    """map文本不带ARM/Thumb模式位: newlib的memset/expf等是Thumb函数, 真实
    入口=奇地址(ELF st_value低位)。若照map偶地址导出, 模块veneer的bx会以
    ARM态进入Thumb代码, 当场乱执行(2026-10-24 wd_face在memset处冻结的根因)。
    用ELF符号表st_value原样覆盖FUNC符号地址(Thumb位天然保留), ld据此生成
    正确的interwork veneer。返回被校正为奇地址的符号名列表。"""
    if elf_path is None:
        elfs = sorted(map_path.parent.glob("*.elf"))
        if not elfs:
            sys.exit("map同目录找不到*.elf —— Thumb位校正需ELF符号表, "
                     "用--elf指定")
        elf_path = elfs[0]
    out = run([DEFAULT_TOOLCHAIN + "readelf", "-sW", str(elf_path)])
    thumb_fixed = []
    for line in out.splitlines():
        m = RE_SYMTAB.match(line)
        if not m:
            continue
        val, typ, ndx, name = m.group(1), m.group(3), m.group(4), m.group(5)
        if typ != "FUNC" or ndx == "UND":
            continue
        name = name.strip()
        if name not in symbols:
            continue
        st = int(val, 16)
        if st != symbols[name]:
            symbols[name] = st
            if st & 1:
                thumb_fixed.append(name)
    return thumb_fixed


def cmd_export(args):
    map_path = Path(args.map) if args.map else DEFAULT_MAP
    if not map_path.exists():
        sys.exit("map not found: %s (先在Vitis里Build固件)" % map_path)

    symbols, conflicts = parse_map(map_path)
    if not symbols:
        sys.exit("no symbols parsed - map format changed?")
    thumb_fixed = apply_elf_modes(symbols, map_path,
                                  Path(args.elf) if args.elf else None)

    lines = sorted("%s=%08x" % (n, symbols[n]) for n in symbols)
    version = fnv1a64("\n".join(lines).encode())

    ld_path = HERE / "fw_exports.ld"
    with ld_path.open("w", encoding="utf-8") as f:
        f.write("/* 自动生成: tools/sdapp/sdapp.py export —— 请勿手改\n"
                " * 固件符号导出(全仓库), SD应用链接时 -T 本文件\n"
                " * 版本: 0x%016x (%d符号) */\n" % (version, len(symbols)))
        f.write("__fw_exports_version = 0x%016x;\n" % version)
        for n in sorted(symbols):
            f.write("%s = 0x%08x;\n" % (n, symbols[n]))

    json_path = HERE / "fw_exports.json"
    json_path.write_text(json.dumps(
        {"version": version, "count": len(symbols)}), encoding="utf-8")

    hdr_path = SRC / "sw" / "app" / "fw_version_gen.h"
    prev = None
    if hdr_path.exists():
        for ln in hdr_path.read_text(encoding="utf-8").splitlines():
            if ln.startswith("#define FW_EXPORTS_VERSION"):
                tok = ln.split("0x")[1].rstrip("UL")
                try:
                    prev = int(tok, 16)
                except ValueError:
                    pass
    with hdr_path.open("w", encoding="utf-8") as f:
        f.write("/* 自动生成: tools/sdapp/sdapp.py export —— 请勿手改/勿入应用构建\n"
                " * 固件内嵌的符号导出版本(与fw_exports.json同步)\n"
                " * 取64位版本的低32位: 头字段uint32; 定宽保证重编译收敛 */\n"
                "#ifndef FW_VERSION_GEN_H\n#define FW_VERSION_GEN_H\n"
                "#define FW_EXPORTS_VERSION 0x%08xU\n#endif\n"
                % (version & 0xFFFFFFFF))

    print("exported %d symbols, version 0x%016x" % (len(symbols), version))
    print("  -> %s" % ld_path)
    print("  -> %s" % json_path)
    print("  -> %s" % hdr_path)
    if prev is not None and prev != (version & 0xFFFFFFFF):
        print("NOTE: version changed %08x -> %08x, 需再编译一次固件并重跑export"
              % (prev, version & 0xFFFFFFFF))
    if thumb_fixed:
        print("thumb-mode fixed %d syms: %s%s"
              % (len(thumb_fixed), ", ".join(thumb_fixed[:8]),
                 "..." if len(thumb_fixed) > 8 else ""))
    if conflicts:
        print("WARNING %d duplicate-name symbols kept first-seen: %s..."
              % (len(conflicts), ", ".join(conflicts[:5])))


# =============================================================================
# ② build —— SD应用编译+链接+封包(原build_app.py)
# =============================================================================
MAGIC = b"ZAPP"
HDR_SIZE = 64
HDR_FMT = "<4sIIII16sIII16x"       # 4+4+4+4+4+16+4+4+4+16 = 64


def cmd_build(args):
    appdir = Path(args.appdir).resolve()
    if not appdir.is_dir():
        sys.exit("应用目录不存在: %s" % appdir)

    json_path = HERE / "fw_exports.json"
    if not json_path.exists():
        sys.exit("缺少 %s —— 先跑: python tools/sdapp/sdapp.py export"
                 % json_path)
    ver = json.loads(json_path.read_text(encoding="utf-8"))["version"]

    sources = sorted(appdir.glob("*.c"))
    if not sources:
        sys.exit("no .c files in %s" % appdir)

    #产物默认按目录名命名(snake.bin/myapp.bin): 不同应用的bin不互相覆盖;
    #网口推送时板上文件名即取它; 需要别名时用-o覆盖
    out = Path(args.out) if args.out else appdir / (appdir.name + ".bin")
    build = appdir / "build"
    build.mkdir(exist_ok=True)
    prefix = args.toolchain

    common = ["-mcpu=cortex-a9", "-mfpu=neon-vfpv3", "-mfloat-abi=hard",
              "-O2", "-Wall", "-ffreestanding",
              "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
              "-I" + str(SRC),
              "-I" + str(SRC / "sw" / "impl" / "lvgl"),
              "-I" + str(SRC / "sw" / "impl" / "lvgl" / "lvgl"),
              "-isystem", args.bsp_include]

    objs = []
    for src in sources:
        obj = build / (src.stem + ".o")
        run([prefix + "gcc"] + common + ["-c", str(src), "-o", str(obj)])
        objs.append(obj)

    elf = build / "app.elf"
    run([prefix + "gcc",
         "-mcpu=cortex-a9", "-mfpu=neon-vfpv3", "-mfloat-abi=hard",
         "-nostdlib", "-nostartfiles",
         "-Wl,-T," + str(HERE / "app.ld"),
         "-Wl,-T," + str(HERE / "fw_exports.ld"),
         "-Wl,--gc-sections", "-Wl,-Map," + str(build / "app.map"),
         "-lgcc",
         "-o", str(elf)] + [str(o) for o in objs])

    # bss的VMA与大小(节表): 加载器按VMA偏移清零(不假设紧贴数据尾)
    hdr_txt = run([prefix + "objdump", "-h", str(elf)])
    bss_size = 0
    bss_off = 0
    last_end = 0x20000000
    for ln in hdr_txt.splitlines():
        parts = ln.split()
        # idx name size vma lma filesz align
        if len(parts) >= 7 and parts[1] in (".text", ".rodata", ".data"):
            last_end = max(last_end, int(parts[4], 16) + int(parts[2], 16))
        if len(parts) >= 7 and parts[1] == ".bss":
            bss_size = int(parts[2], 16)
            bss_off = int(parts[3], 16) - 0x20000000
    if bss_size == 0:
        print("note: no .bss section")

    # 入口地址必须是APP_LOAD_BASE(app_boot在.app_entry_first)
    nm = run([prefix + "nm", str(elf)])
    entry = None
    for ln in nm.splitlines():
        parts = ln.split()
        if len(parts) == 3 and parts[2] == "app_boot":
            entry = int(parts[0], 16)
    if entry != 0x20000000:
        sys.exit("app_boot @0x%x, must be 0x20000000 "
                 "(section .app_entry_first missing?)" % (entry or 0))

    if bss_off < (last_end - 0x20000000):
        sys.exit(".bss VMA overlaps loadable sections? bss@0x%x" % bss_off)

    raw = build / "app.raw"
    run([prefix + "objcopy", "-O", "binary", "--strip-unneeded",
         str(elf), str(raw)])
    body = raw.read_bytes()

    name = args.name.encode("utf-8")[:15]
    total = HDR_SIZE + len(body)
    hdr = struct.pack(HDR_FMT, MAGIC, HDR_SIZE, ver & 0xFFFFFFFF,
                      len(body), bss_size, name, 0, total, bss_off)
    out.write_bytes(hdr + body)

    print("app: %s" % out)
    print("  name=%s version=0x%08x code=%d bss=%d@off+%d total=%d"
          % (args.name, ver & 0xFFFFFFFF, len(body), bss_size, bss_off,
             total))
    print("  -> 推送安装: python tools/netpush/net_push.py %s" % out)


# =============================================================================
def main():
    ap = argparse.ArgumentParser(
        description="SD应用开发套件: export(固件符号导出) + build(应用编译封包)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    pe = sub.add_parser("export", help="固件Build后: map->符号表+版本号")
    pe.add_argument("--map", default=None,
                    help="fw.map路径(默认 build/fw.map)")
    pe.add_argument("--elf", default=None,
                    help="固件ELF路径(默认map同目录第一个*.elf, 校正Thumb位)")
    pe.set_defaults(func=cmd_export)

    pb = sub.add_parser("build", help="编译封包一个SD应用 -> <名>.bin")
    pb.add_argument("appdir", help="应用目录(如 src/sd/apps/snake)")
    pb.add_argument("--name", required=True,
                    help="桌面图标名(<=15字节UTF-8)")
    pb.add_argument("-o", "--out", default=None,
                    help="输出bin(默认<应用目录>/<目录名>.bin)")
    pb.add_argument("--toolchain", default=DEFAULT_TOOLCHAIN)
    pb.add_argument("--bsp-include", default=DEFAULT_BSP_INC)
    pb.set_defaults(func=cmd_build)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
