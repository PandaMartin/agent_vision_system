#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
net_push —— 网口文件推送(板端"文件传输"内置应用的PC侧, 标准TFTP客户端)

用法(固件组件根 vitis/cam_lcd_hdmi_servo_net/ 下):
    python tools/netpush/net_push.py [选项] <文件> [文件 ...]

    选项:
      --ip IP        板端地址(默认 192.168.1.10, 板卡静态IP)
      --port N       UDP端口(默认 69, TFTP标准端口)
      --dir SD目录   目标目录(默认 0:/apps; 模型用 0:/models/<名>)

    示例:
      python tools/netpush/net_push.py snake.bin            # 推SD应用 -> 0:/apps
      python tools/netpush/net_push.py --dir 0:/models/yolo yolo.param yolo.bin

    也可用任意标准tftp客户端推送(裸名落SD根"0:/"):
      tftp -i 192.168.1.10 put snake.bin

前置:
    1. PC网卡(直连网线)配 192.168.1.100 / 255.255.255.0
    2. 板端打开"文件传输"应用(传输只在应用内开放, 桌面态不监听)
    3. 需要Python 3.6+(仅标准库)

协议(标准TFTP/RFC1350, WRQ/octet/512字节锁步):
    PC -> WRQ <SD全路径> octet (UDP 69)
    板 <- ACK(0) (自会话临时端口, 标准TID换口)
    PC -> DATA(块号, <=512字节) <-> 板 ACK(块号) 锁步到尾块
    板收到尾块即ACK(客户端视角传输完成), 落SD在板端异步进行;
    落盘失败时板会补发ERROR(本工具收尾后按体量多听片刻尽力捕获)
    推 /apps 下的应用bin成功后, 板端桌面自动出新图标(无需手动重扫)

注意: TFTP为512字节锁步, 吞吐低于原TCP直流(约数百KB/s量级);
文件尺寸不随WRQ下发(协议不带), 板端传输页收流期间只显示已收字节。
"""

import argparse
import os
import socket
import struct
import sys
import time

DEFAULT_IP = "192.168.1.10"
DEFAULT_PORT = 69
DEFAULT_DIR = "0:/apps"

OP_RRQ, OP_WRQ, OP_DATA, OP_ACK, OP_ERROR = 1, 2, 3, 4, 5
BLK = 512                 # TFTP标准块长(不协商blksize, 兼容所有客户端)
TMO_PKT = 1.0             # 单包等应答超时(超时重发)
RETRY_PKT = 15            # 单包重发预算(15s, 覆盖板端落SD排队等瞬时空闲)
RETRY_WRQ = 6             # WRQ重试预算(板端单会话串行, 忙时需排队)


def die(msg, code=1):
    print("[net_push] 错误: %s" % msg, file=sys.stderr)
    sys.exit(code)


def err_text(pkt):
    """ERROR包 -> 可读文本"""
    if len(pkt) >= 4 and struct.unpack("!H", pkt[:2])[0] == OP_ERROR:
        return pkt[4:].decode("ascii", "replace").strip("\x00") or \
            "code %d" % struct.unpack("!H", pkt[2:4])[0]
    return pkt.hex()


def push_one(ip, port, local, remote_path, log=None, on_progress=None):
    """推送一个文件(TFTP WRQ)。log(text)打一行日志, on_progress(done,total)
    报进度(GUI传回调; CLI走默认打印)。返回True成功, 失败抛异常。"""
    if log is None:
        log = print
    size = os.path.getsize(local)
    log("[%s] %s (%s 字节) -> %s" % (time.strftime("%H:%M:%S"),
        os.path.basename(local), format(size, ","), remote_path))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(TMO_PKT)
    try:
        # 1) WRQ -> 等ACK(0)(板从会话临时端口应答, 后续数据发往该端口)
        wrq = struct.pack("!H", OP_WRQ) + remote_path.encode("ascii") \
            + b"\x00octet\x00"
        server = None
        for _ in range(RETRY_WRQ):
            sock.sendto(wrq, (ip, port))
            try:
                pkt, addr = sock.recvfrom(4 + BLK)
            except socket.timeout:
                continue
            op, blk = struct.unpack("!HH", pkt[:4])
            if op == OP_ERROR:
                raise RuntimeError("板端拒绝: %s" % err_text(pkt))
            if op == OP_ACK and blk == 0:
                server = addr
                break
        if server is None:
            raise ConnectionError("板端无应答(WRQ)")

        # 2) 512字节锁步: DATA(n) -> 等ACK(n) -> 下一块; 超时重发当前块
        t0 = time.time()
        block = 1
        sent = 0
        last_log = -1
        with open(local, "rb") as f:
            while True:
                chunk = f.read(BLK)
                pkt = struct.pack("!HH", OP_DATA, block) + chunk
                for _ in range(RETRY_PKT):
                    sock.sendto(pkt, server)
                    try:
                        rpkt, _ = sock.recvfrom(4 + BLK + 64)
                    except socket.timeout:
                        continue
                    rop, rblk = struct.unpack("!HH", rpkt[:4])
                    if rop == OP_ERROR:
                        raise RuntimeError("板端失败: %s" % err_text(rpkt))
                    if rop == OP_ACK and rblk == block:
                        break          # 本块确认, 下一块
                    # 重复ACK(上块)/杂包: 重发当前块
                else:
                    raise ConnectionError("ACK超时 (block %d)" % block)
                sent += len(chunk)
                if on_progress:
                    on_progress(sent, size)
                if size and sent * 100 // size != last_log \
                        and sent * 100 // size % 5 == 0:
                    dt = time.time() - t0
                    last_log = sent * 100 // size
                    log("    %3d%%  %6.1f KB%s" % (
                        last_log, sent / 1024,
                        ("  %.0f KB/s" % (sent / dt / 1024))
                        if dt > 0.5 else ""))
                if len(chunk) < BLK:   # 尾块(空文件=0字节尾块)已确认
                    break
                block = (block + 1) & 0xFFFF   # u16块号回绕(与板端一致)

        dt = time.time() - t0
        log("    100%%  %.1f KB  平均 %.0f KB/s  %.1fs" % (
            sent / 1024, sent / dt / 1024 if dt > 0 else 0, dt))
        if on_progress:
            on_progress(size, size)

        # 3) 尾ACK后按体量多听片刻: 板端落SD是异步的(先回尾ACK再写盘),
        #    写盘失败板会补发ERROR, 等待时长≈板端写完的时间(尽力捕获;
        #    标准TFTP语义下客户端此刻已可宣布成功, 最终结果以板面为准)
        sock.settimeout(max(2.0, min(15.0, size / 400e3)))
        try:
            rpkt, _ = sock.recvfrom(600)
            if len(rpkt) >= 4 \
                    and struct.unpack("!H", rpkt[:2])[0] == OP_ERROR:
                raise RuntimeError("板端落盘失败: %s" % err_text(rpkt))
        except socket.timeout:
            pass
        log("    完成: 已推送 %s (落SD结果以板面为准)" % remote_path)
        return True
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser(
        description="网口文件推送(板端'文件传输'应用, 标准TFTP)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("前置:")[0])
    ap.add_argument("files", nargs="+", help="要推送的本地文件")
    ap.add_argument("--ip", default=DEFAULT_IP, help="板端IP(默认 %(default)s)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help="UDP端口(默认 %(default)d, TFTP标准端口)")
    ap.add_argument("--dir", default=DEFAULT_DIR,
                    help="SD目标目录(默认 %(default)s)")
    args = ap.parse_args()

    for f in args.files:
        if not os.path.isfile(f):
            die("文件不存在: %s" % f)
        name = os.path.basename(f)
        if not name.isascii():
            die("文件名含非ASCII字符(FatFs长名风险), 请先改名: %s" % name)

    #瞬时网络错误自动重试(板端单会话串行, 前一文件落SD期间WRQ需排队);
    #板端明确拒绝(RuntimeError)不重试——语义错误重推无意义
    RETRIES = 3
    ok = 0
    for f in args.files:
        remote = "%s/%s" % (args.dir.rstrip("/"), os.path.basename(f))
        pushed = False
        for attempt in range(1, RETRIES + 1):
            try:
                if push_one(args.ip, args.port, f, remote):
                    pushed = True
                    ok += 1
                break
            except (ConnectionError, socket.timeout, OSError) as e:
                if attempt < RETRIES:
                    print("[net_push] 瞬时错误(%s), 1.5s后重试(%d/%d)"
                          % (e, attempt, RETRIES - 1), file=sys.stderr)
                    time.sleep(1.5)
                else:
                    print("\n[net_push] 传输失败: %s" % e, file=sys.stderr)
                    print("  请确认: 板端已打开'文件传输'应用, "
                          "PC网卡在192.168.1.x网段", file=sys.stderr)
        if not pushed:
            continue
        time.sleep(0.2)                     #单槽服务器: 文件间稍作间隔

    print("共 %d/%d 个文件推送成功" % (ok, len(args.files)))
    sys.exit(0 if ok == len(args.files) else 1)


if __name__ == "__main__":
    main()
