#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""repo_integrity_check.py —— 提交工程包完整性校验
用法: python repo_integrity_check.py <仓库根目录>
布局: 同时支持两种布局——
  A) 官方推荐结构（本仓库）: src/vivado_project/<工程>/*.xpr + src/vitis_workspace + board/ + ...
  B) 旧扁平结构: 根下直接放 .xpr + vitis/（历史提交包）
检查: 关键产物存在性 / ip_repo 引用有效性 / 文件名 ASCII 合规（官方要求）
退出码: 0=无致命缺失, 1=存在致命缺失
"""
import sys, os, re

def _find_vivado_root(root):
    """返回 .xpr 所在目录（两种布局）。"""
    for cand in [root]:
        if any(f.endswith(".xpr") for f in os.listdir(cand) if os.path.isfile(os.path.join(cand, f))):
            return cand
    vp = os.path.join(root, "src", "vivado_project")
    if os.path.isdir(vp):
        for dp, dns, fns in os.walk(vp):
            if any(f.endswith(".xpr") for f in fns):
                return dp
    return None

def _find_vitis_root(root):
    """返回 vitis 工作区目录（两种布局）。"""
    for cand in [os.path.join(root, "vitis"), os.path.join(root, "src", "vitis_workspace")]:
        if os.path.isdir(cand):
            return cand
    return None

def check(root):
    problems, warns = [], []

    # 0) 官方推荐结构顶层目录
    for d in ["src", "sim", "build", "board", "data", "skill", "report"]:
        if os.path.isdir(os.path.join(root, d)):
            print("[OK] 官方结构目录:", d)
        else:
            warns.append("[结构] 缺少官方推荐目录 %s/（若采用其他组织方式，README 须有目录对照）" % d)
    for f in ["README.md", "skill/README.md"]:
        if not os.path.exists(os.path.join(root, f)):
            problems.append("[致命] 缺失: " + f)

    vroot = _find_vivado_root(root)
    if not vroot:
        problems.append("[致命] 未找到 Vivado 工程（.xpr）")
        vroot = root  # 继续检查其余项
    entries = os.listdir(vroot)

    def need(rel, tag="重要", base=None):
        p = os.path.join(base or vroot, rel)
        if not os.path.exists(p):
            (problems if tag == "致命" else warns).append("[%s] 缺失: %s" % (tag, os.path.relpath(p, root)))
        return os.path.exists(p)

    # 1) 关键产物（相对 Vivado 工程根）
    xpr = next((f for f in entries if f.endswith(".xpr")), None)
    runs = next((d for d in entries if d.endswith(".runs")), None)
    if xpr:
        print("[OK] Vivado 工程:", os.path.relpath(os.path.join(vroot, xpr), root))
        need("ip_repo", "致命")
    if runs:
        for f in ["system_wrapper.bit", "system_wrapper_routed.dcp",
                  "system_wrapper_timing_summary_routed.rpt",
                  "system_wrapper_utilization_placed.rpt",
                  "system_wrapper_route_status.rpt", "system_wrapper_power_routed.rpt"]:
            need(os.path.join(runs, "impl_1", f))
    else:
        problems.append("[致命] 无 .runs 目录（无实现报告）")
    srcs = next((d for d in entries if d.endswith(".srcs")), None)
    if srcs:
        need(os.path.join(srcs, "constrs_1", "new", "pin.xdc"))
        bd = None
        for dp, dns, fns in os.walk(os.path.join(vroot, srcs)):
            if "system.bd" in fns:
                bd = os.path.relpath(os.path.join(dp, "system.bd"), root)
                break
        if bd:
            print("[OK] BD 源:", bd)
        else:
            problems.append("[致命] 未找到 system.bd")

    # XSA / 上板下载组 / 实测记录（官方结构在 board/，旧结构在根）
    if not need("system_wrapper.xsa", "重要", vroot):
        need(os.path.join("board", "system_wrapper.xsa"))
    need(os.path.join("board", "firmware", "system_wrapper.bit"), "重要", root)
    need(os.path.join("board", "firmware", "fsbl.elf"), "重要", root)
    need(os.path.join("board", "firmware", "cam_lcd_hdmi_servo_net.elf"), "重要", root)
    need(os.path.join("board", "measured_outputs.md"), "重要", root)
    need(os.path.join("build", "impl_summary.md"), "重要", root)

    # vitis 工作区
    vit = _find_vitis_root(root)
    if vit:
        for rel in [os.path.join("cam_lcd_hdmi_servo_net", "src", "docs", "app_dev_guide.md"),
                    os.path.join("cam_lcd_hdmi_servo_net", "tools", "bsp_net_check.py"),
                    os.path.join("cam_lcd_hdmi_servo_net", "tools", "netpush", "net_push.py"),
                    "FreeRtos_Platform"]:
            p = os.path.join(vit, rel)
            (print("[OK] ", os.path.relpath(p, root)) if os.path.exists(p)
             else problems.append("[致命] 缺失: " + os.path.relpath(os.path.join(vit, rel), root)))
    else:
        problems.append("[致命] 无 vitis 工作区")

    # 2) ip_repo 引用有效性
    if xpr:
        txt = open(os.path.join(vroot, xpr), encoding="utf-8", errors="ignore").read()
        for r in sorted(set(re.findall(r'\$PPRDIR/([^"<]+)', txt))):
            p = os.path.normpath(os.path.join(vroot, r))
            if os.path.exists(p):
                print("[OK] ip_repo 引用有效:", r)
            else:
                problems.append("[致命] ip_repo 引用指向包外/不存在: " + r +
                                "（请将 ip_repo 随包提交，并在工程 Settings→IP→Repository 中修正）")

    # 3) 文件名 ASCII 合规（官方：目录名与文件名须纯英文）
    bad = []
    for dp, dns, fns in os.walk(root):
        if ".git" in dp:
            continue
        for n in dns + fns:
            if not re.fullmatch(r"[A-Za-z0-9_\-\. ]+", n):
                bad.append(os.path.relpath(os.path.join(dp, n), root))
        if len(bad) >= 60:
            break
    if bad:
        warns.append("[命名] %d 个非英文命名（官方要求目录名/文件名纯英文，建议整改或提供对照）:" % len(bad))
        for b in bad[:15]:
            warns.append("    " + b)

    print("")
    print("===== 结果 =====")
    for w in warns:
        print(w)
    if problems:
        print("致命问题 %d 项:" % len(problems))
        for p in problems:
            print(" " + p)
        return 1
    print("无致命缺失。")
    return 0

if __name__ == "__main__":
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    sys.exit(check(os.path.abspath(root)))
