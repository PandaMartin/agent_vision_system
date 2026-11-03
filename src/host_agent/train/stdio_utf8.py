"""让打包后的训练进程用 UTF-8 写日志。

控制台代码页是 GBK 时，路径里的中文会被界面按 UTF-8 读成替换符，导出就找不到 train_info.json。
"""
from __future__ import annotations

import sys


def force_utf8_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        if stream is None:
            continue
        reconfigure = getattr(stream, "reconfigure", None)
        if not reconfigure:
            continue
        try:
            reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


force_utf8_stdio()
