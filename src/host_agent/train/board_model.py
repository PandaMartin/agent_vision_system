"""竞赛板 12_ov5640_ncnn 的 SD 卡模型档案约定。

板上扫描 /models/<文件夹>/，下列文件应齐全：
  model.cfg        key=value 档案
  model.param.bin  ncnn2mem 生成的二进制结构
  model.bin        权重
  labels.txt       每行一个类别名
  image_ops.cfg    上位机积木记录（板子不读；封包时写进 model.mod）
"""
from __future__ import annotations

import os
import re
import shutil
from collections import Counter
from pathlib import Path

from common import (
    BOARD_SD_MODELS,
    FPGA_DEPLOYMENT,
    MODELS_OUTPUT,
    PROJECT_ROOT,
    ensure_dir,
    format_class_name,
    format_class_names,
    model_stem,
    read_json,
    write_json,
)

BOARD_MODELS = PROJECT_ROOT / "models"
CFG_NAME = "model.cfg"
PARAM_NAME = "model.param.bin"
WEIGHT_NAME = "model.bin"
LABELS_NAME = "labels.txt"
IMAGE_OPS_NAME = "image_ops.cfg"
FOUR_FILES = (CFG_NAME, PARAM_NAME, WEIGHT_NAME, LABELS_NAME)
SD_KEEP_FILES = FOUR_FILES + (IMAGE_OPS_NAME,)

# 竞赛板 mobilenetv2_ssdlite_voc 已验证：BGR、(x-127.5)/127.5
BOARD_DETECT_PIXEL = "bgr"
BOARD_DETECT_MEAN = (127.5, 127.5, 127.5)
BOARD_DETECT_NORM = (1.0 / 127.5, 1.0 / 127.5, 1.0 / 127.5)

_BLOB_RE = re.compile(r"const int BLOB_([A-Za-z0-9_]+)\s*=\s*(-?\d+)")
_STAMP_RE = re.compile(r"_?\d{8}_\d{6}$")
_QUANT_SUFFIXES = (
    "_fp32_(int8_failed)",
    "_fp32__int8_failed",
    "_fp32_int8_failed",
    "_int8_failed",
    "_float32_int8",
    "_float32",
    "_fp32",
    "_int8",
)


CFG_HEADER = """# ============================================================
# 上位机模型档案（18_cam_lcd_hdmi_servo_net 不解析本文件）
# 板上 /models/<短名>/ 只认四件: model.param.bin、model.bin、
# labels.txt、model.mod。本文件留在 models/ 完整包里，供 PC
# 对拍，并作为生成 model.mod 的参数来源。
#
# 字段说明:
#   name       上位机文件夹身份(不是中文显示名)
#   input_size 正方形输入边长
#   pixel      bgr|rgb|bgr2rgb            blob_in/out 来自ncnn2mem的 *_id.h
#   mean/norm  预处理: 先减均值, 再乘norm(1=不缩放)
#   post       classify|detect|retinaface
#   --- post=classify ---   topk 打印Top-K数
# ============================================================
"""

# 板端 TFTP 路径缓冲 48 字节: "0:/models/"+名+"/model.param.bin" 最长 47
BOARD_NAME_MAX = 20
MOD_NAME = "model.mod"
BOARD_DATA_FILES = (PARAM_NAME, WEIGHT_NAME, LABELS_NAME)

_DATASET_SHORT = {
    "wider_face": "wd_face",
    "fashion_mnist": "fmnist",
    "cifar10": "cifar10",
    "cifar100": "cifar100",
    "mnist": "mnist",
    "kmnist": "kmnist",
    "flowers": "flowers",
    "rps": "rps",
    "hands": "hands",
    "coco": "coco",
    "custom": "custom",
}
_ARCH_SHORT = {
    "retinaface": "ret",
    "retinaface_mnet": "ret",
    "ssdlite": "ssd",
    "mobilenet_ssdlite": "ssd",
    "squeezenet": "sqz",
    "mobilenet": "mnv2",
    "mobilenetv2": "mnv2",
    "simple_cnn": "cnn",
    "deep_cnn": "dcnn",
    "mnist_deep_cnn": "dcnn",
    "resnet18": "r18",
}


def quant_token(quant: str) -> str:
    q = (quant or "fp32").lower().replace(" ", "_")
    if "failed" in q or q in ("none", "float32", "fp32"):
        return "fp32"
    if q == "qat" or q.startswith("qat"):
        return "qat"
    if "int8" in q:
        return "int8"
    return "fp32"


def pack_folder_name(dataset: str, arch: str, quant: str) -> str:
    """上位机 models/ 目录名。板上目录用 board_folder_name，不超过 20 字符。"""
    text = f"{model_stem(dataset, arch)}_{quant_token(quant)}".strip("_")
    return text[:47] or "model"


def _ascii_token(text: str, limit: int) -> str:
    raw = "".join(ch for ch in (text or "").lower() if ch.isascii() and ch.isalnum())
    return (raw or "m")[:limit]


def board_folder_name(dataset: str, arch: str, quant: str) -> str:
    """SD 卡 /models/<名>/。全路径必须短于 48 字节，名字不超过 20。"""
    ds_key = (dataset or "").strip().lower()
    ar_key = (arch or "").strip().lower()
    ds = _DATASET_SHORT.get(ds_key) or _ascii_token(ds_key, 8)
    ar = _ARCH_SHORT.get(ar_key) or _ascii_token(ar_key, 4)
    name = f"{ds}_{ar}_{quant_token(quant)}"
    name = "".join(ch for ch in name if ch.isascii() and (ch.isalnum() or ch == "_")).strip("_")
    if len(name) > BOARD_NAME_MAX:
        name = name[:BOARD_NAME_MAX].rstrip("_")
    return name or "model"


def ascii_board_label(text: str, index: int) -> str:
    """板上视频字库只画 ASCII。中文类别名落到 c<序号>。"""
    raw = str(text or "").strip()
    if raw.lower() in ("background", "bg", "__background__"):
        return "background"
    kept = "".join(ch if ch.isascii() and (ch.isalnum() or ch in " _-/.") else " " for ch in raw)
    kept = " ".join(kept.split())
    if not kept:
        return f"c{index}"
    return kept[:24]


def write_board_labels(src: Path, dest: Path) -> None:
    names = load_labels(src)
    if not names:
        names = ["background"]
    lines = [ascii_board_label(name, i) for i, name in enumerate(names)]
    dest.write_text("\n".join(lines) + "\n", encoding="ascii")


def as_triple(values) -> tuple[float, float, float]:
    if values is None:
        return (0.0, 0.0, 0.0)
    if isinstance(values, (int, float)):
        v = float(values)
        return (v, v, v)
    seq = [float(x) for x in values]
    if not seq:
        return (0.0, 0.0, 0.0)
    if len(seq) == 1:
        return (seq[0], seq[0], seq[0])
    if len(seq) == 2:
        return (seq[0], seq[1], seq[1])
    return (seq[0], seq[1], seq[2])


def mean_norm_from_train(normalize: bool) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    """对齐板上 substract_mean_normalize：像素先是 0–255。

    Keras normalize=True 时训练输入是 /255，等价 mean=0、norm=1/255。
    """
    if normalize:
        n = 1.0 / 255.0
        return (0.0, 0.0, 0.0), (n, n, n)
    return (0.0, 0.0, 0.0), (1.0, 1.0, 1.0)


def parse_blob_ids(id_header: Path, input_blob: str, output_blob: str) -> tuple[int, int]:
    text = Path(id_header).read_text(encoding="utf-8", errors="replace")
    blobs: dict[str, int] = {}
    for match in _BLOB_RE.finditer(text):
        blobs[match.group(1)] = int(match.group(2))
    if not blobs:
        return 0, 0

    def lookup(name: str, default: int) -> int:
        key = re.sub(r"[^A-Za-z0-9_]", "_", name or "")
        if key in blobs:
            return blobs[key]
        for cand, val in blobs.items():
            if cand.endswith("_" + key) or cand == key:
                return val
        return default

    blob_in = lookup(input_blob, 0)
    blob_out = lookup(output_blob, max(blobs.values()))
    return blob_in, blob_out


def parse_named_blobs(id_header: Path, names: list[str]) -> dict[str, int]:
    text = Path(id_header).read_text(encoding="utf-8", errors="replace")
    blobs: dict[str, int] = {}
    for match in _BLOB_RE.finditer(text):
        blobs[match.group(1)] = int(match.group(2))
    out = {}
    for name in names:
        key = re.sub(r"[^A-Za-z0-9_]", "_", name or "")
        if key in blobs:
            out[name] = blobs[key]
            continue
        found = None
        for cand, val in blobs.items():
            if cand.endswith("_" + key) or key in cand:
                found = val
                break
        out[name] = found if found is not None else -1
    return out


def write_labels(path: Path, class_names: list[str], post: str = "classify") -> Path:
    names = format_class_names(class_names)
    if not names:
        names = [format_class_name(i, i) for i in range(10)]
    if str(post or "").lower() in ("detect", "retinaface"):
        head = (names[0] if names else "").strip().lower()
        if head not in ("background", "bg", "__background__"):
            names = ["background"] + names
    Path(path).write_text("\n".join(names) + "\n", encoding="utf-8")
    return Path(path)


def write_model_cfg(path: Path, *, name: str, input_size: int, pixel: str,
                    blob_in: int, blob_out: int, mean, norm, post: str = "classify",
                    topk: int = 3, channels: int | None = None,
                    extra_comment: str = "", score_th: float | None = None,
                    nms_iou_th: float | None = None, max_keep: int | None = None,
                    extra_fields: dict | None = None) -> Path:
    mean_t = as_triple(mean)
    norm_t = as_triple(norm)
    lines = [CFG_HEADER.rstrip(), ""]
    if extra_comment:
        lines.append(f"# {extra_comment}")
    lines.extend(
        [
            f"name = {name[:31]}",
            f"input_size = {int(input_size)}",
            f"pixel = {pixel}",
            f"blob_in = {int(blob_in)}",
            f"blob_out = {int(blob_out)}",
            f"mean = {mean_t[0]:g},{mean_t[1]:g},{mean_t[2]:g}",
            f"norm = {norm_t[0]:g},{norm_t[1]:g},{norm_t[2]:g}",
            f"post = {post}",
            f"topk = {int(topk)}",
        ]
    )
    if channels is not None:
        lines.append(f"channels = {int(channels)}")
    if str(post or "").lower() in ("detect", "retinaface"):
        if score_th is not None:
            lines.append(f"score_th = {float(score_th):g}")
        if nms_iou_th is not None:
            lines.append(f"nms_iou_th = {float(nms_iou_th):g}")
        if max_keep is not None:
            lines.append(f"max_keep = {int(max_keep)}")
    if extra_fields:
        for key, val in extra_fields.items():
            if val is None:
                continue
            lines.append(f"{key} = {val}")
    lines.append("")
    Path(path).write_text("\n".join(lines), encoding="utf-8")
    return Path(path)


def parse_model_cfg(path: Path) -> dict:
    cfg: dict = {
        "backend": "ncnn",
        "pixel": "bgr",
        "post": "classify",
        "topk": 3,
        "blob_in": 0,
        "input_blob": "data",
    }
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("[") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        key, val = key.strip(), val.strip()
        if key == "name":
            cfg["name"] = val
        elif key == "display_name":
            cfg["display_name"] = val
        elif key == "input_size":
            cfg["image_size"] = int(float(val))
            cfg["input_size"] = cfg["image_size"]
        elif key == "pixel":
            cfg["pixel"] = val.lower()
        elif key == "blob_in":
            cfg["blob_in"] = int(val)
        elif key == "blob_out":
            cfg["blob_out"] = int(val)
        elif key == "mean":
            cfg["mean"] = [float(x) for x in val.split(",") if x.strip() != ""]
        elif key == "norm":
            cfg["norm"] = [float(x) for x in val.split(",") if x.strip() != ""]
        elif key == "post":
            cfg["post"] = val.lower()
        elif key == "topk":
            cfg["topk"] = int(val)
        elif key == "channels":
            cfg["channels"] = int(val)
        elif key == "score_th":
            cfg["score"] = float(val)
        elif key == "nms_iou_th":
            cfg["nms_iou_th"] = float(val)
        elif key == "max_keep":
            cfg["max_keep"] = int(val)
        elif key.startswith("blob_"):
            try:
                cfg[key] = int(val)
            except ValueError:
                cfg[key] = val
    size = int(cfg.get("image_size") or 28)
    ch = int(cfg.get("channels") or 3)
    cfg["input_shape_nchw"] = [ch, size, size]
    cfg["normalize"] = True
    norms = cfg.get("norm") or [1, 1, 1]
    if all(abs(float(n) - 1.0) < 1e-6 for n in norms):
        cfg["normalize"] = False
    return cfg


def load_labels(path: Path) -> list[str]:
    if not Path(path).exists():
        return []
    return [
        ln.strip()
        for ln in Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
        if ln.strip()
    ]


def is_board_pack(folder: Path) -> bool:
    folder = Path(folder)
    return (folder / CFG_NAME).exists() and (folder / WEIGHT_NAME).exists()


def pack_post(folder: Path) -> str:
    cfg = Path(folder) / CFG_NAME
    if not cfg.exists():
        return ""
    for raw in cfg.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip().replace(" ", "")
        if line.startswith("post="):
            return line.split("=", 1)[1].lower()
    return "classify"


def reference_model_roots() -> list[Path]:
    """本仓库 models/ 以及竞赛工程 12_ov5640_ncnn/models。"""
    env = (os.environ.get("OV5640_NCNN_MODELS") or os.environ.get("FPGA_BOARD_MODELS") or "").strip()
    candidates = []
    if env:
        candidates.append(Path(env))
    candidates.extend(
        [
            Path(r"F:\AWork\12_ov5640_ncnn\models"),
            PROJECT_ROOT.parent.parent / "12_ov5640_ncnn" / "models",
            PROJECT_ROOT.parent / "12_ov5640_ncnn" / "models",
            BOARD_MODELS,
        ]
    )
    seen = set()
    roots = []
    for path in candidates:
        if not path.exists() or not path.is_dir():
            continue
        key = str(path.resolve())
        if key in seen:
            continue
        seen.add(key)
        roots.append(path)
    return roots


def list_board_packs() -> list[Path]:
    packs = []
    for root in reference_model_roots():
        for child in root.iterdir():
            if child.is_dir() and child.name != "presets" and is_board_pack(child):
                packs.append(child)
    return packs


def has_ncnn_weights(folder: Path) -> bool:
    folder = Path(folder)
    if not folder.is_dir():
        return False
    if is_board_pack(folder):
        return True
    if (folder / PARAM_NAME).exists() and (folder / WEIGHT_NAME).exists():
        return True
    params = list(folder.glob("*.param"))
    bins = [p for p in folder.glob("*.bin") if p.name != PARAM_NAME]
    return bool(params and bins)


def _source_label(path: Path) -> str:
    text = str(path.resolve())
    if "12_ov5640_ncnn" in text.replace("/", "\\"):
        return "竞赛板"
    if "fpga_deployment" in text:
        return "导出包"
    if "models_output" in text:
        return "训练产物"
    return "本仓库"


def _iter_weight_roots() -> list[Path]:
    folders: list[Path] = []
    for root in reference_model_roots():
        folders.extend(p for p in root.iterdir() if p.is_dir())
    for extra in (FPGA_DEPLOYMENT, MODELS_OUTPUT):
        if not extra.exists():
            continue
        for child in extra.iterdir():
            if not child.is_dir():
                continue
            folders.append(child)
            folders.extend(p for p in child.iterdir() if p.is_dir())
    return folders


def _normalize_pack_stem(name: str) -> str:
    text = (name or "").lower().replace("-", "_")
    text = _STAMP_RE.sub("", text)
    for suffix in _QUANT_SUFFIXES:
        if text.endswith(suffix):
            text = text[: -len(suffix)]
            break
    parts = [p for p in text.split("_") if p]
    collapsed = []
    for part in parts:
        if collapsed and part == collapsed[-1]:
            continue
        collapsed.append(part)
    return "_".join(collapsed)


def _infer_dataset(folder: Path, cfg: dict, extra: dict) -> str:
    dataset = str(extra.get("dataset") or "").strip().lower()
    if dataset:
        return dataset
    stem = _normalize_pack_stem(str(cfg.get("name") or folder.name))
    for key in ("fashion_mnist", "cifar100", "cifar10", "kmnist", "flowers", "rps", "hands", "coco", "wider_face", "mnist"):
        if stem.startswith(key) or f"_{key}_" in f"_{stem}_":
            return key
    return ""


def _pack_identity(folder: Path, cfg: dict, extra: dict, post: str, quantize: str = "") -> tuple:
    sidecar = str(extra.get("pack_stem") or extra.get("identity_stem") or "").strip()
    if sidecar:
        stem = _normalize_pack_stem(sidecar)
    else:
        dataset = _infer_dataset(folder, cfg, extra)
        custom_path = str(extra.get("custom_path") or "").strip()
        if dataset in ("", "custom") and custom_path:
            dataset = Path(custom_path).name
        arch = str(extra.get("arch") or "").strip()
        if not arch or " " in arch or len(arch) > 40:
            arch = ""
        if dataset and arch:
            stem = model_stem(dataset, arch)
        else:
            stem = _normalize_pack_stem(str(cfg.get("name") or folder.name))
    quant = (quantize or _guess_quantize(folder, extra) or "").lower()
    return (stem, post or "classify", quant)


def _pack_richness(folder: Path, item: dict) -> tuple:
    """越大越优先：层结构可读、int8、非失败回退。"""
    score = 0
    if (folder / "graph.param").exists() or list(folder.glob("*.param")):
        score += 100
    if (folder / "train_info.json").exists() or (folder / "deployment_config.json").exists():
        score += 40
    name = folder.name.lower()
    if "failed" in name:
        score -= 120
    if item.get("quantize") == "int8" or ("int8" in name and "failed" not in name):
        score += 50
    if item.get("source") == "竞赛板":
        score += 8
    return (score, item.get("mtime") or 0)


def _read_display_name(folder: Path, cfg: dict | None = None, extra: dict | None = None) -> str:
    extra = extra or {}
    cfg = cfg or {}
    for src in (extra, cfg):
        for key in ("display_name", "alias"):
            val = str(src.get(key) or "").strip()
            if val:
                return val
    path = Path(folder) / "display_name.txt"
    if path.exists():
        try:
            val = path.read_text(encoding="utf-8").strip().splitlines()[0].strip()
        except Exception:
            val = ""
        if val:
            return val
    return ""


def _pretty_pack_name(item: dict) -> str:
    custom = str(item.get("display_name") or "").strip()
    if custom:
        return custom
    arch = item.get("arch_title") or item.get("name") or item.get("folder") or "模型"
    dataset = str(item.get("dataset") or "").strip()
    ds_map = {
        "mnist": "MNIST",
        "fashion_mnist": "Fashion-MNIST",
        "kmnist": "KMNIST",
        "cifar10": "CIFAR-10",
        "cifar100": "CIFAR-100",
        "flowers": "Flowers",
        "rps": "石头剪刀布",
        "hands": "手势识别",
        "coco": "COCO",
        "wider_face": "WIDER FACE 人脸检测",
        "custom": "自定义",
    }
    ds = ds_map.get(dataset.lower(), "")
    if not ds and dataset and dataset.lower() not in ("model",):
        ds = dataset.replace("_", " ")
    quant = str(item.get("quantize") or "").upper()
    parts = []
    if ds and ds.lower() not in str(arch).lower():
        parts.append(ds)
    if arch:
        parts.append(arch)
    if quant:
        parts.append(quant)
    return " · ".join(parts) or str(item.get("folder"))


def _is_train_run_dir(folder: Path) -> bool:
    folder = Path(folder)
    if not MODELS_OUTPUT.exists():
        return False
    try:
        folder.resolve().relative_to(MODELS_OUTPUT.resolve())
    except ValueError:
        return False
    return (folder / "model.pt").exists() or (folder / "model.pth").exists()


def _folder_stamp(name: str) -> str:
    match = re.search(r"(\d{8}_\d{6})", str(name or ""))
    return match.group(1) if match else ""


def _format_run_stamp(text: str) -> str:
    stamp = _folder_stamp(text)
    if not stamp or "_" not in stamp:
        return ""
    day, tod = stamp.split("_", 1)
    if len(day) != 8 or len(tod) < 4:
        return ""
    return f"{day[4:6]}-{day[6:8]} {tod[:2]}:{tod[2:4]}"


def _weight_mtime(folder: Path) -> float:
    folder = Path(folder)
    weight = folder / WEIGHT_NAME
    if weight.exists():
        return float(weight.stat().st_mtime)
    return float(folder.stat().st_mtime) if folder.exists() else 0.0


def _is_external_competition_pack(folder: Path) -> bool:
    text = str(Path(folder).resolve()).replace("/", "\\")
    work_models = str(BOARD_MODELS.resolve()).replace("/", "\\")
    if text.startswith(work_models.rstrip("\\") + "\\") or text == work_models:
        return False
    return "12_ov5640_ncnn" in text


def _collect_weight_items(*, include_train_runs: bool = False) -> list[dict]:
    seen = set()
    items = []
    for folder in _iter_weight_roots():
        if not folder.exists() or folder.name == "presets" or folder.name.startswith(("_", ".")):
            continue
        key = str(folder.resolve())
        if key in seen:
            continue
        seen.add(key)
        if not include_train_runs and _is_train_run_dir(folder):
            continue
        if not has_ncnn_weights(folder):
            continue
        cfg = {}
        cfg_path = folder / CFG_NAME
        if cfg_path.exists():
            cfg = parse_model_cfg(cfg_path)
        extra = _load_sidecar_info(folder)
        labels = load_labels(folder / LABELS_NAME)
        if not labels and extra.get("class_names"):
            labels = [str(x) for x in extra["class_names"]]
        post = pack_post(folder) or str(cfg.get("post") or extra.get("post") or "classify")
        kind = "检测" if post in ("detect", "retinaface") else "分类"
        size = int(cfg.get("image_size") or cfg.get("input_size") or extra.get("image_size") or 0)
        weight = folder / WEIGHT_NAME
        if not weight.exists():
            bins = [p for p in folder.glob("*.bin") if p.name != PARAM_NAME]
            weight = bins[0] if bins else weight
        kb = weight.stat().st_size / 1024 if weight.exists() else 0
        source = _source_label(folder)
        quantize = _guess_quantize(folder, extra)
        arch_title = _guess_arch_name(folder, cfg, extra)
        dataset = _infer_dataset(folder, cfg, extra)
        identity = _pack_identity(folder, cfg, extra, post, quantize)
        stamp = _format_run_stamp(folder.name) or _format_run_stamp(
            str(extra.get("run_dir") or extra.get("name") or "")
        )
        items.append({
            "path": str(folder.resolve()),
            "name": "",
            "display_name": _read_display_name(folder, cfg, extra),
            "folder": folder.name,
            "post": post,
            "kind": kind,
            "input_size": size,
            "source": source,
            "bytes": int(weight.stat().st_size) if weight.exists() else 0,
            "mtime": folder.stat().st_mtime,
            "kb": kb,
            "quantize": quantize,
            "arch_title": arch_title,
            "dataset": dataset,
            "identity": identity,
            "folder_name": str(extra.get("folder_name") or "").strip(),
            "stamp": stamp,
            "has_graph": (folder / "graph.param").exists() or bool(list(folder.glob("*.param"))),
        })
    return items


def catalog_weight_packs() -> list[dict]:
    """检索可用 ncnn 权重；同一套导出只留一份，未导出的新训练单独列出。"""
    items = _collect_weight_items(include_train_runs=True)

    grouped: dict[tuple, dict] = {}
    train_runs: dict[tuple, list[dict]] = {}
    for item in items:
        ident = item["identity"]
        folder = Path(item["path"])
        if _is_train_run_dir(folder):
            train_runs.setdefault(ident, []).append(item)
            continue
        prev = grouped.get(ident)
        if prev is None or _pack_richness(folder, item) > _pack_richness(Path(prev["path"]), prev):
            grouped[ident] = item
    for ident, runs in train_runs.items():
        best = max(runs, key=lambda it: (_weight_mtime(Path(it["path"])), it.get("mtime") or 0))
        export = grouped.get(ident)
        if export is None:
            grouped[ident] = best
            continue
        if _weight_mtime(Path(best["path"])) > _weight_mtime(Path(export["path"])) + 1.0:
            grouped[(*ident, Path(best["path"]).name)] = best
    result = []
    used_labels = set()
    used_names = set()
    for item in grouped.values():
        item["name"] = _pretty_pack_name(item)
        if not str(item.get("display_name") or "").strip() and item.get("stamp"):
            item["name"] = f"{item['name']} · {item['stamp']}"
        if item["name"] in used_names:
            item["name"] = f"{item['name']} · {item['folder']}"
        used_names.add(item["name"])
        size_text = f"{item['input_size']}" if item.get("input_size") else "?"
        label = f"{item['name']}（{item['kind']} · {size_text}）"
        if label in used_labels:
            label = f"{label} · {item['folder']}"
        used_labels.add(label)
        item["label"] = label
        item["option"] = f"使用权重：{label}"
        result.append(item)
    result.sort(key=lambda x: x["mtime"], reverse=True)
    return result


def weight_option_label(item: dict) -> str:
    return item.get("option") or f"使用权重：{item.get('name')}"


def _guess_arch_name(folder: Path, cfg: dict, extra: dict) -> str:
    arch = str(extra.get("arch") or "").strip()
    if arch and " " not in arch and len(arch) <= 48:
        mapping = {
            "squeezenet": "SqueezeNet",
            "mobilenet": "MobileNetV2",
            "mobilenetv2": "MobileNetV2",
            "simple_cnn": "简单 CNN",
            "deep_cnn": "深层 CNN",
            "mnist_deep_cnn": "深层 CNN",
            "resnet18": "ResNet-18",
            "retinaface": "RetinaFace",
            "retinaface_mnet": "RetinaFace",
        }
        key = arch.lower()
        if key in mapping:
            return mapping[key]
        return arch.replace("_", " ")
    text = f"{cfg.get('name') or ''} {folder.name} {extra.get('arch') or ''}".lower()
    post = str(cfg.get("post") or extra.get("post") or "").lower()
    if "retina" in text or post == "retinaface":
        return "RetinaFace"
    if "squeezenet" in text or "squeeze" in text:
        return "SqueezeNet"
    if "ssdlite" in text or "ssd" in text or (post == "detect" and "mobilenet" in text):
        return "MobileNetV2 SSDLite"
    if "mobilenet" in text:
        return "MobileNetV2"
    if "resnet" in text:
        return "ResNet-18"
    if "deep_cnn" in text or "deep-cnn" in text:
        return "深层 CNN"
    if "simple" in text or "cnn" in text:
        return "简单 CNN"
    return cfg.get("name") or folder.name


def auto_pack_display_name(
    *,
    dataset: str = "",
    arch: str = "",
    quant: str = "",
    run_dir: Path | str | None = None,
) -> str:
    """导出后的默认识别名：数据集 · 架构 · 量化 · 训练时间。"""
    run = Path(run_dir) if run_dir else Path()
    extra = {"arch": arch, "dataset": dataset, "quantize": quant}
    probe = run if str(run) and run.exists() else Path(str(arch or "model"))
    item = {
        "display_name": "",
        "arch_title": _guess_arch_name(probe, {}, extra),
        "dataset": dataset,
        "quantize": (quant or "").lower(),
        "name": arch,
        "folder": run.name,
    }
    base = _pretty_pack_name(item)
    stamped = _format_run_stamp(run.name)
    if stamped:
        return f"{base} · {stamped}"
    return base


def _guess_quantize(folder: Path, extra: dict) -> str:
    q = str(extra.get("quantize") or extra.get("quant") or "").strip().lower()
    if "failed" in q:
        return "fp32"
    if "int8" in q:
        return "int8"
    if q in ("fp32", "float32", "none"):
        return "fp32"
    if q:
        return q
    text = folder.name.lower()
    if "failed" in text:
        return "fp32"
    if "qat" in text:
        return "qat"
    if "int8" in text:
        return "int8"
    if "fp16" in text or "float16" in text:
        return "float16"
    return "fp32"


def _estimate_params(weight_bytes: int, quantize: str) -> int:
    if weight_bytes <= 0:
        return 0
    key = (quantize or "").lower()
    if "int8" in key or key == "qat":
        return int(weight_bytes)
    if "16" in key:
        return max(1, weight_bytes // 2)
    return max(1, weight_bytes // 4)


_KNOWN_ARCHITECTURES = {
    "squeezenet": {
        "summary": "SqueezeNet 1.1 · Fire 模块分类 · 建议 227×227 BGR",
        "layers": [
            "Input  227×227×3",
            "Convolution  conv1  64, 3×3, stride 2",
            "Pooling  pool1  3×3",
            "Fire2  squeeze 16 → expand 64 + 64 Concat",
            "Fire3  squeeze 16 → expand 64 + 64 Concat",
            "Pooling  pool3",
            "Fire4  squeeze 32 → expand 128 + 128 Concat",
            "Fire5  squeeze 32 → expand 128 + 128 Concat",
            "Pooling  pool5",
            "Fire6  squeeze 48 → expand 192 + 192 Concat",
            "Fire7  squeeze 48 → expand 192 + 192 Concat",
            "Fire8  squeeze 64 → expand 256 + 256 Concat",
            "Fire9  squeeze 64 → expand 256 + 256 Concat",
            "Dropout  0.5",
            "Convolution  conv10  类别数, 1×1",
            "Pooling  global average",
            "Softmax",
        ],
    },
    "mobilenet": {
        "summary": "MobileNetV2 SSDLite 检测 · 建议 300×300 · 骨干 IR 块 + Extra + loc/conf 头",
        "layers": [
            "Input  300×300×3 BGR，mean=127.5，norm=1/127.5（对齐板上 VOC SSDLite）",
            "Convolution  stem  32, 3×3, stride 2 + BN + Clip(ReLU6)",
            "Inverted Residual  t=1  c=16  ×1  stride 1",
            "Inverted Residual  t=6  c=24  ×2  stride 2",
            "Inverted Residual  t=6  c=32  ×3  stride 2",
            "Inverted Residual  t=6  c=64  ×4  stride 2",
            "Inverted Residual  t=6  c=96  ×3  stride 1  （SSD 特征图）",
            "Inverted Residual  t=6  c=160 ×3  stride 2",
            "Inverted Residual  t=6  c=320 ×1  stride 1",
            "Convolution  1280, 1×1 + BN + ReLU6",
            "SSD Extra  多尺度附加卷积，生成检测特征图",
            "SSD 分类头  Depthwise 预测类别分数，导出后 Softmax",
            "SSD 回归头  Depthwise 预测边框偏移",
            "DetectionOutput  N×6（label, score, x1, y1, x2, y2）",
            "NMS 后处理  score_th=0.50，IoU=0.45，max_keep=32",
        ],
    },
    "retinaface": {
        "summary": "RetinaFace MobileNet0.25 人脸检测 · 320×320 RGB letterbox · fp32",
        "layers": [
            "Input  320×320×3 RGB letterbox，mean=0，norm=1",
            "MobileNet0.25  骨干，输出 stride 8 / 16 / 32",
            "FPN  1×1 降维 + 最近邻上采样 Add",
            "SSH  三尺度上下文卷积 Concat + ReLU",
            "cls 头  1×1 ×3，通道 [bg0,bg1,face0,face1]",
            "bbox 头  1×1 ×3，2 锚 × 4",
            "landmark 头  1×1 ×3，2 锚 × 10",
            "导出 Softmax  Reshape→Softmax→Reshape",
            "NMS  score_th=0.50，IoU=0.40，max_keep=32",
        ],
    },
    "simple_cnn": {
        "summary": "简单 CNN · 两层卷积 + 池化，适合小图分类",
        "layers": [
            "Input",
            "Convolution + ReLU",
            "Pooling",
            "Convolution + ReLU",
            "Pooling",
            "Dense / Softmax",
        ],
    },
    "deep_cnn": {
        "summary": "深层 CNN · 多块卷积 + GAP 分类",
        "layers": [
            "Input",
            "卷积块  Conv + BN + ReLU + Pool  （重复数块）",
            "GlobalAveragePool",
            "Dropout",
            "Dense / Softmax",
        ],
    },
    "resnet18": {
        "summary": "ResNet-18 分类 · 残差块 + GAP",
        "layers": [
            "Input",
            "Stem  Conv 7×7 / 3×3 + Pool",
            "Residual  stage2–5",
            "GlobalAveragePool",
            "Dense / Softmax",
        ],
    },
}


def _known_architecture(folder: Path, cfg: dict, extra: dict, post: str, arch_title: str) -> tuple[str, list[str]]:
    text = " ".join(
        [
            folder.name,
            str(cfg.get("name") or ""),
            str(extra.get("arch") or ""),
            str(arch_title or ""),
            str(post or ""),
        ]
    ).lower()
    if "retina" in text:
        key = "retinaface"
    elif "squeeze" in text:
        key = "squeezenet"
    elif "ssdlite" in text or "ssd" in text or "mobilenet" in text:
        key = "mobilenet"
    elif "resnet" in text:
        key = "resnet18"
    elif "deep_cnn" in text or "deep-cnn" in text or "深层" in text:
        key = "deep_cnn"
    elif "simple_cnn" in text or "simple-cnn" in text or "简单 cnn" in text:
        key = "simple_cnn"
    else:
        return "", []
    spec = _KNOWN_ARCHITECTURES[key]
    return spec["summary"], list(spec["layers"])


_LAYER_INDEX_RE = re.compile(r"^\s*\d+\.\s*")


def _html_escape(text) -> str:
    return (
        str(text)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


def layer_cells_html(lines, *, summary: str = "", title: str = "") -> str:
    """代码编辑器样式：左侧行号，右侧层内容。开始页 / 已训练模型共用。"""
    parts = []
    if title:
        parts.append(
            f"<p style='margin:0 0 8px 0;font-size:14px;'><b>{_html_escape(title)}</b></p>"
        )
    if summary:
        parts.append(
            f"<p style='margin:0 0 10px 0;color:#374151;line-height:1.65;font-size:13px;'>"
            f"{_html_escape(summary)}</p>"
        )
    rows = []
    index = 0
    for line in lines or []:
        text = _LAYER_INDEX_RE.sub("", str(line)).strip()
        if not text:
            continue
        index += 1
        code_bg = "#FAFBFC" if index % 2 else "#FFFFFF"
        rows.append(
            "<tr>"
            f"<td style='width:40px;padding:3px 10px 3px 6px;text-align:right;"
            f"vertical-align:top;background:#EEF1F5;color:#8B939C;"
            f"border-right:1px solid #D8DEE6;font-family:Consolas,\"Cascadia Mono\",monospace;"
            f"font-size:12px;line-height:1.75;'>{index}</td>"
            f"<td style='padding:3px 12px;vertical-align:top;background:{code_bg};"
            f"color:#1F2937;font-family:Consolas,\"Cascadia Mono\",monospace;"
            f"font-size:13px;line-height:1.75;white-space:pre-wrap;'>"
            f"{_html_escape(text)}</td>"
            "</tr>"
        )
    if index == 0 and not summary:
        parts.append("<p style='color:#9CA3AF;margin:0;'>暂无层结构说明。</p>")
        return "".join(parts)
    if rows:
        parts.append(
            "<table style='border-collapse:collapse;width:100%;"
            "border:1px solid #D8DEE6;background:#FFFFFF;' cellspacing='0' cellpadding='0'>"
            + "".join(rows)
            + "</table>"
        )
    return "".join(parts)


_MAX_GRAPH_OUTLINE = 14


def _compact_graph_lines(layers: list[dict], max_lines: int = _MAX_GRAPH_OUTLINE) -> list[str]:
    """把展开后的 ncnn 算子收成短列表，避免已训练页铺上百行。"""
    useful = [layer for layer in layers if str(layer.get("type") or "") not in ("Split", "")]
    groups: list[dict] = []
    for layer in useful:
        kind = str(layer.get("type") or "Op")
        name = str(layer.get("name") or "")
        if groups and groups[-1]["type"] == kind:
            groups[-1]["n"] += 1
            groups[-1]["last"] = name
        else:
            groups.append({"type": kind, "n": 1, "first": name, "last": name})
    lines = []
    for group in groups:
        if group["n"] == 1:
            extra = f"  {group['first']}" if group["first"] else ""
            lines.append(f"{group['type']}{extra}")
        else:
            span = group["first"]
            if group["last"] and group["last"] != group["first"]:
                span = f"{group['first']} … {group['last']}"
            lines.append(f"{group['type']}  ×{group['n']}  ({span})")
    if len(lines) <= max_lines:
        return lines
    kept = lines[: max_lines - 1]
    kept.append(f"… 其余 {len(lines) - (max_lines - 1)} 组算子已折叠")
    return kept


def _summarize_graph(param_path: Path) -> tuple[str, list[str], int]:
    from ncnn_infer import parse_param

    layers = parse_param(param_path)
    counts = Counter(layer["type"] for layer in layers)
    summary = "ncnn 导出图  " + " · ".join(f"{name}×{n}" for name, n in counts.most_common(6))
    return summary, _compact_graph_lines(layers), len(layers)


def public_board_status(text: str) -> str:
    """给界面看的板上状态。编译命令和乱码路径不进入这里。"""
    original = str(text or "")
    raw = " ".join(original.replace("\ufffd", "").split())
    if not raw:
        return ""
    noisy = (
        "\ufffd" in original
        or "command failed" in raw.lower()
        or "arm-none-eabi" in raw
        or "arm-xilinx-eabi" in raw
        or ":\\" in raw
        or len(raw) > 80
    )
    if not noisy:
        return raw
    if "已按当前固件封包" in raw:
        if "图像积木" in raw:
            return "model.mod 已按当前固件封包，并写入了图像积木"
        return "model.mod 已按当前固件封包"
    if "缺少 model.cfg" in raw:
        return "缺少 model.cfg，不能生成 model.mod"
    if "未找到" in raw:
        return "未找到交叉编译器，model.mod 尚未编译"
    if raw.startswith("可以推送"):
        return "可以推送到板子"
    return "model.mod 尚未编译成功"


def _load_sidecar_info(folder: Path) -> dict:
    extra: dict = {}
    for name in ("train_info.json", "deployment_config.json", "model_info.json"):
        path = folder / name
        if not path.exists():
            continue
        try:
            data = read_json(path)
        except Exception:
            continue
        if isinstance(data, dict):
            extra.update({k: v for k, v in data.items() if v is not None})
    return extra


def format_param_count(n: int) -> str:
    if n <= 0:
        return "未知"
    if n >= 1_000_000:
        return f"{n / 1_000_000:.2f} M（{n:,}）"
    if n >= 1_000:
        return f"{n / 1_000:.1f} K（{n:,}）"
    return f"{n:,}"


def format_bytes(n: int) -> str:
    if n <= 0:
        return "0 B"
    if n >= 1024 * 1024:
        return f" {n / (1024 * 1024):.2f} MB".strip()
    if n >= 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n} B"


def inspect_weight_pack(folder: Path | str) -> dict:
    """读取一个 ncnn 权重文件夹的架构、大小、参数量、类别等信息。"""
    folder = Path(folder)
    cfg = parse_model_cfg(folder / CFG_NAME) if (folder / CFG_NAME).exists() else {}
    extra = _load_sidecar_info(folder)
    labels = load_labels(folder / LABELS_NAME)
    if not labels and extra.get("class_names"):
        labels = [str(x) for x in extra["class_names"]]
    labels = format_class_names(labels)

    weight = folder / WEIGHT_NAME
    if not weight.exists():
        bins = [p for p in folder.glob("*.bin") if p.name != PARAM_NAME]
        weight = bins[0] if bins else weight
    weight_bytes = weight.stat().st_size if weight.exists() else 0
    pack_bytes = 0
    for child in folder.iterdir():
        if child.is_file():
            pack_bytes += child.stat().st_size

    quantize = _guess_quantize(folder, extra)
    params = extra.get("params") or extra.get("num_params") or extra.get("parameter_count")
    params_source = "训练记录"
    try:
        params = int(params) if params is not None else 0
    except (TypeError, ValueError):
        params = 0
    if params <= 0:
        params = _estimate_params(weight_bytes, quantize)
        params_source = "按权重大小估算"

    arch = _guess_arch_name(folder, cfg, extra)
    graph_summary = ""
    layer_lines: list[str] = []
    layer_count = 0
    text_param = folder / "graph.param"
    if not text_param.exists():
        candidates = sorted(p for p in folder.glob("*.param") if p.name != PARAM_NAME)
        text_param = candidates[0] if candidates else text_param
    if text_param.exists():
        try:
            graph_summary, layer_lines, layer_count = _summarize_graph(text_param)
        except Exception:
            graph_summary, layer_lines, layer_count = "", [], 0

    post = pack_post(folder) or str(cfg.get("post") or extra.get("post") or "classify")
    known_summary, known_layers = _known_architecture(folder, cfg, extra, post, arch)
    raw_layer_count = layer_count
    if known_layers:
        graph_summary = known_summary or graph_summary
        if raw_layer_count > len(known_layers):
            if "retina" in str(arch or "").lower() or post == "retinaface":
                graph_summary += f"（导出计算图把 BN/ReLU 展开为 {raw_layer_count} 层，概念结构仍是骨干+FPN+SSH+九个头）"
            else:
                graph_summary += f"（导出计算图把 BN/ReLU 展开为 {raw_layer_count} 层，下面只列出概念结构）"
        layer_lines = known_layers
        layer_count = len(known_layers)
    kind = "检测" if post in ("detect", "retinaface") else "分类"
    size = int(cfg.get("image_size") or cfg.get("input_size") or extra.get("image_size") or 0)
    dataset = _infer_dataset(folder, cfg, extra)
    channels = int(cfg.get("channels") or extra.get("channels") or (1 if size and size <= 32 and dataset == "mnist" else 3))
    name = _pretty_pack_name({
        "arch_title": arch,
        "dataset": dataset,
        "quantize": quantize,
        "display_name": _read_display_name(folder, cfg, extra),
        "name": str(cfg.get("name") or extra.get("name") or folder.name),
        "folder": folder.name,
    })
    num_classes = len(labels) if labels else int(extra.get("num_classes") or 0)

    return {
        "path": str(folder.resolve()),
        "name": name,
        "display_name": _read_display_name(folder, cfg, extra),
        "folder": folder.name,
        "source": _source_label(folder),
        "arch": arch,
        "arch_detail": graph_summary,
        "layers": layer_lines,
        "layer_count": layer_count,
        "graph_layer_count": raw_layer_count,
        "post": post,
        "kind": kind,
        "input_size": size,
        "channels": channels,
        "pixel": str(cfg.get("pixel") or extra.get("pixel") or "bgr"),
        "quantize": quantize,
        "weight_bytes": weight_bytes,
        "pack_bytes": pack_bytes,
        "params": params,
        "params_source": params_source,
        "num_classes": num_classes,
        "class_names": labels,
        "dataset": dataset,
        "test_accuracy": extra.get("test_accuracy") or extra.get("float_test_accuracy"),
        "mtime": folder.stat().st_mtime if folder.exists() else 0,
        "board_folder": str(extra.get("board_folder") or ""),
        "board_status": public_board_status(str(extra.get("board_status") or "")),
        "board_ready": bool(extra.get("board_ready")),
        "sd_models_dir": str(extra.get("sd_models_dir") or ""),
    }


def _sanitize_display_name(name: str) -> str:
    text = " ".join(str(name or "").replace("\n", " ").replace("\r", " ").split()).strip()
    return text.replace("=", " ").strip()[:80]


def set_pack_display_name(folder: Path | str, name: str) -> str:
    """修改已训练模型的显示名，写入 display_name.txt / sidecar / model.cfg。"""
    folder = Path(folder)
    if not folder.is_dir():
        raise FileNotFoundError(f"模型目录不存在: {folder}")
    text = _sanitize_display_name(name)
    if not text:
        raise ValueError("名称不能为空")
    (folder / "display_name.txt").write_text(text + "\n", encoding="utf-8")
    for json_name in ("train_info.json", "deployment_config.json", "model_info.json"):
        path = folder / json_name
        if not path.exists():
            continue
        try:
            data = read_json(path)
        except Exception:
            continue
        if isinstance(data, dict):
            data["display_name"] = text
            write_json(path, data)
    cfg_path = folder / CFG_NAME
    if cfg_path.exists():
        try:
            lines = cfg_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except Exception:
            lines = []
        out = []
        saw_name = False
        saw_display = False
        for line in lines:
            stripped = line.strip()
            if stripped.startswith("#") or "=" not in stripped:
                out.append(line)
                continue
            key = stripped.split("=", 1)[0].strip()
            if key == "name":
                out.append(line)
                saw_name = True
            elif key == "display_name":
                out.append(f"display_name = {text}")
                saw_display = True
            else:
                out.append(line)
        if not saw_name:
            out.append(f"name = {folder.name[:31]}")
        if not saw_display:
            out.append(f"display_name = {text}")
        cfg_path.write_text("\n".join(out).rstrip() + "\n", encoding="utf-8")
    return text


def _pack_parent_roots() -> list[Path]:
    roots = []
    for path in [*reference_model_roots(), FPGA_DEPLOYMENT, MODELS_OUTPUT, BOARD_SD_MODELS]:
        if path.exists() and path.is_dir():
            roots.append(path.resolve())
    return roots


def _export_name_key(folder: Path, extra: dict | None = None) -> str:
    extra = extra or _load_sidecar_info(folder)
    name = str(extra.get("folder_name") or "").strip()
    if name:
        return name
    return _STAMP_RE.sub("", Path(folder).name).strip("_") or Path(folder).name


def related_weight_packs(folder: Path | str) -> list[Path]:
    """同一套权重的全部副本（本仓库 / 导出包 / 训练目录），不含未选中的竞赛板参考。"""
    folder = Path(folder).resolve()
    extra = _load_sidecar_info(folder) if folder.exists() else {}
    cfg = parse_model_cfg(folder / CFG_NAME) if (folder / CFG_NAME).exists() else {}
    post = pack_post(folder) if folder.exists() else str(extra.get("post") or "classify")
    quantize = _guess_quantize(folder, extra) if folder.exists() else ""
    ident = _pack_identity(folder, cfg, extra, post, quantize) if folder.exists() else None
    key = _export_name_key(folder, extra)

    found: dict[str, Path] = {}
    if folder.exists():
        found[str(folder)] = folder

    def _add(path: Path):
        path = Path(path)
        if not path.exists() or not path.is_dir():
            return
        resolved = path.resolve()
        if resolved.name.lower() == "presets":
            return
        if resolved in _pack_parent_roots():
            return
        if _is_external_competition_pack(resolved) and resolved != folder:
            return
        if not has_ncnn_weights(resolved):
            return
        found[str(resolved)] = resolved

    sd = extra.get("sd_models_dir")
    if sd:
        _add(Path(sd))
    if BOARD_SD_MODELS.exists():
        _add(BOARD_SD_MODELS / folder.name)
        if key:
            _add(BOARD_SD_MODELS / key)
    if key and BOARD_MODELS.exists():
        _add(BOARD_MODELS / key)
    if FPGA_DEPLOYMENT.exists():
        for child in FPGA_DEPLOYMENT.iterdir():
            if child.name == key or (key and child.name.startswith(key + "_")):
                _add(child)
    if MODELS_OUTPUT.exists():
        for child in MODELS_OUTPUT.iterdir():
            if not child.is_dir():
                continue
            if child.name == key or (key and child.name.startswith(key + "_")):
                _add(child)

    if ident is not None:
        for item in _collect_weight_items(include_train_runs=True):
            if item.get("identity") != ident:
                continue
            _add(Path(item["path"]))

    return sorted(found.values(), key=lambda p: str(p))


def purge_export_duplicates(folder_name: str, keep: Path | str) -> list[Path]:
    """导出时清掉 fpga_deployment 里同名的时间戳副本。"""
    keep = Path(keep).resolve()
    name = (folder_name or "").strip()
    removed: list[Path] = []
    if not name or not FPGA_DEPLOYMENT.exists():
        return removed
    for child in list(FPGA_DEPLOYMENT.iterdir()):
        if not child.is_dir():
            continue
        resolved = child.resolve()
        if resolved == keep:
            continue
        if child.name == name or child.name.startswith(name + "_"):
            shutil.rmtree(resolved, ignore_errors=True)
            removed.append(resolved)
    return removed


def _assert_deletable_pack(folder: Path) -> Path:
    folder = Path(folder).resolve()
    if not folder.exists() or not folder.is_dir():
        raise FileNotFoundError(f"模型目录不存在: {folder}")
    if folder.name.lower() == "presets":
        raise ValueError("不能删除预设目录")
    roots = _pack_parent_roots()
    if folder in roots:
        raise ValueError("不能删除权重根目录")
    nested = False
    for root in roots:
        try:
            folder.relative_to(root)
            nested = True
            break
        except ValueError:
            continue
    if not nested:
        raise ValueError("只能删除本工具检索到的权重目录")
    if not has_ncnn_weights(folder) and not _is_train_run_dir(folder):
        raise ValueError("不是有效的 ncnn 权重目录")
    return folder


def delete_weight_pack(folder: Path | str) -> list[Path]:
    """删除一套权重的全部本地副本，避免删完又冒出导出包。"""
    folder = Path(folder).resolve()
    targets = related_weight_packs(folder)
    if not targets:
        targets = [_assert_deletable_pack(folder)]
    deleted: list[Path] = []
    errors: list[str] = []
    for path in targets:
        try:
            _assert_deletable_pack(path)
            shutil.rmtree(path)
            deleted.append(path)
        except Exception as exc:
            errors.append(f"{path}: {exc}")
    if not deleted:
        raise ValueError(errors[0] if errors else "没有可删除的模型")
    return deleted


def pack_path_from_option(text: str) -> str | None:
    t = (text or "").strip()
    if not t:
        return None
    items = catalog_weight_packs()
    for item in items:
        if t in (item["option"], item["label"], item["path"], item["name"], item["folder"]):
            return item["path"]
    rest = t
    for prefix in ("使用权重：", "使用权重:"):
        if rest.startswith(prefix):
            rest = rest[len(prefix):].strip()
            break
    if rest != t:
        for item in items:
            if rest in (item["label"], item["name"], item["folder"], item["path"]):
                return item["path"]
    return None


def write_four_files(
    dest: Path,
    *,
    param_bin: Path,
    weight_bin: Path,
    cfg_text_path: Path | None = None,
    labels_path: Path | None = None,
    cfg_kwargs: dict | None = None,
    class_names: list[str] | None = None,
) -> Path:
    dest = ensure_dir(dest)
    shutil.copy2(param_bin, dest / PARAM_NAME)
    shutil.copy2(weight_bin, dest / WEIGHT_NAME)
    if cfg_text_path and Path(cfg_text_path).exists():
        shutil.copy2(cfg_text_path, dest / CFG_NAME)
    elif cfg_kwargs:
        write_model_cfg(dest / CFG_NAME, **cfg_kwargs)
    if labels_path and Path(labels_path).exists():
        shutil.copy2(labels_path, dest / LABELS_NAME)
    elif class_names is not None:
        write_labels(dest / LABELS_NAME, class_names)
    return dest


def board_folder_for_pack(folder: Path) -> str:
    """从完整包推断板上短目录名。"""
    folder = Path(folder)
    extra = _load_sidecar_info(folder) if folder.exists() else {}
    saved = str(extra.get("board_folder") or "").strip()
    if saved and len(saved) <= BOARD_NAME_MAX and saved.isascii() and saved.replace("_", "").isalnum():
        return saved
    dataset = str(extra.get("dataset") or "").strip().lower()
    arch = str(extra.get("arch") or "").strip().lower()
    quant = str(extra.get("quantize") or folder.name)
    if not dataset:
        dataset = _infer_dataset(folder, {}, extra)
    if not arch:
        text = f"{folder.name} {extra.get('arch') or ''}".lower()
        if "retina" in text:
            arch = "retinaface"
        elif "ssd" in text:
            arch = "ssdlite"
        elif "squeeze" in text:
            arch = "squeezenet"
        elif "resnet" in text:
            arch = "resnet18"
        elif "deep_cnn" in text or "deep-cnn" in text:
            arch = "deep_cnn"
        elif "mobilenet" in text:
            arch = "mobilenet"
        elif "cnn" in text:
            arch = "simple_cnn"
        else:
            arch = "net"
    return board_folder_name(dataset, arch, quant)


def write_sd_four_files(src: Path, folder_name: str | None = None, model_mod: Path | None = None, include_mod: bool = False) -> Path:
    """拷出板上四件到 models_sd/<短名>/。不拷 model.cfg / image_ops.cfg。"""
    src = Path(src)
    name = (folder_name or "").strip() or board_folder_for_pack(src)
    if len(name) > BOARD_NAME_MAX or not name.isascii():
        name = board_folder_for_pack(src)
    missing = [item for item in BOARD_DATA_FILES if not (src / item).is_file()]
    if missing:
        raise FileNotFoundError(f"{src} 缺少板上文件: {', '.join(missing)}")
    dest = ensure_dir(BOARD_SD_MODELS / name)
    keep = set(BOARD_DATA_FILES + (MOD_NAME,))
    for child in list(dest.iterdir()):
        if child.name in keep:
            continue
        if child.is_dir():
            shutil.rmtree(child, ignore_errors=True)
        else:
            child.unlink(missing_ok=True)
    shutil.copy2(src / PARAM_NAME, dest / PARAM_NAME)
    shutil.copy2(src / WEIGHT_NAME, dest / WEIGHT_NAME)
    write_board_labels(src / LABELS_NAME, dest / LABELS_NAME)
    mod_dest = dest / MOD_NAME
    mod_src = Path(model_mod) if model_mod else None
    if include_mod and mod_src is not None and mod_src.is_file():
        shutil.copy2(mod_src, mod_dest)
    elif mod_dest.exists():
        mod_dest.unlink()
    for stale in (CFG_NAME, IMAGE_OPS_NAME):
        leftover = dest / stale
        if leftover.exists():
            leftover.unlink()
    return dest


def retire_long_sd_dir(host: Path, short_name: str) -> None:
    """删掉按旧长目录名放过的 models_sd 副本，避免再被整夹拷到板上。"""
    host = Path(host)
    extra = _load_sidecar_info(host) if host.exists() else {}
    names = {host.name, str(extra.get("folder_name") or "").strip()}
    for name in names:
        if not name or name == short_name:
            continue
        old = BOARD_SD_MODELS / name
        if old.is_dir():
            shutil.rmtree(old, ignore_errors=True)


def sync_sd_four_packs(root: Path | None = None) -> list[Path]:
    """把 models/ 下已有完整包同步成板上短名目录。"""
    from board_mod import prepare_board_pack

    root = Path(root or BOARD_MODELS)
    written: list[Path] = []
    if not root.is_dir():
        return written
    for child in root.iterdir():
        if not child.is_dir() or child.name in ("presets",) or child.name.startswith("_"):
            continue
        if not all((child / item).is_file() for item in BOARD_DATA_FILES):
            continue
        info = prepare_board_pack(child)
        dest = Path(info["sd_dir"])
        written.append(dest)
    return written
