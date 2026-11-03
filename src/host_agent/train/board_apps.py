"""把板上的 SD 应用（ZAPP bin）经 TFTP 下发到 0:/apps/。

应用是桌面图标，不是训练模型。板端打开「文件传输」后，推送完成会自动出图标。
本机新建的应用放在仓库 apps/<目录名>/，用板工程的 sdapp.py 编成 bin 再下发。
"""
from __future__ import annotations

import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from board_mod import resolve_fpga_root, resolve_toolchain
from board_push import push_one

from repo_paths import project_root, script_argv

HOST_APPS = project_root() / "apps"
_FOLDER_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]{0,15}")

MAGIC = b"ZAPP"
HDR_SIZE = 64
HDR_FMT = "<4sIIII16sIII16x"
APPS_REMOTE = "0:/apps"
PATH_LIMIT = 48


def parse_app_bin(path: Path | str) -> dict:
    path = Path(path)
    data = path.read_bytes()
    if len(data) < HDR_SIZE:
        raise ValueError("%s 不是 SD 应用：文件短于 64 字节头" % path.name)
    magic, hdr_size, version, load_size, bss_size, raw_name, entry_off, total, bss_off = struct.unpack(
        HDR_FMT, data[:HDR_SIZE]
    )
    if magic != MAGIC or hdr_size != HDR_SIZE:
        raise ValueError("%s 不是 SD 应用：文件头不是 ZAPP" % path.name)
    if total != len(data) or HDR_SIZE + load_size != len(data):
        raise ValueError("%s 的长度和文件头不一致" % path.name)
    if entry_off != 0:
        raise ValueError("%s 的入口偏移不是 0" % path.name)
    name = raw_name.split(b"\x00", 1)[0].decode("utf-8", "replace") or path.stem
    return {
        "path": str(path.resolve()),
        "file": path.name,
        "name": name,
        "version": int(version),
        "load_size": int(load_size),
        "bss_size": int(bss_size),
        "bss_off": int(bss_off),
        "bytes": len(data),
    }


def remote_app_path(filename: str) -> str:
    name = (filename or "").strip()
    if not name or not name.isascii() or "/" in name or "\\" in name or ".." in name:
        raise ValueError("应用文件名必须是不含目录的 ASCII：%s" % filename)
    remote = "%s/%s" % (APPS_REMOTE, name)
    if len(remote) >= PATH_LIMIT:
        raise ValueError("路径超过板端 48 字节缓冲: %s" % remote)
    return remote


def _scan_app_root(apps: Path, found: list[dict], seen: set[str]) -> None:
    if not apps.is_dir():
        return
    for child in sorted(apps.iterdir()):
        if not child.is_dir() or child.name.startswith((".", "_")):
            continue
        binary = child / ("%s.bin" % child.name)
        if not binary.is_file():
            continue
        key = str(binary.resolve())
        if key in seen:
            continue
        try:
            info = parse_app_bin(binary)
        except ValueError:
            continue
        source = child / "app_main.c"
        info["app_dir"] = str(child.resolve())
        info["source"] = str(source.resolve()) if source.is_file() else ""
        info["editable"] = _is_host_app(child) and source.is_file()
        found.append(info)
        seen.add(key)


def list_board_apps(fpga_root: str | Path | None = None) -> list[dict]:
    """扫描板工程 src/sd/apps 和本机 apps/ 下已经编好的 bin。"""
    found: list[dict] = []
    seen: set[str] = set()
    root = resolve_fpga_root(fpga_root)
    if root is not None:
        _scan_app_root(root / "src" / "sd" / "apps", found, seen)
    _scan_app_root(HOST_APPS, found, seen)
    if HOST_APPS.is_dir():
        listed = {item.get("app_dir") for item in found}
        for child in sorted(HOST_APPS.iterdir()):
            source = child / "app_main.c"
            if not child.is_dir() or not source.is_file() or str(child.resolve()) in listed:
                continue
            meta = _read_meta(child)
            found.append({
                "path": "",
                "file": child.name,
                "name": meta.get("icon") or child.name,
                "version": 0,
                "bytes": 0,
                "load_size": 0,
                "bss_size": 0,
                "bss_off": 0,
                "app_dir": str(child.resolve()),
                "source": str(source.resolve()),
                "editable": True,
            })
    return found


def _is_host_app(path: Path) -> bool:
    try:
        Path(path).resolve().relative_to(HOST_APPS.resolve())
    except ValueError:
        return False
    return True


def _read_meta(app_dir: Path) -> dict:
    path = Path(app_dir) / "app.json"
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def read_app_source(app_dir: Path | str) -> str:
    path = Path(app_dir) / "app_main.c"
    if not path.is_file():
        raise FileNotFoundError("没有源码 %s" % path)
    return path.read_text(encoding="utf-8")


def write_app_source(app_dir: Path | str, source: str) -> Path:
    text = source or ""
    if "app_boot" not in text or ".app_entry_first" not in text:
        raise ValueError("源码必须保留 .app_entry_first 段里的 app_boot，否则板子无法启动这个应用")
    path = Path(app_dir) / "app_main.c"
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def delete_board_app(app_dir: Path | str) -> Path:
    """删除本机新建的应用目录。不删板工程里的贪吃蛇、人脸追踪。"""
    app_dir = Path(app_dir).resolve()
    if not _is_host_app(app_dir) or app_dir == HOST_APPS.resolve():
        raise ValueError("只能删除本机新建的应用")
    if not app_dir.is_dir():
        raise FileNotFoundError("应用目录不存在：%s" % app_dir)
    shutil.rmtree(app_dir)
    return app_dir


def app_icon_name(app_dir: Path | str) -> str:
    app_dir = Path(app_dir)
    icon = str(_read_meta(app_dir).get("icon") or "").strip()
    if icon:
        return icon
    binary = app_dir / ("%s.bin" % app_dir.name)
    if binary.is_file():
        try:
            return parse_app_bin(binary)["name"]
        except ValueError:
            pass
    return app_dir.name


def _font_ok(text: str, fpga_root: Path | None) -> str:
    """屏幕和桌面图标只能用 ASCII，或固件字库里已有的字。"""
    extra = set()
    if fpga_root is not None:
        symbols = fpga_root / "tools" / "font" / "ui_symbols.txt"
        if symbols.is_file():
            extra = set(symbols.read_text(encoding="utf-8", errors="replace"))
    missing = []
    for ch in text:
        if ch.isascii() and 32 <= ord(ch) <= 126:
            continue
        if ch in extra:
            continue
        missing.append(ch)
    if missing:
        raise ValueError("这些字不在板上字库里，图标或标题会是空白：%s" % "".join(dict.fromkeys(missing)))
    return text


def _c_quote(text: str) -> str:
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n" or ch == "\r":
            out.append(" ")
        elif ord(ch) < 32:
            continue
        else:
            out.append(ch)
    return "".join(out)


def create_board_app(folder: str, icon_name: str, title: str, fpga_root: str | Path | None = None) -> Path:
    """在本机 apps/<目录名>/ 写一个带标题和退出按钮的 SD 应用源码。"""
    folder = (folder or "").strip()
    icon_name = (icon_name or "").strip()
    title = (title or icon_name or folder).strip()
    if not _FOLDER_RE.fullmatch(folder):
        raise ValueError("目录名用字母开头，只含字母、数字和下划线，最长 16")
    if not icon_name:
        raise ValueError("请填写桌面图标名")
    if len(icon_name.encode("utf-8")) > 15:
        raise ValueError("桌面图标名最多 15 字节（约 5 个汉字）")
    root = resolve_fpga_root(fpga_root)
    _font_ok(icon_name, root)
    _font_ok(title, root)
    dest = HOST_APPS / folder
    if dest.exists():
        raise ValueError("应用目录已存在：%s" % dest)
    dest.mkdir(parents=True)
    source = '''#include "lvgl.h"
#include "sw/app/ext_app/loader.h"

extern const lv_font_t font_cn_16;

/* 自定义功能写在本文件里，改完后点「编译并下发」。
 * 界面在 app_main 里创建，屏幕 800x480，控件用固定坐标。
 * 周期动作放进 tick_cb，并用 app_timer_create 注册。
 * 退出时调用 fw_app_exit() 后立刻 return。
 * 不要用 printf 或 malloc。汉字必须是固件字库里已有的字。
 * 下面的计数器只是例子，可以整段换成自己的逻辑。 */

static volatile int s_count;
static lv_obj_t* s_count_label;
static lv_timer_t* s_timer;

static void btn_exit_cb(lv_event_t* e)
{
    (void)e;
    fw_app_exit();
}

static void tick_cb(lv_timer_t* t)
{
    (void)t;
    s_count++;
    lv_label_set_text_fmt(s_count_label, "计数 %%d", s_count);
}

void app_main(void)
{
    lv_obj_t* scr = lv_obj_create(0);
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_obj_set_pos(title, 24, 28);
    lv_obj_set_style_text_font(title, &font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00E5FF), 0);
    lv_label_set_text(title, "%s");

    s_count_label = lv_label_create(scr);
    lv_obj_set_pos(s_count_label, 24, 72);
    lv_obj_set_style_text_font(s_count_label, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_count_label, lv_color_white(), 0);
    lv_label_set_text(s_count_label, "计数 0");

    lv_obj_t* btn = lv_button_create(scr);
    lv_obj_set_pos(btn, 24, 120);
    lv_obj_set_size(btn, 88, 36);
    lv_obj_t* ex = lv_label_create(btn);
    lv_obj_set_style_text_font(ex, &font_cn_16, 0);
    lv_obj_set_style_text_color(ex, lv_color_white(), 0);
    lv_label_set_text(ex, "退出");
    lv_obj_center(ex);
    lv_obj_add_event_cb(btn, btn_exit_cb, LV_EVENT_CLICKED, 0);

    lv_screen_load(scr);
    s_timer = app_timer_create(tick_cb, 500, 0);
}

__attribute__((section(".app_entry_first")))
void app_boot(void)
{
    app_main();
}
''' % _c_quote(title)
    (dest / "app_main.c").write_text(source, encoding="utf-8", newline="\n")
    (dest / "app.json").write_text(
        json.dumps({"folder": folder, "icon": icon_name, "title": title}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return dest


def build_board_app(app_dir: Path | str, icon_name: str, fpga_root: str | Path | None = None) -> Path:
    """用板工程 sdapp.py 把应用目录编成 <目录名>.bin。"""
    app_dir = Path(app_dir)
    root = resolve_fpga_root(fpga_root)
    if root is None:
        raise FileNotFoundError("未找到板工程，无法编译应用")
    script = root / "tools" / "sdapp" / "sdapp.py"
    if not script.is_file():
        raise FileNotFoundError("未找到 %s" % script)
    icon_name = (icon_name or "").strip()
    if not icon_name or len(icon_name.encode("utf-8")) > 15:
        raise ValueError("桌面图标名最多 15 字节")
    cmd = script_argv(script, "build", str(app_dir), "--name", icon_name)
    toolchain = resolve_toolchain()
    if not toolchain:
        raise FileNotFoundError("未找到 arm-none-eabi-gcc")
    cmd.extend(["--toolchain", toolchain])
    run_kwargs = dict(
        capture_output=True,
        text=True,
        timeout=180,
        encoding="utf-8",
        errors="replace",
    )
    if sys.platform == "win32":
        run_kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    proc = subprocess.run(cmd, **run_kwargs)
    binary = app_dir / ("%s.bin" % app_dir.name)
    if proc.returncode != 0 or not binary.is_file():
        tail = " | ".join(((proc.stdout or "") + "\n" + (proc.stderr or "")).splitlines()[-8:])
        raise RuntimeError(tail or "应用编译失败")
    return binary


def push_board_app(bin_path: Path | str, ip: str, port: int = 69) -> str:
    info = parse_app_bin(bin_path)
    remote = remote_app_path(info["file"])
    push_one(ip, int(port), Path(info["path"]), remote)
    return remote


_ALLOWED_INCLUDES = {'"lvgl.h"', '"sw/app/ext_app/loader.h"'}
_FORBIDDEN_SOURCE = (
    (r"#\s*include\s*<", "只允许 #include \"lvgl.h\" 和 #include \"sw/app/ext_app/loader.h\""),
    (r"\bprintf\s*\(", "不要用 printf，文字用 lv_label_set_text 或 lv_label_set_text_fmt"),
    (r"\bmalloc\s*\(", "不要用 malloc"),
    (r"\bfree\s*\(", "不要用 free"),
    (r"\blv_timer_create\s*\(", "定时器必须用 app_timer_create，不要用 lv_timer_create"),
    (r"\blv_pct\s*\(", "不要用 lv_pct，坐标和尺寸写整数"),
    (r"\blv_obj_get_(?:width|height)\s*\(", "不要在运行时读取控件宽高"),
    (r"\bstd::", "只写 C，不要写 C++"),
    (r"\b(?:class|namespace|template)\s+", "只写 C，不要写 C++"),
)


def font_symbols(fpga_root: str | Path | None = None) -> str:
    root = resolve_fpga_root(fpga_root)
    if root is None:
        return ""
    path = root / "tools" / "font" / "ui_symbols.txt"
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace").strip()


def validate_app_source(source: str, fpga_root: str | Path | None = None) -> None:
    """按已上板的 snake / 计数器示例检查 AI 写的 app_main.c。"""
    text = source or ""
    if len(text) > 48000:
        raise ValueError("源码超过 48KB")
    if "void app_main" not in text:
        raise ValueError("必须有 void app_main(void)，界面在这里创建")
    if "app_boot" not in text or ".app_entry_first" not in text:
        raise ValueError("必须保留 __attribute__((section(\".app_entry_first\"))) void app_boot(void)，并在里面调用 app_main")
    if "lv_screen_load" not in text:
        raise ValueError("app_main 末尾必须 lv_screen_load(scr)")
    if "fw_app_exit" not in text:
        raise ValueError("必须提供退出按钮，回调里调用 fw_app_exit() 后立刻 return")
    if "static volatile" not in text:
        raise ValueError("必须保留一个会被读到的 static volatile 变量，否则链接时没有 .bss，sdapp 会拒绝封包")
    if re.search(r"app_timer_create\s*\(\s*\d+", text):
        raise ValueError("app_timer_create 的参数顺序是 (回调函数, 毫秒, user_data)，第一个参数不能是数字")
    includes = re.findall(r'#\s*include\s*([<"][^>"]+[>"])', text)
    bad = [item for item in includes if item not in _ALLOWED_INCLUDES]
    if bad:
        raise ValueError("不允许的头文件: %s" % ", ".join(bad))
    if '"lvgl.h"' not in includes or '"sw/app/ext_app/loader.h"' not in includes:
        raise ValueError("必须包含 lvgl.h 和 sw/app/ext_app/loader.h")
    if "font_cn_16" in text and "extern const lv_font_t font_cn_16" not in text:
        raise ValueError("使用 font_cn_16 时必须声明 extern const lv_font_t font_cn_16")
    for pattern, message in _FORBIDDEN_SOURCE:
        if re.search(pattern, text):
            raise ValueError(message)
    root = resolve_fpga_root(fpga_root)
    for line in text.splitlines():
        if line.strip().startswith("#include"):
            continue
        for match in re.finditer(r'"((?:\\.|[^"\\])*)"', line):
            literal = match.group(1).replace(r"\n", " ").replace(r"\"", '"')
            _font_ok(literal, root)


def write_ai_board_app(
    folder: str,
    icon_name: str,
    title: str,
    source: str,
    fpga_root: str | Path | None = None,
) -> Path:
    """校验并写入本机 apps/<目录名>/。已有同名本机应用则覆盖源码。"""
    folder = (folder or "").strip()
    icon_name = (icon_name or "").strip()
    title = (title or icon_name or folder).strip()
    if not _FOLDER_RE.fullmatch(folder):
        raise ValueError("目录名用字母开头，只含字母、数字和下划线，最长 16")
    if folder in {"snake", "face_track"}:
        raise ValueError("snake 和 face_track 是板工程自带示例，请换一个目录名")
    if not icon_name or len(icon_name.encode("utf-8")) > 15:
        raise ValueError("桌面图标名最多 15 字节（约 5 个汉字）")
    root = resolve_fpga_root(fpga_root)
    _font_ok(icon_name, root)
    _font_ok(title, root)
    validate_app_source(source, root)
    dest = HOST_APPS / folder
    if dest.exists() and not dest.is_dir():
        raise ValueError("无法写入：%s" % dest)
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "app_main.c").write_text(source.replace("\r\n", "\n"), encoding="utf-8", newline="\n")
    (dest / "app.json").write_text(
        json.dumps({"folder": folder, "icon": icon_name, "title": title}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return dest


def source_is_complete(source: str) -> bool:
    text = source or ""
    return "void app_main" in text and "app_boot" in text and ".app_entry_first" in text


def _checked_app_dir(folder: str) -> Path:
    folder = (folder or "").strip()
    if not _FOLDER_RE.fullmatch(folder):
        raise ValueError("目录名用字母开头，只含字母、数字和下划线，最长 16")
    if folder in {"snake", "face_track"}:
        raise ValueError("snake 和 face_track 是板工程自带示例，请换一个目录名")
    return HOST_APPS / folder


def _meta_fields(dest: Path, folder: str, icon_name: str, title: str, fpga_root) -> tuple[str, str]:
    meta = _read_meta(dest) if dest.exists() else {}
    icon_name = (icon_name or str(meta.get("icon") or folder)).strip() or folder
    title = (title or str(meta.get("title") or icon_name)).strip() or icon_name
    if len(icon_name.encode("utf-8")) > 15:
        raise ValueError("桌面图标名最多 15 字节（约 5 个汉字）")
    root = resolve_fpga_root(fpga_root)
    _font_ok(icon_name, root)
    _font_ok(title, root)
    return icon_name, title


def _write_meta(dest: Path, folder: str, icon_name: str, title: str) -> None:
    (dest / "app.json").write_text(
        json.dumps({"folder": folder, "icon": icon_name, "title": title}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _piece_status(folder: str, action: str, text: str) -> str:
    lines = text.splitlines()
    tail = "\n".join(lines[-3:])
    if source_is_complete(text):
        nxt = "文件里已经有 app_boot。下一步调用 write_board_app，不要传 source。"
    else:
        nxt = (
            "文件还不完整。用 save_app_source mode=append 只追加剩下的部分。"
            "不要从头重写，也不要说上一轮漏了 source。"
        )
    return "已%s：apps/%s/app_main.c，共 %d 行。\n末尾：\n%s\n%s" % (action, folder, len(lines), tail, nxt)


def save_app_piece(
    folder: str,
    icon_name: str,
    title: str,
    content: str,
    mode: str = "overwrite",
    old_text: str = "",
    fpga_root: str | Path | None = None,
) -> str:
    """分段写入 apps/<目录>/app_main.c，不编译。"""
    dest = _checked_app_dir(folder)
    folder = dest.name
    mode = (mode or "overwrite").strip().lower()
    content = (content or "").replace("\r\n", "\n")
    old = (old_text or "").replace("\r\n", "\n")
    dest.mkdir(parents=True, exist_ok=True)
    icon_name, title = _meta_fields(dest, folder, icon_name, title, fpga_root)
    path = dest / "app_main.c"
    if mode == "replace":
        if not old.strip():
            raise ValueError("replace 需要 old_text，填要换掉的那几行原文。")
        if not path.is_file():
            raise ValueError("还没有 app_main.c。先用 mode=overwrite 写开头，不要整份重写。")
        current = path.read_text(encoding="utf-8")
        count = current.count(old)
        if count != 1:
            raise ValueError(
                "old_text 在 apps/%s/app_main.c 里出现 %d 次，需要恰好 1 次。"
                "用 read_project_file 读 apps/%s/app_main.c，换一段更长的原文。不要整份重写。"
                % (folder, count, folder)
            )
        text = current.replace(old, content, 1)
        if len(text) > 48000:
            raise ValueError("源码超过 48KB")
        path.write_text(text, encoding="utf-8", newline="\n")
        _write_meta(dest, folder, icon_name, title)
        return _piece_status(folder, "替换一小段", text)
    if mode == "append":
        if not content.strip():
            raise ValueError("追加内容是空的")
        existing = path.read_text(encoding="utf-8") if path.is_file() else ""
        if source_is_complete(existing):
            raise ValueError(
                "apps/%s/app_main.c 已经有 app_boot，不要再追加。"
                "调用 write_board_app 且不要传 source。要改逻辑就用 mode=replace。" % folder
            )
        if existing and not existing.endswith("\n"):
            existing += "\n"
        text = existing + content
        if len(text) > 48000:
            raise ValueError("源码超过 48KB")
        path.write_text(text, encoding="utf-8", newline="\n")
        _write_meta(dest, folder, icon_name, title)
        return _piece_status(folder, "追加到末尾", text)
    if mode != "overwrite":
        raise ValueError("mode 只能是 overwrite、append 或 replace")
    if not content.strip():
        raise ValueError("写入内容是空的")
    if len(content) > 48000:
        raise ValueError("源码超过 48KB")
    if path.is_file():
        existing = path.read_text(encoding="utf-8")
        if source_is_complete(existing) and not source_is_complete(content):
            raise ValueError(
                "apps/%s/app_main.c 已经是完整文件，这次内容不完整，没有覆盖。"
                "不要整份重写。用 mode=replace 只改几行，或 write_board_app 且不要传 source。" % folder
            )
        if (not source_is_complete(existing)) and (not source_is_complete(content)):
            if len(content.splitlines()) <= len(existing.splitlines()):
                raise ValueError(
                    "apps/%s/app_main.c 已有 %d 行未完成的源码，这次没有更长，没有覆盖。"
                    "用 mode=append 从末尾继续，不要从头再写。"
                    % (folder, len(existing.splitlines()))
                )
    path.write_text(content, encoding="utf-8", newline="\n")
    _write_meta(dest, folder, icon_name, title)
    return _piece_status(folder, "写入", content)


def prepare_ai_app(
    folder: str,
    icon_name: str,
    title: str,
    source: str,
    fpga_root: str | Path | None = None,
) -> str:
    dest = _checked_app_dir(folder)
    folder = dest.name
    source = (source or "").replace("\r\n", "\n")
    if not source.strip():
        path = dest / "app_main.c"
        if not path.is_file():
            raise ValueError(
                "没有收到 source，apps/%s/app_main.c 也不存在。"
                "请调用 save_app_source，mode=overwrite，一次大约 100 行。"
                "不要把整份源码塞进 write_board_app。" % folder
            )
        text = path.read_text(encoding="utf-8")
        if not source_is_complete(text):
            raise ValueError(
                "apps/%s/app_main.c 有 %d 行，还没有 app_boot。"
                "用 save_app_source mode=append 只追加剩余部分。不要整份重写。"
                % (folder, len(text.splitlines()))
            )
        meta = _read_meta(dest)
        icon_name = (icon_name or str(meta.get("icon") or folder)).strip() or folder
        title = (title or str(meta.get("title") or icon_name)).strip() or icon_name
    else:
        save_app_piece(folder, icon_name, title, source, "overwrite", "", fpga_root)
        text = (dest / "app_main.c").read_text(encoding="utf-8")
        meta = _read_meta(dest)
        icon_name = str(meta.get("icon") or icon_name or folder).strip() or folder
        title = str(meta.get("title") or title or icon_name).strip() or icon_name
        if not source_is_complete(text):
            raise ValueError(
                "已写入 apps/%s/app_main.c（%d 行），还没有 app_boot，所以没有编译。"
                "用 save_app_source mode=append 只追加剩余部分。不要整份重写，也不要再说漏了 source。"
                % (folder, len(text.splitlines()))
            )
    try:
        validate_app_source(text, fpga_root)
    except ValueError as exc:
        raise ValueError(
            "apps/%s/app_main.c 已在磁盘上，还不能编译：%s\n"
            "用 save_app_source mode=replace 只改有问题的几行，然后 write_board_app 不要传 source。不要整份重写。"
            % (folder, exc)
        ) from exc
    try:
        binary = build_board_app(dest, icon_name, fpga_root)
    except Exception as exc:
        raise RuntimeError(
            "编译失败。源码仍在 apps/%s/app_main.c。用 save_app_source mode=replace 只替换报错附近的原文，"
            "然后 write_board_app 且不要传 source。不要整份重写。\n%s" % (folder, exc)
        ) from exc
    return "编译成功：%s\n桌面图标名：%s\n用户确认后再调用 deploy_board_app 下发。" % (binary, icon_name)


def deploy_ai_app(folder: str, ip: str, port: int = 69) -> str:
    folder = (folder or "").strip()
    if not _FOLDER_RE.fullmatch(folder):
        raise ValueError("目录名不合法")
    binary = HOST_APPS / folder / ("%s.bin" % folder)
    if not binary.is_file():
        raise FileNotFoundError("还没有编译成功的 bin，先调用 write_board_app")
    remote = push_board_app(binary, ip, int(port))
    return "已下发到 %s" % remote
