#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stress_models —— 模型包连续推送压测(诊断版, TFTP)

逐文件推送并记录: 成败/异常类型/耗时 + 每次推送后PC侧到板UDP 69端口的
端点快照(诊断连通性)。复用net_push.push_one的完整TFTP协议路径。

用法: python stress_models.py [轮数, 默认8]
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from net_push import push_one  # noqa: E402

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "src", "sd", "models", "mnv2_ssdlite_voc"))
FILES = ["labels.txt", "model.bin", "model.mod", "model.param.bin"]
REMOTE_DIR = "0:/models/mnv2_ssdlite_voc"
IP = "192.168.1.10"
PORT = 69


def udp_endpoints():
    out = subprocess.run(["netstat", "-ano", "-p", "udp"],
                         capture_output=True).stdout
    text = out.decode("gbk", errors="replace")   # Windows中文环境netstat是GBK
    rows = [l.split() for l in text.splitlines()
            if ":69 " in l + " " and len(l.split()) >= 3]
    return ",".join("%s" % r[1] for r in rows) or "-"


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    ok = fail = 0
    print("baseline udp: %s" % udp_endpoints())
    t_all = time.time()
    for r in range(1, rounds + 1):
        for f in FILES:
            local = os.path.join(SRC, f)
            t0 = time.time()
            try:
                push_one(IP, PORT, local, "%s/%s" % (REMOTE_DIR, f),
                         log=lambda s: None)
                ok += 1
                res = "OK   %5.1fs" % (time.time() - t0)
            except Exception as e:
                fail += 1
                res = "FAIL %4.1fs %s: %s" % (time.time() - t0,
                                              type(e).__name__, e)
            time.sleep(0.2)
            print("R%02d %-16s %s | udp: %s"
                  % (r, f, res, udp_endpoints()), flush=True)
            if fail >= 8:
                print("** 连续失败过多, 提前终止")
                break
        if fail >= 8:
            break
    print("TOTAL ok=%d fail=%d (%.0fs)"
          % (ok, fail, time.time() - t_all))
    sys.exit(0 if fail == 0 else 1)


if __name__ == "__main__":
    main()
