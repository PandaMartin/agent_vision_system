"""定位 ncnn 命令行工具（ncnn2table / ncnn2int8 / ncnn2mem）。

优先使用仓库 prebuilt/ncnn。没有时再看 FPGA_NCNN_ROOT 和本机的 fpga_ncnn 目录。
"""
from __future__ import annotations

import os
from pathlib import Path

from common import PROJECT_ROOT

PREBUILT_NCNN = PROJECT_ROOT / "prebuilt" / "ncnn"


def find_ncnn_root() -> Path | None:
    if (PREBUILT_NCNN / "ncnn2mem.exe").is_file():
        return PREBUILT_NCNN
    env = os.environ.get("FPGA_NCNN_ROOT", "").strip()
    candidates = []
    if env:
        candidates.append(Path(env))
    candidates.extend(
        [
            Path(r"F:\AWork\fpga_ncnn"),
            PROJECT_ROOT.parent.parent / "fpga_ncnn",
            PROJECT_ROOT.parent / "fpga_ncnn",
        ]
    )
    seen = set()
    for path in candidates:
        key = str(path.resolve()) if path.exists() else str(path)
        if key in seen:
            continue
        seen.add(key)
        if (path / "docs" / "REPRODUCE.md").exists() or (path / "third_party" / "ncnn").exists():
            return path
        if (path / "ncnn2mem.exe").is_file():
            return path
    return None


def _first_existing(paths: list[Path]) -> Path | None:
    for path in paths:
        if path.is_file():
            return path
    return None


def ncnn2table_exe(root: Path | None = None) -> Path | None:
    root = root or find_ncnn_root()
    if root is None:
        return None
    return _first_existing(
        [
            root / "ncnn2table.exe",
            root / "build_host" / "tools" / "quantize" / "ncnn2table.exe",
        ]
    )


def ncnn2int8_exe(root: Path | None = None) -> Path | None:
    root = root or find_ncnn_root()
    if root is None:
        return None
    return _first_existing(
        [
            root / "ncnn2int8.exe",
            root / "build_host" / "tools" / "quantize" / "ncnn2int8.exe",
        ]
    )


def ncnn2mem_exe(root: Path | None = None) -> Path | None:
    root = root or find_ncnn_root()
    if root is None:
        return None
    return _first_existing(
        [
            root / "ncnn2mem.exe",
            root / "tests" / "ncnn2mem.exe",
            root / "build_host" / "tools" / "ncnn2mem.exe",
        ]
    )


def bin2c_py(root: Path | None = None) -> Path:
    local = Path(__file__).resolve().parent / "bin2c.py"
    if local.exists():
        return local
    root = root or find_ncnn_root()
    if root:
        remote = root / "scripts" / "bin2c.py"
        if remote.exists():
            return remote
    return local
