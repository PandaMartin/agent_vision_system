"""把本机网卡配到与板子同一网段，便于 TFTP 推送。

按 board_profile.json 的 pc_ip_hint（例如 192.168.1.100/24）向选中网卡
追加地址，不删除原有 IP，也不改成只保留这一段。改地址需要管理员权限。
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

_IP_CIDR = re.compile(
    r"^\s*(\d{1,3}(?:\.\d{1,3}){3})\s*/\s*(\d{1,2})\s*$"
)


def _run_ps(script: str, timeout: int = 45) -> tuple[int, str, str]:
    # 打包后的程序没有控制台，PowerShell 默认用 GBK。这里强制 UTF-8，网卡名才不会变成乱码。
    wrapped = (
        "[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false; "
        "$OutputEncoding = [Console]::OutputEncoding; "
        + script
    )
    proc = subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-Command",
            wrapped,
        ],
        capture_output=True,
        timeout=timeout,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )

    def _text(data: bytes | None) -> str:
        return (data or b"").decode("utf-8", errors="replace")

    return proc.returncode, _text(proc.stdout), _text(proc.stderr)


def parse_pc_ip_hint(hint: str | None = None) -> dict:
    """返回 {ip, prefix, mask, cidr}。hint 缺省时读板卡档案。"""
    raw = (hint or "").strip()
    if not raw:
        from host_skill import load_profile

        raw = str(load_profile().get("pc_ip_hint") or "192.168.1.100/24")
    found = _IP_CIDR.match(raw)
    if not found:
        raise ValueError("pc_ip_hint 格式应为 192.168.1.100/24，当前是：%s" % raw)
    ip = found.group(1)
    prefix = int(found.group(2))
    if prefix < 0 or prefix > 32:
        raise ValueError("子网前缀无效：%d" % prefix)
    parts = [int(p) for p in ip.split(".")]
    if any(p < 0 or p > 255 for p in parts):
        raise ValueError("IP 无效：%s" % ip)
    mask_int = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF if prefix else 0
    mask = ".".join(str((mask_int >> (8 * (3 - i))) & 0xFF) for i in range(4))
    return {"ip": ip, "prefix": prefix, "mask": mask, "cidr": "%s/%d" % (ip, prefix)}


def _same_subnet(a: str, b: str, prefix: int) -> bool:
    def to_int(text: str) -> int:
        parts = [int(p) for p in text.split(".")]
        return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]

    bits = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF if prefix else 0
    return (to_int(a) & bits) == (to_int(b) & bits)


def list_adapters() -> list[dict]:
    """列出本机 IPv4 网卡。每项含 name、status、ips、if_type、connected。"""
    script = r"""
$ErrorActionPreference = 'Stop'
$adapters = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object {
  $_.Status -ne 'Not Present'
}
$rows = foreach ($a in $adapters) {
  $ips = @(Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -and $_.IPAddress -notlike '169.254.*' } |
    Select-Object -ExpandProperty IPAddress)
  [pscustomobject]@{
    name = $a.Name
    status = [string]$a.Status
    if_type = [string]$a.InterfaceDescription
    media = [string]$a.MediaType
    connected = [bool]($a.Status -eq 'Up')
    ips = @($ips)
  }
}
$rows | ConvertTo-Json -Compress -Depth 4
"""
    code, out, err = _run_ps(script)
    text = out.strip()
    if code != 0 or not text:
        raise RuntimeError((err or out or "无法枚举网卡").strip())
    data = json.loads(text)
    if isinstance(data, dict):
        data = [data]
    result: list[dict] = []
    for item in data or []:
        ips = item.get("ips") or []
        if isinstance(ips, str):
            ips = [ips]
        result.append(
            {
                "name": str(item.get("name") or ""),
                "status": str(item.get("status") or ""),
                "if_type": str(item.get("if_type") or ""),
                "media": str(item.get("media") or ""),
                "connected": bool(item.get("connected")),
                "ips": [str(x) for x in ips if x],
            }
        )
    return [row for row in result if row["name"]]


def _is_wifi(adapter: dict) -> bool:
    blob = (" ".join([adapter.get("name", ""), adapter.get("if_type", ""), adapter.get("media", "")])).lower()
    keys = ("wi-fi", "wifi", "wlan", "wireless", "无线", "802.11")
    return any(key in blob for key in keys)


def _is_virtual(adapter: dict) -> bool:
    blob = (" ".join([adapter.get("name", ""), adapter.get("if_type", "")])).lower()
    keys = (
        "virtual", "vmware", "virtualbox", "hyper-v", "vethernet",
        "loopback", "npcap", "tap-windows", "wsl", "docker", "vpn",
        "虚拟", "回环",
    )
    return any(key in blob for key in keys)


def source_ip_for_board(board_ip: str, hint: str | None = None) -> str:
    """选出与板子同网段、可以用来发包的本机地址。"""
    target = parse_pc_ip_hint(hint)
    prefix = int(target["prefix"])
    preferred = target["ip"]
    ranked: list[tuple[int, str]] = []
    for row in list_adapters():
        for ip in row.get("ips") or []:
            if not _same_subnet(ip, board_ip, prefix):
                continue
            score = 0
            if ip == preferred:
                score += 50
            if row.get("connected"):
                score += 20
            if not _is_wifi(row):
                score += 10
            ranked.append((score, ip))
    if not ranked:
        return ""
    ranked.sort(reverse=True)
    return ranked[0][1]


def pick_adapter(adapters: list[dict] | None = None, preferred: str = "") -> dict | None:
    """优先已连接的有线网卡；preferred 有同名则直接用。"""
    rows = adapters if adapters is not None else list_adapters()
    if preferred:
        for row in rows:
            if row["name"] == preferred:
                return row
    scored: list[tuple[int, dict]] = []
    for row in rows:
        score = 0
        if row.get("connected"):
            score += 100
        if not _is_wifi(row):
            score += 40
        if not _is_virtual(row):
            score += 30
        name = row.get("name") or ""
        if "以太网" in name or "Ethernet" in name or "eth" in name.lower():
            score += 20
        if row.get("ips"):
            score += 5
        scored.append((score, row))
    scored.sort(key=lambda item: item[0], reverse=True)
    return scored[0][1] if scored else None


def net_status(hint: str | None = None, preferred: str = "") -> dict:
    """查询是否已具备推板所需地址。"""
    target = parse_pc_ip_hint(hint)
    adapters = list_adapters()
    holders = [
        row for row in adapters
        if target["ip"] in (row.get("ips") or [])
    ]
    chosen = holders[0] if holders else pick_adapter(adapters, preferred)
    ready = bool(holders)
    return {
        "ready": ready,
        "target": target,
        "adapter": chosen,
        "holders": holders,
        "adapters": adapters,
        "message": (
            "网口已就绪：%s 上有 %s" % (holders[0]["name"], target["cidr"])
            if holders
            else "尚未配置 %s。可点「自动配置 PC 网口」追加到有线网卡。" % target["cidr"]
        ),
    }


def _is_admin() -> bool:
    if sys.platform != "win32":
        return False
    try:
        import ctypes

        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def _netsh_direct(adapter: str, ip: str, mask: str) -> tuple[int, str]:
    proc = subprocess.run(
        [
            "netsh", "interface", "ipv4", "add", "address",
            "name=%s" % adapter,
            "addr=%s" % ip,
            "mask=%s" % mask,
        ],
        capture_output=True,
        text=True,
        timeout=60,
        encoding="utf-8",
        errors="replace",
    )
    text = ((proc.stdout or "") + "\n" + (proc.stderr or "")).strip()
    if _already_exists(text):
        return 0, text
    return proc.returncode, text


def _already_exists(text: str) -> bool:
    folded = (text or "").lower()
    return "already exists" in folded or "已存在" in text or "对象已经存在" in text


def _netsh_add(adapter: str, ip: str, mask: str) -> tuple[int, str]:
    """追加地址。非管理员时写脚本再提权，避免 UAC 窗口一过就把失败当成完成。"""
    if _is_admin():
        return _netsh_direct(adapter, ip, mask)

    work = Path(tempfile.mkdtemp(prefix="fpga_net_"))
    script = work / "add_ip.ps1"
    name_file = work / "name.txt"
    result = work / "result.txt"
    name_file.write_text(adapter, encoding="utf-8-sig")
    result.write_text("", encoding="utf-8")
    script.write_text(
        "\n".join([
            "$ErrorActionPreference = 'Continue'",
            "$name = (Get-Content -LiteralPath '%s' -Encoding UTF8 -Raw).Trim()" % str(name_file).replace("'", "''"),
            "$output = & netsh.exe interface ipv4 add address name=$name addr=%s mask=%s 2>&1 | Out-String" % (ip, mask),
            "$code = $LASTEXITCODE",
            "if ($null -eq $code) { $code = 1 }",
            "$text = ([string]$output).Trim()",
            "if ($text -match 'already exists|已存在|对象已经存在') { $code = 0 }",
            "Set-Content -LiteralPath '%s' -Value ($code.ToString() + \"`n\" + $text) -Encoding UTF8" % str(result).replace("'", "''"),
            "exit $code",
        ]),
        encoding="utf-8-sig",
    )
    # ArgumentList 必须是一整段字符串。拆成数组时，提权接口会返回“数据无效”(错误 13)。
    ps_script = str(script).replace("'", "''")
    launch = (
        "$p = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru "
        "-ArgumentList '-NoProfile -ExecutionPolicy Bypass -File %s'; "
        "if ($null -eq $p) { exit 1 }; "
        "if ($null -eq $p.ExitCode) { exit 0 }; exit $p.ExitCode"
    ) % ps_script
    code, out, err = _run_ps(launch, timeout=180)
    detail = ((out or "") + "\n" + (err or "")).strip()
    try:
        raw = result.read_text(encoding="utf-8-sig", errors="replace").strip()
    except OSError as exc:
        raw = ""
        detail = (detail + "\n无法读取配置结果：%s" % exc).strip()
    if raw:
        lines = raw.splitlines()
        head = lines[0].strip().lstrip("\ufeff").lstrip("-")
        if head.isdigit():
            code = int(head)
            detail = "\n".join(lines[1:]).strip() or detail
        else:
            detail = raw
    shutil.rmtree(work, ignore_errors=True)
    if _already_exists(detail):
        return 0, detail
    return code, detail


def configure_pc_board_net(
    hint: str | None = None,
    adapter_name: str = "",
) -> dict:
    """向网卡追加 pc_ip_hint。已存在则直接返回 ready。"""
    if sys.platform != "win32":
        return {"ok": False, "ready": False, "message": "目前只支持在 Windows 上自动配置网口。"}

    status = net_status(hint=hint, preferred=adapter_name)
    target = status["target"]
    if status["ready"]:
        return {
            "ok": True,
            "ready": True,
            "changed": False,
            "adapter": status["holders"][0]["name"],
            "target": target,
            "message": status["message"],
        }

    adapters = status["adapters"]
    chosen = None
    if adapter_name:
        for row in adapters:
            if row["name"] == adapter_name:
                chosen = row
                break
        if chosen is None:
            return {
                "ok": False,
                "ready": False,
                "message": "找不到网卡「%s」。" % adapter_name,
                "adapters": adapters,
                "target": target,
            }
    else:
        chosen = pick_adapter(adapters)
    connected = [row for row in adapters if row.get("connected") and not _is_virtual(row)]
    if chosen is None or not chosen.get("connected"):
        wired = [row for row in connected if not _is_wifi(row)]
        if wired:
            chosen = wired[0]
        elif connected:
            chosen = connected[0]
    if chosen is None or not chosen.get("connected"):
        return {
            "ok": False,
            "ready": False,
            "message": "没有已连接的网卡。板子显示已连接只说明网线插上了，请在电脑的网络连接里确认这块网卡是「已连接」。",
            "adapters": adapters,
            "target": target,
        }
    if _is_wifi(chosen):
        wired = [row for row in connected if not _is_wifi(row)]
        if wired:
            chosen = wired[0]

    code, detail = _netsh_add(chosen["name"], target["ip"], target["mask"])
    after = net_status(hint=target["cidr"], preferred=chosen["name"])
    for _ in range(8):
        if after["ready"]:
            break
        time.sleep(0.5)
        after = net_status(hint=target["cidr"], preferred=chosen["name"])
    if after["ready"]:
        return {
            "ok": True,
            "ready": True,
            "changed": True,
            "adapter": chosen["name"],
            "target": target,
            "message": "已在「%s」上追加 %s。原有地址保留。" % (chosen["name"], target["cidr"]),
            "detail": detail,
        }

    hint_admin = ""
    if code != 0:
        hint_admin = " 若弹出了用户账户控制却点了「否」，请再试一次并选择「是」。"
    return {
        "ok": False,
        "ready": False,
        "changed": False,
        "adapter": chosen["name"],
        "target": target,
        "message": (
            "未能在「%s」上添加 %s。%s%s"
            % (
                chosen["name"],
                target["cidr"],
                hint_admin,
                (" " + detail[:240]) if detail else "",
            )
        ).strip(),
        "detail": detail or ("netsh 退出码 %d" % code),
        "adapters": adapters,
    }


def board_reachable_hint(board_ip: str | None = None) -> str:
    """给推送页用的一句提示。"""
    from host_skill import load_profile

    profile = load_profile()
    board = (board_ip or str(profile.get("board_ip") or "192.168.1.10")).strip()
    try:
        status = net_status()
    except Exception as exc:
        return "检查本机网口失败：%s。板地址 %s。" % (exc, board)
    if status["ready"]:
        return "%s；推送目标板 %s。请先打开板上「文件传输」。" % (status["message"], board)
    return "%s 板地址 %s。请先打开板上「文件传输」。" % (status["message"], board)
