"""把 models_sd/<短名>/ 经 TFTP 推到板子。协议与板端 net_push.py 一致。

板 IP 192.168.1.10，UDP 69。板上要先打开「文件传输」。只推不拉。
图片推到推理应用读取的 0:/photos/，格式是 24 位 BMP。
"""
from __future__ import annotations

import hashlib
import socket
import struct
import sys
import tempfile
import time
from pathlib import Path

DEFAULT_IP = "192.168.1.10"
DEFAULT_PORT = 69
OP_WRQ, OP_DATA, OP_ACK, OP_ERROR = 2, 3, 4, 5
BLK = 512
TMO_PKT = 1.0
RETRY_PKT = 15
RETRY_WRQ = 6
BOARD_FILES = ("model.param.bin", "model.bin", "labels.txt", "model.mod")


def _err_text(pkt: bytes) -> str:
    if len(pkt) >= 4 and struct.unpack("!H", pkt[:2])[0] == OP_ERROR:
        return pkt[4:].decode("ascii", "replace").strip("\x00") or "code %d" % struct.unpack("!H", pkt[2:4])[0]
    return pkt.hex()


def _ignore_udp_reset(sock: socket.socket) -> None:
    """Windows 会把板子的“端口没开”收成 WinError 10054。关掉后按超时重试。"""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        from ctypes import wintypes

        flag = ctypes.c_ulong(0)
        returned = wintypes.DWORD()
        ctypes.windll.ws2_32.WSAIoctl(
            sock.fileno(),
            0x9800000C,
            ctypes.byref(flag),
            ctypes.sizeof(flag),
            None,
            0,
            ctypes.byref(returned),
            None,
            None,
        )
    except Exception:
        return


def _recv_pkt(sock: socket.socket):
    try:
        return sock.recvfrom(4 + BLK + 64)
    except socket.timeout:
        return None
    except ConnectionResetError:
        return None
    except OSError as exc:
        if getattr(exc, "winerror", None) == 10054:
            return None
        raise


def push_one(ip: str, port: int, local: Path, remote_path: str) -> None:
    local = Path(local)
    size = local.stat().st_size
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(TMO_PKT)
    _ignore_udp_reset(sock)
    try:
        from board_net import source_ip_for_board

        source = source_ip_for_board(ip)
        if not source:
            raise ConnectionError(
                "本机没有与板子 %s 同网段的地址，数据包发不出去。请先配置网口。" % ip
            )
        try:
            sock.bind((source, 0))
        except OSError as exc:
            raise ConnectionError("无法从 %s 向板子发包：%s" % (source, exc)) from exc
        wrq = struct.pack("!H", OP_WRQ) + remote_path.encode("ascii") + b"\x00octet\x00"
        server = None
        for _ in range(RETRY_WRQ):
            sock.sendto(wrq, (ip, port))
            got = _recv_pkt(sock)
            if got is None:
                continue
            pkt, addr = got
            op, blk = struct.unpack("!HH", pkt[:4])
            if op == OP_ERROR:
                raise RuntimeError("板端拒绝: %s" % _err_text(pkt))
            if op == OP_ACK and blk == 0:
                server = addr
                break
        if server is None:
            raise ConnectionError(
                "板子没有接收文件。网线连通只表示地址能到，UDP 69 只在「文件传输」里监听。"
                "请在板上打开「文件传输」，停在这个画面后再下发。"
            )
        block = 1
        with local.open("rb") as handle:
            while True:
                chunk = handle.read(BLK)
                pkt = struct.pack("!HH", OP_DATA, block) + chunk
                acked = False
                for _ in range(RETRY_PKT):
                    sock.sendto(pkt, server)
                    got = _recv_pkt(sock)
                    if got is None:
                        continue
                    rpkt, _ = got
                    rop, rblk = struct.unpack("!HH", rpkt[:4])
                    if rop == OP_ERROR:
                        raise RuntimeError("板端失败: %s" % _err_text(rpkt))
                    if rop == OP_ACK and rblk == block:
                        acked = True
                        break
                if not acked:
                    raise ConnectionError(
                        "传到第 %d 块时板子停止应答。请保持「文件传输」开着，再重新下发。" % block
                    )
                if len(chunk) < BLK:
                    break
                block = (block + 1) & 0xFFFF
        sock.settimeout(max(2.0, min(15.0, size / 400e3)))
        try:
            got = _recv_pkt(sock)
            if got is None:
                return
            rpkt, _ = got
            if len(rpkt) >= 4 and struct.unpack("!H", rpkt[:2])[0] == OP_ERROR:
                raise RuntimeError("板端落盘失败: %s" % _err_text(rpkt))
        except socket.timeout:
            pass
    finally:
        sock.close()


def push_board_dir(sd_dir: Path | str, board_folder: str, ip: str = DEFAULT_IP, port: int = DEFAULT_PORT) -> list[str]:
    sd_dir = Path(sd_dir)
    name = (board_folder or "").strip()
    if not name.isascii() or not name or ".." in name:
        raise ValueError("板上目录名必须是短 ASCII")
    remote_dir = "0:/models/%s" % name
    if len(remote_dir + "/model.param.bin") >= 48:
        raise ValueError("路径超过板端 48 字节缓冲: %s" % remote_dir)
    sent = []
    for filename in BOARD_FILES:
        local = sd_dir / filename
        if not local.is_file():
            raise FileNotFoundError("缺少 %s" % local)
        if not filename.isascii():
            raise ValueError("文件名含非 ASCII: %s" % filename)
        push_one(ip, int(port), local, "%s/%s" % (remote_dir, filename))
        sent.append(filename)
        time.sleep(0.2)
    return sent


PHOTO_MAX_W = 1280
PHOTO_MAX_H = 720
_PHOTO_PREFIX = "0:/photos/"


def photo_remote_name(source: Path | str) -> str:
    """板上文件名只能是 ASCII。中文原名改成 photo_ 加短摘要，避免互相覆盖。"""
    source = Path(source)
    stem = source.stem
    if stem.isascii():
        cleaned = "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in stem).strip("_")
    else:
        cleaned = ""
    if not cleaned:
        digest = hashlib.sha1(source.name.encode("utf-8")).hexdigest()[:6]
        cleaned = "photo_%s" % digest
    room = 47 - len(_PHOTO_PREFIX) - len(".bmp")
    if room < 1:
        raise ValueError("图片目录路径超过板端 48 字节缓冲")
    return cleaned[:room] + ".bmp"


def render_photo_bmp(source: Path | str, dest: Path | str) -> Path:
    """写成推理应用能读的 24 位 BMP。宽超过 1280 或高超过 720 时缩小，宽补成 4 的倍数。"""
    from PIL import Image

    source = Path(source)
    dest = Path(dest)
    resample = getattr(getattr(Image, "Resampling", Image), "LANCZOS", Image.LANCZOS)
    with Image.open(source) as img:
        rgb = img.convert("RGB")
        width, height = rgb.size
        if width <= 0 or height <= 0:
            raise ValueError("图片尺寸无效：%s" % source.name)
        scale = min(PHOTO_MAX_W / width, PHOTO_MAX_H / height, 1.0)
        if scale < 1.0:
            width = max(1, int(round(width * scale)))
            height = max(1, int(round(height * scale)))
            rgb = rgb.resize((width, height), resample)
        if width % 4 != 0:
            width = width - (width % 4)
            if width < 4:
                raise ValueError("图片太窄，无法做成板端要求的宽度：%s" % source.name)
            rgb = rgb.crop((0, 0, width, height))
        if width > 4096 or height > 4096:
            raise ValueError("图片超过板端 4096 像素上限")
        dest.parent.mkdir(parents=True, exist_ok=True)
        rgb.save(dest, format="BMP")
    return dest


def push_photo(source: Path | str, ip: str = DEFAULT_IP, port: int = DEFAULT_PORT) -> str:
    """把一张本地图片转成 BMP，推到 0:/photos/<文件名>.bmp。"""
    source = Path(source)
    if not source.is_file():
        raise FileNotFoundError("找不到图片：%s" % source)
    remote_name = photo_remote_name(source)
    remote = _PHOTO_PREFIX + remote_name
    if len(remote) >= 48 or not remote_name.isascii() or ".." in remote:
        raise ValueError("板上的图片路径不合法：%s" % remote)
    with tempfile.TemporaryDirectory() as tmp:
        dest = Path(tmp) / remote_name
        render_photo_bmp(source, dest)
        push_one(ip, int(port), dest, remote)
    return remote
