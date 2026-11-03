"""列出 FPGA 工程 src/sd 里已经编好的应用和模型，并经 TFTP 拷到板子。

只读板工程，不改里面的文件。不下发「板端固件替换」。
板上要先打开「文件传输」。
"""
from __future__ import annotations

import time
from pathlib import Path

from board_apps import parse_app_bin, remote_app_path
from board_mod import resolve_fpga_root
from board_push import BOARD_FILES, push_board_dir, push_one

PATH_LIMIT = 48


def _model_remote_ok(name: str) -> bool:
    remote = "0:/models/%s/model.param.bin" % name
    return bool(name) and name.isascii() and ".." not in name and len(remote) < PATH_LIMIT


def list_fpga_code(fpga_root: str | Path | None = None) -> dict:
    """返回 {root, items, error}。items 是可拷到板子的应用 bin 和模型四文件。"""
    root = resolve_fpga_root(fpga_root)
    if root is None:
        return {"root": "", "items": [], "error": "未找到 FPGA 工程。可设置环境变量 FPGA_PROJECT。"}
    sd = root / "src" / "sd"
    items: list[dict] = []
    apps = sd / "apps"
    if apps.is_dir():
        for child in sorted(apps.iterdir()):
            if not child.is_dir() or child.name.startswith((".", "_")):
                continue
            binary = child / ("%s.bin" % child.name)
            if not binary.is_file():
                continue
            try:
                info = parse_app_bin(binary)
                remote = remote_app_path(binary.name)
            except ValueError:
                continue
            items.append({
                "kind": "app",
                "name": info.get("name") or child.name,
                "folder": child.name,
                "local": str(binary.resolve()),
                "remote": remote,
                "bytes": int(binary.stat().st_size),
                "detail": "桌面应用 → %s" % remote,
            })
    models = sd / "models"
    if models.is_dir():
        for child in sorted(models.iterdir()):
            if not child.is_dir() or child.name.startswith((".", "_")):
                continue
            if not _model_remote_ok(child.name):
                continue
            files = [child / name for name in BOARD_FILES]
            if not all(path.is_file() for path in files):
                continue
            total = sum(path.stat().st_size for path in files)
            items.append({
                "kind": "model",
                "name": child.name,
                "folder": child.name,
                "local": str(child.resolve()),
                "remote": "0:/models/%s/" % child.name,
                "bytes": int(total),
                "detail": "模型四文件 → 0:/models/%s/" % child.name,
            })
    return {"root": str(root), "items": items, "error": ""}


def push_fpga_items(items: list[dict], ip: str, port: int) -> list[str]:
    """把 list_fpga_code 里选出的条目拷到板子。返回已发送的远程路径。"""
    sent: list[str] = []
    for item in items:
        kind = item.get("kind")
        if kind == "app":
            remote = remote_app_path(Path(item["local"]).name)
            push_one(ip, int(port), Path(item["local"]), remote)
            sent.append(remote)
        elif kind == "model":
            name = str(item.get("folder") or item.get("name") or "")
            if not _model_remote_ok(name):
                raise ValueError("模型目录名不能拷到板子: %s" % name)
            push_board_dir(item["local"], name, ip, int(port))
            sent.append("0:/models/%s/" % name)
        else:
            raise ValueError("未知条目: %s" % kind)
        time.sleep(0.2)
    return sent


def remote_file_path(remote_dir: str, filename: str) -> str:
    folder = (remote_dir or "").strip().replace("\\", "/")
    name = (filename or "").strip()
    if not folder.startswith("0:/") or ".." in folder:
        raise ValueError("目标目录必须以 0:/ 开头，且不能含 ..")
    if not name or not name.isascii() or "/" in name or "\\" in name or ".." in name:
        raise ValueError("文件名必须是不含目录的 ASCII：%s" % filename)
    remote = folder.rstrip("/") + "/" + name
    if not remote.isascii() or len(remote) >= PATH_LIMIT:
        raise ValueError("路径超过板端 48 字节缓冲: %s" % remote)
    return remote


def push_local_files(files: list[str | Path], remote_dir: str, ip: str, port: int) -> list[str]:
    sent: list[str] = []
    for raw in files:
        path = Path(raw)
        if not path.is_file():
            raise FileNotFoundError("没有文件 %s" % path)
        remote = remote_file_path(remote_dir, path.name)
        push_one(ip, int(port), path, remote)
        sent.append(remote)
        time.sleep(0.2)
    return sent
