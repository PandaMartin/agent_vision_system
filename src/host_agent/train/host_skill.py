"""上位机 agent 的说明、板卡档案，以及固件符号表 / 字库查询。"""
from __future__ import annotations

import json
import os
import re
from pathlib import Path

from repo_paths import project_root

SKILL_DIR = project_root() / "skills" / "host-agent"
PROFILE_PATH = project_root() / "board_profile.json"
_SYMBOL_LINE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;")
_NOTES = {
    "project": "project.md",
    "fpga": "fpga.md",
    "board": "board.md",
    "apps": "apps.md",
    "shell": "shell.md",
    "ui": "ui.md",
}

_PROFILE_DEFAULTS = {
    "board_name": "18_cam_lcd_hdmi_servo_net",
    "screen_width": 800,
    "screen_height": 480,
    "board_ip": "192.168.1.10",
    "board_port": 69,
    "pc_ip_hint": "192.168.1.100/24",
    "path_limit": 48,
    "board_folder_max": 20,
    "app_folder_max": 16,
    "icon_bytes_max": 15,
    "gui_app_max": 8,
    "model_index_max": 8,
    "model_mod_base": "0x21000000",
    "model_mod_size": "0x00400000",
    "ops_scratch_base": "0x21400000",
    "ops_scratch_size": "0x00800000",
    "photo_max_w": 1280,
    "photo_max_h": 720,
}


def load_profile() -> dict:
    data = dict(_PROFILE_DEFAULTS)
    if PROFILE_PATH.is_file():
        try:
            loaded = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            loaded = {}
        if isinstance(loaded, dict):
            data.update({k: v for k, v in loaded.items() if v is not None})
    return data


def profile_summary() -> str:
    profile = load_profile()
    keys = (
        "board_name", "screen_width", "screen_height", "board_ip", "board_port",
        "pc_ip_hint", "path_limit", "board_folder_max", "app_folder_max",
        "icon_bytes_max", "gui_app_max", "model_index_max",
        "model_mod_base", "model_mod_size", "ops_scratch_base", "ops_scratch_size",
    )
    return " ".join("%s=%s" % (key, profile.get(key)) for key in keys)


def resolve_tools_root(explicit: str | Path | None = None) -> tuple[Path | None, str]:
    """仓库里已有 tools/sdapp 就用仓库的，否则用 FPGA_PROJECT 工程里的 tools。"""
    if explicit:
        path = Path(explicit)
        if (path / "sdapp" / "sdapp.py").is_file():
            return path, "explicit"
        if (path / "tools" / "sdapp" / "sdapp.py").is_file():
            return path / "tools", "explicit"
    repo = project_root() / "tools"
    if (repo / "sdapp" / "sdapp.py").is_file():
        return repo, "repo"
    env = (os.environ.get("FPGA_PROJECT") or "").strip()
    candidates = [Path(env)] if env else []
    try:
        from board_mod import resolve_fpga_root

        found = resolve_fpga_root(None)
        if found is not None:
            candidates.append(found)
    except Exception:
        pass
    for root in candidates:
        tools = root / "tools"
        if (tools / "sdapp" / "sdapp.py").is_file():
            return tools, "fpga"
    return None, ""


def read_host_note(page: str) -> str:
    name = _NOTES.get((page or "").strip().lower())
    if not name:
        return "page 只能是 project、fpga、board、apps、shell 或 ui"
    path = SKILL_DIR / name
    if not path.is_file():
        return "说明不存在: %s" % name
    return path.read_text(encoding="utf-8")


def skill_prompt() -> str:
    path = SKILL_DIR / "SKILL.md"
    body = path.read_text(encoding="utf-8") if path.is_file() else ""
    return body.strip() + "\n\n当前板卡档案：" + profile_summary()


def _match_symbols(text: str, name: str, limit: int = 40) -> tuple[list[str], int, int]:
    needle = (name or "").strip()
    matched: list[str] = []
    total = 0
    hits = 0
    for line in text.splitlines():
        found = _SYMBOL_LINE.match(line.strip())
        if not found:
            continue
        total += 1
        symbol = found.group(1)
        if needle and needle not in symbol:
            continue
        hits += 1
        if len(matched) < limit:
            matched.append("%s = %s" % (symbol, found.group(2)))
    return matched, total, hits


def query_symbols(kind: str = "both", name: str = "", tools_root: str | Path | None = None) -> str:
    """查 fw_exports.ld 和 ui_symbols.txt。缺文件时说明要先导出，不编造地址。"""
    tools, source = resolve_tools_root(tools_root)
    lines: list[str] = []
    if tools is None:
        lines.append("未找到 tools 目录。仓库中没有 tools/sdapp/sdapp.py 时，设置 FPGA_PROJECT 指向含 tools/sdapp/sdapp.py 的工程根。")
        lines.append("不要编造函数地址。")
        return "\n".join(lines)
    lines.append("工具目录来源=%s" % source)
    want = (kind or "both").strip().lower()
    if want not in ("fw", "font", "both"):
        want = "both"
    if want in ("fw", "both"):
        ld = tools / "sdapp" / "fw_exports.ld"
        meta = tools / "sdapp" / "fw_exports.json"
        if not ld.is_file():
            lines.append("固件符号表不存在。在该工具目录执行: python sdapp/sdapp.py export")
            lines.append("不要编造函数地址。")
        else:
            matched, total, hits = _match_symbols(ld.read_text(encoding="utf-8", errors="replace"), name)
            version = ""
            if meta.is_file():
                try:
                    payload = json.loads(meta.read_text(encoding="utf-8"))
                    version = str(payload.get("version") or "")
                except (OSError, json.JSONDecodeError):
                    version = ""
            lines.append("固件符号 %d 个%s" % (total, (" 版本 " + version) if version else ""))
            if name and not matched:
                lines.append("没有名称包含「%s」的符号。" % name.strip())
            elif matched:
                lines.extend(matched)
                if hits > len(matched):
                    lines.append("匹配 %d 条，只列出前 %d 条。把 name 写得更具体。" % (hits, len(matched)))
            else:
                lines.append("未给 name，不展开全部符号。查询例子：name=fw_app_exit")
    if want in ("font", "both"):
        font = tools / "font" / "ui_symbols.txt"
        if not font.is_file():
            lines.append("字库 ui_symbols.txt 不存在。在工具目录执行: python sdapp/sdapp.py export 后仍缺失，则按字库工具重新生成。")
        else:
            text = font.read_text(encoding="utf-8", errors="replace").strip()
            lines.append("字库 %d 个字符" % len(text))
            probe = (name or "").strip()
            if probe:
                missing = [ch for ch in probe if ch not in text and not ch.isascii()]
                if missing:
                    lines.append("这些字不在字库中，不要写上屏幕：" + "".join(dict.fromkeys(missing)))
                else:
                    lines.append("所查非 ASCII 字符都在字库中。")
            else:
                lines.append(text)
    return "\n".join(lines)
