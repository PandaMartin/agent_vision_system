"""仓库根目录。源码运行与 PyInstaller onedir 共用。"""
from __future__ import annotations

import os
import sys
from pathlib import Path


def script_argv(script: Path, *args: str) -> list[str]:
    """源码用 python 跑脚本。打包后的 exe 必须带 --host-script，否则会再开一个界面。"""
    import sys

    command = [sys.executable]
    if getattr(sys, "frozen", False):
        command.append("--host-script")
    else:
        command.append("-u")
    command.append(str(script))
    command.extend(str(item) for item in args)
    return command


def project_root() -> Path:
    env = (os.environ.get("FPGA_HOST_ROOT") or "").strip()
    if env:
        return Path(env).expanduser().resolve()
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


def ui_dir() -> Path:
    """界面资源目录。冻结后优先仓库旁的 agent_ui_v2，否则用 _MEIPASS。"""
    root = project_root()
    beside = root / "agent_ui_v2"
    if beside.is_dir():
        return beside
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", None)
        if meipass:
            bundled = Path(meipass) / "agent_ui_v2"
            if bundled.is_dir():
                return bundled
            return Path(meipass)
    return Path(__file__).resolve().parent.parent / "agent_ui_v2"
