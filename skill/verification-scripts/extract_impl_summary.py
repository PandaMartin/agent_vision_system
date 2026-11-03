#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract_impl_summary.py —— 从 Vivado impl_1 报告提取关键指标为 Markdown 表
用法: python extract_impl_summary.py <工程包根目录>
适用: Zynq/任何 Vivado 工程（定位 *.runs/impl_1/ 下的标准报告名）
输出: 打印 Markdown；重定向即可落盘。已对本提交工程包实跑验证。
"""
import sys, os, re

def find_reports(root):
    runs = next((d for d in os.listdir(root) if d.endswith(".runs")), None)
    if not runs:
        sys.exit("未找到 .runs 目录")
    impl = os.path.join(root, runs, "impl_1")
    return {name: os.path.join(impl, name) for name in [
        "system_wrapper_utilization_placed.rpt",
        "system_wrapper_timing_summary_routed.rpt",
        "system_wrapper_route_status.rpt",
        "system_wrapper_power_routed.rpt",
    ] if os.path.exists(os.path.join(impl, name))}

def grep_block(path, pattern, n=1):
    out = []
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            if re.search(pattern, line):
                out.append(line.strip())
                if len(out) >= n:
                    break
    return out

def main(root):
    rpt = find_reports(root)
    print("| 指标 | 数值 | 来源 |")
    print("|---|---|---|")

    u = rpt.get("system_wrapper_utilization_placed.rpt")
    if u:
        for key, label in [("Slice LUTs", "LUT"), ("Slice Registers", "FF"),
                           ("Block RAM Tile", "BRAM"), ("DSPs", "DSP"), ("Bonded IOB", "IOB")]:
            row = grep_block(u, rf"^\| {re.escape(key)}\s*\|")
            if row:
                m = re.match(r"\|[^|]+\|\s*(\d+)\s*\|\s*\d+\s*\|\s*\d+\s*\|\s*(\d+)\s*\|\s*([\d.]+)", row[0])
                if m:
                    print(f"| {label} | {m.group(1)} / {m.group(2)} ({m.group(3)}%) | utilization_placed |")

    t = rpt.get("system_wrapper_timing_summary_routed.rpt")
    if t:
        lines = open(t, encoding="utf-8", errors="ignore").read().splitlines()
        idx = next((i for i, l in enumerate(lines) if "Design Timing Summary" in l), None)
        if idx is not None:
            nums = []
            for l in lines[idx:idx + 20]:
                toks = l.split()
                if len(toks) >= 8 and re.match(r"^-?\d+\.\d+$", toks[0]):
                    nums = toks
                    break
            if len(nums) >= 8:
                print(f"| 建立 WNS/TNS | {nums[0]} ns / {nums[1]} ns（违例端点 {nums[2]}/{nums[3]}） | timing_summary |")
                print(f"| 保持 WHS/THS | {nums[4]} ns / {nums[5]} ns（违例端点 {nums[6]}/{nums[7]}） | timing_summary |")

    r = rpt.get("system_wrapper_route_status.rpt")
    if r:
        fully = grep_block(r, r"fully routed nets")
        errors = grep_block(r, r"nets with routing errors")
        if fully and errors:
            fr = re.search(r":\s*(\d+)", fully[0]).group(1)
            er = re.search(r":\s*(\d+)", errors[0]).group(1)
            print(f"| 布线 | 全布通 {fr} 网 / 错误 {er} | route_status |")

    p = rpt.get("system_wrapper_power_routed.rpt")
    if p:
        tot = grep_block(p, r"Total On-Chip Power")
        if tot:
            w = re.search(r"\|\s*([\d.]+)\s*\|", tot[0].split("|", 2)[2] + "|").group(1) \
                if False else re.search(r"Total On-Chip Power \(W\)\s*\|\s*([\d.]+)", tot[0]).group(1)
            print(f"| 片上总功耗 | {w} W | power |")

if __name__ == "__main__":
    main(os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "."))
