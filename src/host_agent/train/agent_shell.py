"""给上位机 agent 用的项目内命令和读文件。

工作目录固定在仓库根。训练、下发、JTAG 仍走专用工具。
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from repo_paths import project_root

_MAX_OUT = 8000
_BLOCKED = (
    "shutdown",
    "restart-computer",
    "stop-computer",
    "diskpart",
    "cipher /w",
    "reg delete",
    "reg add",
    "mkfs",
    "format c:",
    "format d:",
    "format e:",
    "format f:",
)


def _root() -> Path:
    return project_root().resolve()


def _inside(path: Path) -> Path:
    root = _root()
    resolved = path.resolve()
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise ValueError("只能访问项目目录里的文件：%s" % resolved) from exc
    return resolved


def _decode(data: bytes | None) -> str:
    raw = data or b""
    if not raw:
        return ""
    text = raw.decode("utf-8", errors="replace")
    if "\ufffd" in text:
        text = raw.decode("gbk", errors="replace")
    return text.replace("\ufffd", "")


def _clip(text: str) -> str:
    text = (text or "").strip()
    if len(text) <= _MAX_OUT:
        return text
    return text[:_MAX_OUT] + "\n… 输出已截断"


def _blocked(command: str) -> str:
    folded = " ".join((command or "").lower().split())
    for item in _BLOCKED:
        if item in folded:
            return "这条命令不执行：%s" % item
    return ""


def _rewrite_python(command: str) -> str:
    stripped = command.strip()
    lower = stripped.lower()
    for prefix in ("python ", "python.exe ", "py -3 ", "py "):
        if not lower.startswith(prefix):
            continue
        rest = stripped[len(prefix):].lstrip()
        exe = sys.executable
        if " " in exe:
            exe = '"%s"' % exe
        if getattr(sys, "frozen", False):
            if rest.startswith("-"):
                return stripped
            return "%s --host-script %s" % (exe, rest)
        return "%s -u %s" % (exe, rest)
    return stripped


def run_project_command(command: str, timeout: int = 60) -> str:
    """在仓库根目录执行一条命令，返回退出码和输出。"""
    text = (command or "").strip()
    if not text:
        return "命令是空的"
    denied = _blocked(text)
    if denied:
        return denied
    try:
        seconds = int(timeout)
    except (TypeError, ValueError):
        seconds = 60
    seconds = max(5, min(seconds, 180))
    text = _rewrite_python(text)
    env = os.environ.copy()
    root = _root()
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    env["FPGA_HOST_ROOT"] = str(root)
    train = str(root / "train")
    prev = env.get("PYTHONPATH") or ""
    env["PYTHONPATH"] = train if not prev else train + os.pathsep + prev
    flags = 0
    if sys.platform == "win32":
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    try:
        proc = subprocess.run(
            text,
            shell=True,
            cwd=str(root),
            capture_output=True,
            timeout=seconds,
            env=env,
            creationflags=flags,
        )
    except subprocess.TimeoutExpired:
        return "命令超过 %d 秒，已停止。\n%s" % (seconds, text)
    except OSError as exc:
        return "无法执行：%s" % exc
    out = _clip(_decode(proc.stdout))
    err = _clip(_decode(proc.stderr))
    lines = ["退出码 %s" % proc.returncode, "目录 %s" % root, "命令 %s" % text]
    if out:
        lines.append(out)
    if err:
        lines.append(err)
    if proc.returncode != 0 and not out and not err:
        lines.append("命令没有输出")
    return "\n".join(lines)


def read_project_file(path: str, max_lines: int = 200) -> str:
    """读取仓库内的文本文件。路径相对仓库根。"""
    raw = (path or "").strip().strip('"')
    if not raw:
        return "没有给出文件路径"
    try:
        limit = int(max_lines)
    except (TypeError, ValueError):
        limit = 200
    limit = max(20, min(limit, 400))
    candidate = Path(raw)
    if not candidate.is_absolute():
        candidate = _root() / candidate
    try:
        target = _inside(candidate)
    except ValueError as exc:
        return str(exc)
    if not target.is_file():
        return "找不到文件：%s" % target.relative_to(_root())
    if target.stat().st_size > 1_500_000:
        return "文件太大，请用 run_command 查看其中一段：%s" % target.name
    data = target.read_bytes()
    if b"\x00" in data[:1024]:
        return "这是二进制文件，不直接展示：%s" % target.name
    body = _decode(data)
    rows = body.splitlines()
    shown = rows[:limit]
    rel = target.relative_to(_root())
    head = "文件 %s  共 %d 行" % (rel.as_posix(), len(rows))
    if len(rows) > limit:
        head += "，这里只给出前 %d 行" % limit
    return head + "\n" + "\n".join(shown)
