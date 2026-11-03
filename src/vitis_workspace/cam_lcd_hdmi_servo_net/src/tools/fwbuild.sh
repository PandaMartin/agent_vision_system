#!/bin/bash
# =============================================================================
# fwbuild —— 固件命令行一键构建守卫
#
# 免疫Vitis IDE两个坑(均为板上踩实的历史):
#   坑1: IDE retarget把src/CMakeLists.txt整文件清空(2026-10起多次发生,
#        报"Error in retargeting the Application")。
#   坑2: 空CMakeLists.configure出的残根build.ninja没有elf目标, 构建报
#        "No project() warning + ninja: no work to do"但"Build Finished
#        successfully"——假成功, ELF根本没编。
#
# 本脚本依次做: 恢复被清空的CMakeLists -> 构建 -> 校验build.ninja确有目标
# -> 报告ELF时间戳。任何一步不对都显式报错, 不产出假成功。
#
# 用法(仓库vitis目录下): bash cam_lcd_hdmi_servo_net/src/tools/fwbuild.sh
# =============================================================================
set -u

BASE="$(cd "$(dirname "$0")/../.." && pwd)"        # .../cam_lcd_hdmi_servo_net
VITIS="$(cd "$BASE/.." && pwd)"
SRC="$BASE/src"
BUILD="$BASE/build"
ELF="$BUILD/cam_lcd_hdmi_servo_net.elf"
CMAKE="D:/software/Cmake/bin/cmake.exe"

#工具链bin进PATH(链接规则的arm-none-eabi-size依赖PATH)
export PATH="/d/software/AMDDesign/2025.2/Vitis/gnu/aarch32/nt/gcc-arm-none-eabi/bin:$PATH"

#1. CMakeLists被IDE清空则先恢复(自定义内容已全部迁出该文件, restore零损失)
if [ ! -s "$SRC/CMakeLists.txt" ]; then
    echo "[fwbuild] CMakeLists.txt为空(IDE清空), git restore恢复"
    git -C "$VITIS" restore cam_lcd_hdmi_servo_net/src/CMakeLists.txt || {
        echo "[fwbuild] 恢复失败, 手工检查"; exit 1; }
fi

#2. 构建(源码无变化时ninja的no work to do属正常)
"$CMAKE" --build "$BUILD" || { echo "[fwbuild] 构建失败"; exit 1; }

#3. 假成功识别: build.ninja必须含elf目标(残根=configure吃过空CMakeLists)
if ! grep -q "cam_lcd_hdmi_servo_net.elf" "$BUILD/build.ninja"; then
    echo "[fwbuild] 异常: build.ninja无elf目标(残根构建树)!"
    echo "          处理: 删除$BUILD后重新configure再编"
    exit 1
fi

if [ ! -f "$ELF" ]; then
    echo "[fwbuild] 异常: $ELF不存在"; exit 1
fi

echo "[fwbuild] 完成: $(stat -c %y "$ELF")"
echo "[fwbuild] $ELF"
