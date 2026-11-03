#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bsp_net_check —— 平台网络配置核对/恢复(背景与细节见 src/docs/平台网络配置记录.md)

lwIP 栈跑在 FreeRtos_Platform 平台的 BSP 里, 有 2 项 UI 参数 + 2 处 libsrc
手改。UI 参数重建自动保留; libsrc 手改在平台"重新生成"时会被冲掉——本脚本
一条命令核对全部, 可选自动恢复。

用法(固件组件根下):
    python tools/bsp_net_check.py          # 只读检查, 打印 PASS/FAIL
    python tools/bsp_net_check.py fix      # 自动恢复两处 libsrc 手改(.bak备份)

fix 之后仍需手工三步(脚本结尾会再提示):
    1) 重编 BSP 库:  ninja 跑 gen_bsp 目录(命令见提示)
    2) 同步产物:     liblwip220.a 与 lwipopts.h -> bsp/ 与 export/ 两份
    3) Vitis Build 应用固件
"""
import io
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # 组件根
PLAT = os.path.join(ROOT, '..', 'FreeRtos_Platform')
BSP  = os.path.join(PLAT, 'ps7_cortexa9_0', 'freertos_ps7_cortexa9_0', 'bsp')
LWIP = os.path.join(BSP, 'libsrc', 'lwip220', 'src', 'lwip-2.2.0')
GEN  = os.path.join(BSP, 'libsrc', 'build_configs', 'gen_bsp')
EXP  = os.path.join(PLAT, 'export', 'FreeRtos_Platform', 'sw',
                    'freertos_ps7_cortexa9_0')

YAML      = os.path.join(BSP, 'bsp.yaml')
TPL       = os.path.join(LWIP, 'contrib', 'ports', 'xilinx', 'include',
                         'lwipopts.h.in')
PHY       = os.path.join(LWIP, 'contrib', 'ports', 'xilinx', 'netif',
                         'xemacpsif_physpeed.c')
GEN_HDR   = os.path.join(GEN, 'include', 'lwipopts.h')
GEN_LIB   = os.path.join(GEN, 'libsrc', 'lwip220', 'src', 'liblwip220.a')
EXP_HDR   = os.path.join(EXP, 'include', 'lwipopts.h')
EXP_LIB   = os.path.join(EXP, 'lib', 'liblwip220.a')

RCV_LINE  = '#define LWIP_SO_RCVTIMEO 1'

results = []


def rec(name, ok, msg):
    results.append(ok)
    print('[%s] %s%s' % ('PASS' if ok else 'FAIL', name,
                         ('  ' + msg) if msg else ''))


def warn(name, msg):
    print('[WARN] %s  %s' % (name, msg))


def read(p):
    with io.open(p, encoding='utf-8', errors='replace') as f:
        return f.read()


def yaml_value(key):
    m = re.search(key + r':.*?\n(.*?\n){0,5}', read(YAML))
    if not m:
        return None
    v = re.search(r"value: '?([^'\n]+)'?", m.group(0))
    return v.group(1).strip() if v else None


#---- 各检查项 ----------------------------------------------------------------

def chk_yaml_wnd():
    v = yaml_value('lwip220_tcp_wnd')
    rec('UI参数 lwip220_tcp_wnd == 11680', v == '11680',
        '当前=%s (UI参数, 重建自动保留)' % v)


def chk_yaml_speed():
    v = yaml_value('lwip220_temac_phy_link_speed')
    ok = v == 'CONFIG_LINKSPEED_AUTODETECT'
    rec('UI参数 temac_phy_link_speed == AUTODETECT', ok,
        '当前=%s (勿设CONFIG_LINKSPEED100/1000: 强制速率实测连不上PC)' % v)


def chk_tpl_rcvtimeo():
    ok = RCV_LINE in read(TPL)
    rec('libsrc lwipopts.h.in 含 %s' % RCV_LINE, ok,
        '缺失=收包退回轮询, 速度减半以上')


def chk_phy_timeout():
    vals = re.findall(r'timeout_counter\s*==\s*(\d+)', read(PHY))
    n20 = vals.count('20')
    ok = (n20 >= 4) and ('5' not in vals)
    rec('PHY自协商等待补丁(4处==20秒)', ok,
        '当前=%s (上游是5秒, 千兆自协商常超5s会导致开机无网络)' % vals)


def chk_gen_header():
    ok, miss = True, []
    try:
        h = read(GEN_HDR)
        for pat, what in ((r'#define TCP_WND 11680', 'TCP_WND=11680'),
                          (RCV_LINE, 'LWIP_SO_RCVTIMEO')):
            if not re.search(pat, h):
                ok = False
                miss.append(what)
    except OSError:
        ok, miss = False, ['文件不存在']
    rec('BSP生成配置(gen_bsp/include/lwipopts.h)', ok,
        ('缺: ' + ', '.join(miss)) if miss else '')


def chk_export_sync():
    try:
        h = read(EXP_HDR)
        hdr_ok = (RCV_LINE in h) and ('#define TCP_WND 11680' in h)
        rec('export头文件与BSP一致', hdr_ok, '需从 gen_bsp/include 拷贝')
    except OSError:
        rec('export头文件存在', False, EXP_HDR)
    try:
        stale = os.path.getmtime(EXP_LIB) < os.path.getmtime(GEN_LIB)
        warn('export库时间戳', '落后于gen_bsp, 需拷贝 liblwip220.a'
             if stale else '一致')
    except OSError:
        warn('export库', '文件缺失: %s' % EXP_LIB)


CHECKS = (chk_yaml_wnd, chk_yaml_speed, chk_tpl_rcvtimeo,
          chk_phy_timeout, chk_gen_header, chk_export_sync)


#---- fix: 恢复两处 libsrc 手改 ------------------------------------------------

def fix():
    changed = 0
    if RCV_LINE not in read(TPL):
        shutil.copy2(TPL, TPL + '.bak')
        s = read(TPL)
        anchor = '#cmakedefine TCP_WND @TCP_WND@'
        s = s.replace(anchor, RCV_LINE + '\n' + anchor, 1)
        with io.open(TPL, 'w', encoding='utf-8', newline='') as f:
            f.write(s)
        print('已恢复: lwipopts.h.in 加 %s (备份.bak)' % RCV_LINE)
        changed += 1
    phy = read(PHY)
    if re.search(r'timeout_counter\s*==\s*5', phy):
        shutil.copy2(PHY, PHY + '.bak')
        phy = re.sub(r'timeout_counter\s*==\s*5',
                     'timeout_counter == 20', phy)
        with io.open(PHY, 'w', encoding='utf-8', newline='') as f:
            f.write(phy)
        print('已恢复: xemacpsif_physpeed.c 自协商等待 5s->20s (备份.bak)')
        changed += 1
    if not changed:
        print('两处 libsrc 手改均在, 无需恢复')
        return
    print('\n修复完成, 接下来手工执行:')
    print('  1) 重编BSP库:')
    print(r'     cd vitis\FreeRtos_Platform\ps7_cortexa9_0\freertos_ps7_cortexa9_0'
          r'\bsp\libsrc\build_configs\gen_bsp')
    print('     D:\\software\\AMDDesign\\2025.2\\Vitis\\bin\\ninja.exe')
    print('  2) 同步: gen_bsp/libsrc/lwip220/src/liblwip220.a 与'
          ' gen_bsp/include/lwipopts.h')
    print('     各拷贝到 bsp/lib|include 与 export/.../lib|include 两份')
    print('  3) Vitis Build 应用固件, 上板 ping 192.168.1.10 验证')


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'check'
    if mode == 'fix':
        fix()
        return
    print('平台网络配置核对 (组件根: %s)\n' % ROOT)
    for c in CHECKS:
        try:
            c()
        except OSError as e:
            rec(c.__name__, False, str(e))
        print('')
    bad = results.count(False)
    print('结果: %d/%d 通过' % (len(results) - bad, len(results)))
    if bad:
        print('有 FAIL: 跑 python tools/bsp_net_check.py fix 自动恢复'
              '(仅恢复libsrc两处; UI参数需去Vitis平台设置里核对)')
        sys.exit(1)


if __name__ == '__main__':
    main()
