"""上位机内的板工程工具、符号表，以及下载到空白 FPGA 的镜像。

tools/ 与 firmware/ 可以整套从新的 Vitis 工程替换。
符号版本以 tools/sdapp/fw_exports.json 为准。
下载优先用 xsct（Vitis）或 xsdb（Vivado）。都没有时用项目内置的 OpenOCD。
掉电后需再下载一次。只安装 Vitis HLS 时没有 xsct。
"""
from __future__ import annotations

import json
import locale
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from repo_paths import project_root

BUNDLE_FILES = {
    "bit": "system_wrapper.bit",
    "fsbl": "fsbl.elf",
    "elf": "cam_lcd_hdmi_servo_net.elf",
}

_XSCT_CANDIDATES = (
    r"D:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat",
    r"D:\software\AMDDesign\2025.2\Vitis\bin\xsct.bat",
)


def tools_dir() -> Path:
    return project_root() / "tools"


def firmware_dir() -> Path:
    return project_root() / "firmware"


def manifest_path() -> Path:
    return firmware_dir() / "manifest.json"


def symbol_meta(tools: Path | None = None) -> dict:
    root = tools or tools_dir()
    path = root / "sdapp" / "fw_exports.json"
    ld = root / "sdapp" / "fw_exports.ld"
    if not path.is_file():
        return {"version": "", "count": 0, "ld": ld.is_file()}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        data = {}
    return {
        "version": str(data.get("version") or ""),
        "count": int(data.get("count") or 0),
        "ld": ld.is_file(),
    }


def load_manifest() -> dict:
    path = manifest_path()
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def bundle_status() -> dict:
    fw = firmware_dir()
    files = {}
    missing = []
    for key, name in BUNDLE_FILES.items():
        path = fw / name
        ok = path.is_file() and path.stat().st_size > 0
        files[key] = {"name": name, "path": str(path), "bytes": path.stat().st_size if ok else 0, "ok": ok}
        if not ok:
            missing.append(name)
    symbols = symbol_meta()
    ready = not missing and symbols["ld"]
    return {
        "ready": ready,
        "tools": str(tools_dir()),
        "firmware": str(fw),
        "symbols": symbols,
        "files": files,
        "missing": missing,
        "manifest": load_manifest(),
    }


def _app_root(path: Path) -> Path | None:
    path = path.resolve()
    candidates = [path, path / "vitis" / "cam_lcd_hdmi_servo_net"]
    if path.name != "cam_lcd_hdmi_servo_net":
        candidates.append(path / "cam_lcd_hdmi_servo_net")
    for item in candidates:
        if (item / "tools" / "sdapp" / "sdapp.py").is_file():
            return item
    return None


def _pick_existing(candidates: list[Path]) -> Path | None:
    for path in candidates:
        if path.is_file() and path.stat().st_size > 0:
            return path
    return None


def locate_firmware_sources(app_root: Path) -> dict[str, Path]:
    """在 Vitis 应用目录里找比特流、FSBL 和应用 ELF。"""
    ws = app_root.parent
    bit = _pick_existing([
        app_root / "_ide" / "bitstream" / "system_wrapper.bit",
        app_root / "build" / "system_wrapper.bit",
        ws / "FreeRtos_Platform" / "export" / "FreeRtos_Platform" / "hw" / "system_wrapper.bit",
    ])
    fsbl = _pick_existing([
        ws / "FreeRtos_Platform" / "export" / "FreeRtos_Platform" / "sw" / "boot" / "fsbl.elf",
        ws / "FreeRtos_Platform" / "zynq_fsbl" / "build" / "fsbl.elf",
    ])
    elf = _pick_existing([
        app_root / "build" / "cam_lcd_hdmi_servo_net.elf",
        app_root / "build" / (app_root.name + ".elf"),
    ])
    found = {}
    if bit:
        found["bit"] = bit
    if fsbl:
        found["fsbl"] = fsbl
    if elf:
        found["elf"] = elf
    return found


def compare_with_project(fpga_root: str | Path) -> dict:
    """对比仓库内符号表和指定板工程。不复制文件。"""
    app = _app_root(Path(fpga_root))
    if app is None:
        return {"ok": False, "message": "该目录里没有 tools/sdapp/sdapp.py。"}
    local = symbol_meta()
    remote = symbol_meta(app / "tools")
    sources = locate_firmware_sources(app)
    missing = [BUNDLE_FILES[key] for key in BUNDLE_FILES if key not in sources]
    same = bool(local["version"]) and local["version"] == remote["version"]
    if not remote["version"]:
        relation = "板工程里还没有 fw_exports.json。请先在该工程执行 python tools/sdapp/sdapp.py export。"
    elif not local["version"]:
        relation = "仓库里还没有符号表，可以导入。"
    elif same:
        relation = "符号版本一致（%s）。" % remote["version"]
    else:
        relation = "符号版本不同。仓库 %s，板工程 %s。可以替换。" % (local["version"], remote["version"])
    return {
        "ok": True,
        "same": same,
        "app": str(app),
        "local": local,
        "remote": remote,
        "firmware_missing": missing,
        "message": relation,
    }


def _copy_tree(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(
        src,
        dst,
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
    )


def import_from_project(fpga_root: str | Path) -> dict:
    """用板工程的 tools 和已编译镜像替换仓库里的副本。"""
    app = _app_root(Path(fpga_root))
    if app is None:
        return {"ok": False, "message": "未找到含 tools/sdapp/sdapp.py 的板工程。"}
    remote = symbol_meta(app / "tools")
    if not remote["ld"]:
        return {
            "ok": False,
            "message": "板工程没有 fw_exports.ld。先在该工程执行 python tools/sdapp/sdapp.py export。",
        }
    sources = locate_firmware_sources(app)
    missing = [key for key in BUNDLE_FILES if key not in sources]
    if missing:
        names = "、".join(BUNDLE_FILES[key] for key in missing)
        return {
            "ok": False,
            "message": "板工程里缺少 %s。请先在 Vitis 里编完 FSBL 和应用，并导出比特流。" % names,
        }
    _copy_tree(app / "tools", tools_dir())
    fw = firmware_dir()
    fw.mkdir(parents=True, exist_ok=True)
    for key, name in BUNDLE_FILES.items():
        shutil.copy2(sources[key], fw / name)
    meta = symbol_meta()
    manifest = {
        "board_name": "18_cam_lcd_hdmi_servo_net",
        "source": str(app),
        "symbol_version": meta["version"],
        "symbol_count": meta["count"],
        "files": dict(BUNDLE_FILES),
    }
    manifest_path().write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {
        "ok": True,
        "message": "已替换 tools 与下载镜像。符号 %s 个，版本 %s。" % (meta["count"], meta["version"]),
        "manifest": manifest,
    }


MISSING_DOWNLOAD_TOOL = (
    "未找到下载工具。本机没有 xsct/xsdb，项目里的 prebuilt/openocd 也不完整。"
    "可以安装 Vivado，或设置 FPGA_XSCT 指向 xsct.bat、xsdb.bat。"
)

_SCAN_ROOTS = (
    Path(r"C:\Xilinx"),
    Path(r"D:\Xilinx"),
    Path(r"E:\Xilinx"),
    Path(r"F:\Xilinx"),
    Path(r"C:\AMDDesignTools"),
    Path(r"D:\AMDDesignTools"),
    Path(r"C:\software\AMDDesign"),
    Path(r"D:\software\AMDDesign"),
    Path(r"C:\Program Files\Xilinx"),
    Path(r"D:\Program Files\Xilinx"),
)

_tool_cache: dict | None | bool = False


def _version_key(name: str) -> tuple[int, ...]:
    parts: list[int] = []
    for piece in name.split("."):
        if piece.isdigit():
            parts.append(int(piece))
        else:
            break
    return tuple(parts) if parts else (0,)


def _version_of(bat: Path) -> tuple[int, ...]:
    for candidate in (bat.parent.parent.name, bat.parent.parent.parent.name):
        key = _version_key(candidate)
        if key != (0,):
            return key
    return (0,)


def _tool_kind(path: Path) -> str:
    return "xsct" if "xsct" in path.name.lower() else "xsdb"


def _describe_tool(path: Path) -> dict:
    kind = _tool_kind(path)
    label = "Vitis xsct" if kind == "xsct" else "Vivado xsdb"
    return {"path": str(path), "kind": kind, "label": label}


def _child_dirs(path: Path) -> list[Path]:
    try:
        return [item for item in path.iterdir() if item.is_dir()]
    except OSError:
        return []


def _bats_under(product: Path, bat_name: str) -> list[Path]:
    found = []
    direct = product / "bin" / bat_name
    if direct.is_file():
        found.append(direct)
    versions = _child_dirs(product)
    versions.sort(key=lambda item: _version_key(item.name), reverse=True)
    for version in versions:
        bat = version / "bin" / bat_name
        if bat.is_file():
            found.append(bat)
    return found


def _remember(bucket: list[Path], seen: set[str], path: Path) -> None:
    if not path.is_file() or path.stat().st_size <= 0:
        return
    key = str(path.resolve()).lower()
    if key in seen:
        return
    seen.add(key)
    bucket.append(path)


def _registry_bats() -> list[tuple[Path, str]]:
    if os.name != "nt":
        return []
    import winreg

    keys = (
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
    )
    found = []
    for hive, key_path in keys:
        try:
            root = winreg.OpenKey(hive, key_path)
        except OSError:
            continue
        try:
            count = winreg.QueryInfoKey(root)[0]
            for index in range(count):
                try:
                    sub = winreg.OpenKey(root, winreg.EnumKey(root, index))
                except OSError:
                    continue
                try:
                    try:
                        name = str(winreg.QueryValueEx(sub, "DisplayName")[0])
                        loc = str(winreg.QueryValueEx(sub, "InstallLocation")[0]).strip().strip('"')
                    except OSError:
                        continue
                finally:
                    sub.Close()
                if not loc:
                    continue
                lowered = name.lower()
                base = Path(loc)
                if "vitis" in lowered and "hls" not in lowered:
                    bat = base / "bin" / "xsct.bat"
                    if bat.is_file():
                        found.append((bat, "xsct"))
                if "vivado" in lowered:
                    bat = base / "bin" / "xsdb.bat"
                    if bat.is_file():
                        found.append((bat, "xsdb"))
        finally:
            root.Close()
    return found


def _search_download_tool() -> dict | None:
    xsct_hits: list[Path] = []
    xsdb_hits: list[Path] = []
    seen: set[str] = set()

    override = (os.environ.get("FPGA_XSCT") or os.environ.get("FPGA_XSDB") or "").strip().strip('"')
    if override:
        path = Path(override)
        if path.is_file():
            if path.name.lower().startswith("openocd"):
                return _describe_openocd(path)
            return _describe_tool(path)

    for raw in _XSCT_CANDIDATES:
        _remember(xsct_hits, seen, Path(raw))

    for env_name, filename, bucket in (
        ("XILINX_VITIS", "xsct.bat", xsct_hits),
        ("XILINX_VIVADO", "xsdb.bat", xsdb_hits),
    ):
        raw = (os.environ.get(env_name) or "").strip().strip('"')
        if raw:
            _remember(bucket, seen, Path(raw) / "bin" / filename)

    for name in ("xsct.bat", "xsct", "xsdb.bat", "xsdb"):
        found = shutil.which(name)
        if not found:
            continue
        path = Path(found)
        _remember(xsct_hits if "xsct" in path.name.lower() else xsdb_hits, seen, path)

    for root in _SCAN_ROOTS:
        if not root.is_dir():
            continue
        products = []
        for child in _child_dirs(root):
            if child.name in ("Vitis", "Vivado"):
                products.append(child)
            else:
                for name in ("Vitis", "Vivado"):
                    nested = child / name
                    if nested.is_dir():
                        products.append(nested)
        for product in products:
            bucket = xsct_hits if product.name == "Vitis" else xsdb_hits
            bat_name = "xsct.bat" if product.name == "Vitis" else "xsdb.bat"
            for bat in _bats_under(product, bat_name):
                _remember(bucket, seen, bat)

    if not xsct_hits and not xsdb_hits:
        for bat, kind in _registry_bats():
            _remember(xsct_hits if kind == "xsct" else xsdb_hits, seen, bat)

    if xsct_hits:
        return _describe_tool(max(xsct_hits, key=_version_of))
    if xsdb_hits:
        return _describe_tool(max(xsdb_hits, key=_version_of))
    return bundled_openocd()


def _describe_openocd(exe: Path) -> dict:
    return {"path": str(exe), "kind": "openocd", "label": "项目内置 OpenOCD"}


def bundled_openocd() -> dict | None:
    """prebuilt/openocd 里的 openocd.exe。没有 Vivado 的电脑用它下载。"""
    root = project_root() / "prebuilt" / "openocd"
    if not root.is_dir():
        return None
    found = list(root.glob("bin/openocd.exe")) + list(root.glob("*/bin/openocd.exe"))
    found = [path for path in found if path.is_file()]
    if not found:
        return None
    return _describe_openocd(found[0])


def find_download_tool() -> dict | None:
    """已安装的 xsct/xsdb 优先。否则用项目内置 OpenOCD。"""
    global _tool_cache
    if _tool_cache is not False:
        return _tool_cache
    _tool_cache = _search_download_tool()
    return _tool_cache


def find_xsct() -> Path | None:
    tool = find_download_tool()
    if not tool:
        return None
    return Path(tool["path"])


def _decode_output(data: bytes | str | None) -> str:
    if data is None:
        return ""
    if isinstance(data, str):
        return data
    for encoding in ("utf-8", "gbk"):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def _script_bytes(text: str) -> bytes:
    encoding = locale.getpreferredencoding(False) or "utf-8"
    try:
        return text.encode(encoding)
    except UnicodeEncodeError:
        return text.encode("utf-8")


def _run_download_tool(tool: Path, tcl_path: Path) -> tuple[int, str]:
    """单独进程跑 bat，避免无控制台的上位机跟着 xsdb/xsct 一起退出。"""
    # 整段作为命令行字符串交给 cmd。多一层引号，路径里有空格时也不会把引号剥掉。
    cmdline = 'cmd.exe /d /c ""%s" "%s""' % (tool, tcl_path)
    kwargs = {
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
    }
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        kwargs["startupinfo"] = startup
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen(cmdline, **kwargs)
    try:
        out, err = proc.communicate(timeout=240)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            try:
                subprocess.run(
                    ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                    check=False,
                )
            except OSError:
                pass
        proc.kill()
        out, err = proc.communicate()
        raise subprocess.TimeoutExpired(cmdline, 240, output=out, stderr=err)
    log = (_decode_output(out) + "\n" + _decode_output(err)).strip()
    code = proc.returncode if proc.returncode is not None else 1
    return code, log


def rebuild_linked_artifacts(fpga_root: str | Path | None = None) -> dict:
    """用指定板工程里的新符号表，重编本机桌面应用和各模型的 model.mod。"""
    root_text = str(fpga_root or "").strip()
    if not root_text:
        root_text = str(load_manifest().get("source") or "")
    app = _app_root(Path(root_text)) if root_text else None
    if app is None:
        return {
            "ok": False,
            "message": "没有可用的板工程。先 adapt_firmware action=import，或传入含 tools/sdapp 的目录。",
        }
    lines = ["板工程 %s" % app]
    ok = True
    from board_apps import HOST_APPS, build_board_app

    if HOST_APPS.is_dir():
        for child in sorted(HOST_APPS.iterdir()):
            if not child.is_dir() or child.name in ("snake", "face_track"):
                continue
            if not (child / "app_main.c").is_file():
                continue
            icon = child.name
            meta_path = child / "app.json"
            if meta_path.is_file():
                try:
                    meta = json.loads(meta_path.read_text(encoding="utf-8"))
                    icon = str(meta.get("icon") or icon)
                except (OSError, json.JSONDecodeError):
                    pass
            try:
                binary = build_board_app(child, icon, app)
                lines.append("应用 %s 已重编 %s" % (child.name, binary.name))
            except Exception as exc:
                ok = False
                lines.append("应用 %s 重编失败: %s" % (child.name, exc))
    else:
        lines.append("没有 apps/ 目录。")

    from board_mod import prepare_board_pack

    models = project_root() / "models"
    if models.is_dir():
        for child in sorted(models.iterdir()):
            if not child.is_dir() or not (child / "model.cfg").is_file():
                continue
            try:
                result = prepare_board_pack(child, app)
                lines.append("模型 %s：%s" % (child.name, result.get("status") or ""))
                if not result.get("ready"):
                    ok = False
            except Exception as exc:
                ok = False
                lines.append("模型 %s 重编失败: %s" % (child.name, exc))
    lines.append("重编完成后，板上「文件传输」打开后再推送应用和模型。")
    return {"ok": ok, "message": "\n".join(lines), "app": str(app)}


_OPENOCD_INTERFACES = (
    "interface/ftdi/digilent-hs2.cfg",
    "interface/ftdi/digilent_jtag_hs3.cfg",
    "interface/ftdi/digilent_jtag_smt2.cfg",
    "interface/ftdi/digilent-hs1.cfg",
    "interface/ftdi/digilent_jtag_smt2_nc.cfg",
    "ft2232_generic.cfg",
)


def _openocd_layout(exe: Path) -> tuple[Path, Path]:
    root = exe.parent.parent
    scripts = root / "openocd" / "scripts"
    board = project_root() / "prebuilt" / "openocd" / "board"
    return scripts, board


def _run_argv(argv: list[str], timeout: int) -> tuple[int, str]:
    kwargs = {
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
    }
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        kwargs["startupinfo"] = startup
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen(argv, **kwargs)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            try:
                subprocess.run(
                    ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                    check=False,
                )
            except OSError:
                pass
        proc.kill()
        out, err = proc.communicate()
        raise subprocess.TimeoutExpired(argv, timeout, output=out, stderr=err)
    log = (_decode_output(out) + "\n" + _decode_output(err)).strip()
    code = proc.returncode if proc.returncode is not None else 1
    return code, log


def _elf_entry(path: Path) -> int:
    with path.open("rb") as handle:
        header = handle.read(64)
    if len(header) < 32 or header[:4] != b"\x7fELF":
        raise ValueError("不是 ELF：%s" % path.name)
    if header[4] == 2:
        return int.from_bytes(header[24:32], "little")
    return int.from_bytes(header[24:28], "little")


def _openocd_interface_arg(name: str, scripts: Path, board: Path) -> str | None:
    direct = Path(name)
    if direct.is_file():
        return str(direct)
    if (scripts / name).is_file():
        return name
    if (board / name).is_file():
        return str(board / name)
    return None


def _openocd_command(exe: Path, scripts: Path, board: Path, interface: str, cfg: Path) -> list[str]:
    return [
        str(exe),
        "-s", str(scripts),
        "-s", str(board),
        "-c", "gdb_port disabled",
        "-c", "tcl_port disabled",
        "-c", "telnet_port disabled",
        "-f", interface,
        "-f", "target/zynq_7000.cfg",
        "-f", str(cfg),
    ]


def _write_cfg(text: str) -> Path:
    fd, name = tempfile.mkstemp(suffix=".cfg", prefix="fpga_ocd_")
    os.close(fd)
    path = Path(name)
    path.write_bytes(_script_bytes(text))
    return path


def _probe_openocd(exe: Path, scripts: Path, board: Path, interface: str) -> tuple[bool, str]:
    cfg = _write_cfg("adapter speed 1000\ninit\necho PROBE_OK\nshutdown\n")
    try:
        _code, log = _run_argv(_openocd_command(exe, scripts, board, interface, cfg), 30)
    except subprocess.TimeoutExpired as exc:
        log = (_decode_output(exc.output) + "\n" + _decode_output(exc.stderr)).strip()
        return False, log
    finally:
        cfg.unlink(missing_ok=True)
    return "PROBE_OK" in log, log


def _select_openocd_interface(exe: Path) -> tuple[str | None, str]:
    scripts, board = _openocd_layout(exe)
    if not scripts.is_dir():
        return None, "OpenOCD 脚本目录不存在：%s" % scripts
    chosen = (os.environ.get("FPGA_OPENOCD_INTERFACE") or "").strip().strip('"')
    names = (chosen,) if chosen else _OPENOCD_INTERFACES
    notes = []
    for name in names:
        if not name:
            continue
        arg = _openocd_interface_arg(name, scripts, board)
        if not arg:
            notes.append("%s 不存在" % name)
            continue
        ok, log = _probe_openocd(exe, scripts, board, arg)
        if ok:
            return arg, log
        tail = log.strip().splitlines()[-1] if log.strip() else "无输出"
        notes.append("%s：%s" % (Path(name).name, tail))
        if chosen:
            break
    return None, "\n".join(notes)


def _program_with_openocd(tool: dict, bit: Path, fsbl: Path, elf: Path) -> dict:
    exe = Path(tool["path"])
    scripts, board = _openocd_layout(exe)
    interface, probe_log = _select_openocd_interface(exe)
    if not interface:
        return {
            "ok": False,
            "message": (
                "没有找到可用的 JTAG 下载器。项目内置 OpenOCD，不必安装 Vivado。"
                "请确认下载器已插上；若驱动仍是串口或 Xilinx 专用驱动，用 Zadig 把 JTAG 接口换成 WinUSB。"
                "也可以设置 FPGA_OPENOCD_INTERFACE 指定 interface 配置。"
            ),
            "log": probe_log,
        }
    stage = Path(tempfile.mkdtemp(prefix="fpga_dl_"))
    cfg = None
    try:
        staged_bit = stage / "system.bit"
        staged_fsbl = stage / "fsbl.elf"
        staged_elf = stage / "app.elf"
        shutil.copy2(bit, staged_bit)
        shutil.copy2(fsbl, staged_fsbl)
        shutil.copy2(elf, staged_elf)
        fsbl_entry = _elf_entry(staged_fsbl)
        elf_entry = _elf_entry(staged_elf)
        cfg = _write_cfg(
            """
adapter speed 1000
init
if {[catch {targets zynq.cpu0}]} {
  echo PROGRAM_FAIL no Cortex-A9 target
  shutdown
}
targets zynq.cpu0
catch {reset halt}
if {[catch {pld load 0 {%s}} msg]} {
  echo "PROGRAM_FAIL bitstream $msg"
  shutdown
}
catch {halt}
if {[catch {load_image {%s}} msg]} {
  echo "PROGRAM_FAIL fsbl $msg"
  shutdown
}
resume 0x%08x
sleep 3000
catch {halt}
if {[catch {load_image {%s}} msg]} {
  echo "PROGRAM_FAIL elf $msg"
  shutdown
}
resume 0x%08x
echo PROGRAM_OK
shutdown
"""
            % (
                staged_bit.as_posix(),
                staged_fsbl.as_posix(),
                fsbl_entry,
                staged_elf.as_posix(),
                elf_entry,
            )
        )
        try:
            code, log = _run_argv(_openocd_command(exe, scripts, board, interface, cfg), 240)
        except subprocess.TimeoutExpired as exc:
            log = (_decode_output(exc.output) + "\n" + _decode_output(exc.stderr)).strip()
            return {
                "ok": False,
                "message": "下载超时。请确认 JTAG 已连接，并且没有别的下载软件占用这条线。",
                "log": log,
            }
        except OSError as exc:
            return {"ok": False, "message": "无法启动 OpenOCD：%s" % exc, "log": ""}
    finally:
        if cfg is not None:
            cfg.unlink(missing_ok=True)
        shutil.rmtree(stage, ignore_errors=True)
    log = ("工具 %s\n接口 %s\n%s" % (exe, interface, log)).strip()
    ok = "PROGRAM_OK" in log
    if ok:
        message = (
            "已下载到 FPGA。板上程序在本次上电期间运行；重新上电后需要再下载一次。"
            "秒表、人脸追踪和贪吃蛇不在比特流里，它们要写到 SD 卡的 0:/apps。"
            "请在板上打开「文件传输」，再在「板端应用」里把这三个应用下发一次。"
        )
    elif "no Cortex-A9" in log:
        message = "没有发现 Cortex-A9。请接上 JTAG，给板子上电后再试。"
    elif "PROGRAM_FAIL bitstream" in log:
        message = "比特流没有写进去。"
    elif "PROGRAM_FAIL fsbl" in log:
        message = "FSBL 没有写进去。"
    elif "PROGRAM_FAIL elf" in log:
        message = "应用没有写进去。FSBL 可能还没把内存准备好。"
    else:
        message = "下载未完成（OpenOCD 退出码 %s）。" % code
    return {"ok": ok, "message": message, "log": log}


def program_blank_fpga() -> dict:
    """把比特流、FSBL 和应用 ELF 下载到已连接的空白 Zynq。"""
    status = bundle_status()
    if not status["ready"]:
        lack = "、".join(status["missing"] or ["fw_exports.ld"])
        return {"ok": False, "message": "下载文件不齐：%s" % lack, "log": ""}
    tool = find_download_tool()
    if not tool:
        return {"ok": False, "message": MISSING_DOWNLOAD_TOOL, "log": ""}
    tool_path = Path(tool["path"])
    fw = firmware_dir()
    bit = (fw / BUNDLE_FILES["bit"]).resolve()
    fsbl = (fw / BUNDLE_FILES["fsbl"]).resolve()
    elf = (fw / BUNDLE_FILES["elf"]).resolve()
    if tool.get("kind") == "openocd":
        return _program_with_openocd(tool, bit, fsbl, elf)
    tcl = """
set bit {%s}
set fsbl {%s}
set elf {%s}
connect
set hits [targets -nocase -filter {name =~ "*Cortex-A9*#0"}]
if {[llength $hits] == 0} {
  puts "PROGRAM_FAIL no Cortex-A9 target"
  exit 2
}
targets -set -nocase -filter {name =~ "*Cortex-A9*#0"}
rst -system
after 2000
fpga -file $bit
targets -set -nocase -filter {name =~ "*Cortex-A9*#0"}
dow $fsbl
con
after 3000
stop
dow $elf
con
puts "PROGRAM_OK"
""" % (bit.as_posix(), fsbl.as_posix(), elf.as_posix())
    fd, name = tempfile.mkstemp(suffix=".tcl", prefix="fpga_program_")
    os.close(fd)
    script = Path(name)
    script.write_bytes(_script_bytes(tcl))
    try:
        try:
            code, log = _run_download_tool(tool_path, script)
        except subprocess.TimeoutExpired as exc:
            log = (_decode_output(exc.output) + "\n" + _decode_output(exc.stderr)).strip()
            return {
                "ok": False,
                "message": "下载超时。请确认 JTAG 已连接，并且没有别的 Vivado 或 Vitis 会话占用。",
                "log": log,
            }
        except OSError as exc:
            return {"ok": False, "message": "无法启动 %s：%s" % (tool["label"], exc), "log": ""}
    finally:
        script.unlink(missing_ok=True)
    log = ("工具 %s\n%s" % (tool_path, log)).strip()
    ok = code == 0 and "PROGRAM_OK" in log
    if ok:
        message = (
            "已下载到 FPGA。板上程序在本次上电期间运行；重新上电后需要再下载一次。"
            "秒表、人脸追踪和贪吃蛇不在比特流里，它们要写到 SD 卡的 0:/apps。"
            "请在板上打开「文件传输」，再在「板端应用」里把这三个应用下发一次。"
        )
    elif "no Cortex-A9" in log:
        message = "没有发现 Cortex-A9。请接上 JTAG，给板子上电后再试。"
    else:
        message = "下载未完成（%s 退出码 %s）。" % (tool["label"], code)
    return {"ok": ok, "message": message, "log": log}
