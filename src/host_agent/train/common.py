"""训练/导出共用路径与工具。"""
from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path

from repo_paths import project_root

PROJECT_ROOT = project_root()
DATASETS_DIR = PROJECT_ROOT / "datasets"
MODELS_OUTPUT = PROJECT_ROOT / "models_output"
FPGA_DEPLOYMENT = PROJECT_ROOT / "fpga_deployment"
BOARD_MODELS = PROJECT_ROOT / "models"
BOARD_SD_MODELS = PROJECT_ROOT / "models_sd"
MODEL_REGISTRY = MODELS_OUTPUT / "model_info.json"
EXTERNAL_COCO_DIR = PROJECT_ROOT.parent / "tflm_pl_accel" / "data" / "coco"

DATASET_ALIASES = {
    "mnist": "mnist",
    "cifar10": "cifar10",
    "cifar-10": "cifar10",
    "cifar100": "cifar100",
    "cifar-100": "cifar100",
    "fashion_mnist": "fashion_mnist",
    "fashion-mnist": "fashion_mnist",
    "fashionmnist": "fashion_mnist",
    "kmnist": "kmnist",
    "kuzushiji": "kmnist",
    "kuzushiji_mnist": "kmnist",
    "flowers": "flowers",
    "flower_photos": "flowers",
    "tf_flowers": "flowers",
    "rps": "rps",
    "rock_paper_scissors": "rps",
    "rock-paper-scissors": "rps",
    "hands": "hands",
    "hand": "hands",
    "gesture": "hands",
    "gestures": "hands",
    "hand_gesture": "hands",
    "hand_gestures": "hands",
    "手势": "hands",
    "手势识别": "hands",
    "手势数字": "hands",
    "数字手势": "hands",
    "coco": "coco",
    "coco2017": "coco",
    "mscoco": "coco",
    "wider_face": "wider_face",
    "widerface": "wider_face",
    "wider": "wider_face",
    "face": "wider_face",
    "faces": "wider_face",
    "人脸": "wider_face",
    "人脸检测": "wider_face",
    "custom": "custom",
}

ARCH_ALIASES = {
    "squeezenet": "squeezenet",
    "squeeze-net": "squeezenet",
    "squeeze": "squeezenet",
    "mobilenet": "mobilenet",
    "mobilenetv2": "mobilenet",
    "mobilenet_v2": "mobilenet",
    "ssdlite": "ssdlite",
    "ssd": "ssdlite",
    "mobilenet_ssdlite": "ssdlite",
    "mobilenetv2_ssdlite": "ssdlite",
    "retinaface": "retinaface",
    "retinaface_mnet": "retinaface",
    "retina": "retinaface",
    "mnet25": "retinaface",
    "simple_cnn": "simple_cnn",
    "simple-cnn": "simple_cnn",
    "cnn": "simple_cnn",
    "deep_cnn": "deep_cnn",
    "deep-cnn": "deep_cnn",
    "resnet18": "resnet18",
    "resnet-18": "resnet18",
    "resnet": "resnet18",
}


def now_stamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def sanitize_token(text: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in (text or "")).strip("_")


def collapse_duplicate_tokens(text: str) -> str:
    parts = [p for p in (text or "").replace("-", "_").split("_") if p]
    collapsed: list[str] = []
    for part in parts:
        if collapsed and part.lower() == collapsed[-1].lower():
            continue
        collapsed.append(part)
    return "_".join(collapsed)


def model_stem(dataset: str, arch: str) -> str:
    """训练目录 / 导出文件夹共用的网络身份，避免 mnist_mnist_deep_cnn 这类重复前缀。"""
    ds = sanitize_token(dataset).lower()
    ar = sanitize_token(arch).lower()
    if ar.startswith(ds + "_") or ar == ds:
        stem = ar or ds
    elif ds and ar:
        stem = f"{ds}_{ar}"
    else:
        stem = ar or ds or "model"
    return collapse_duplicate_tokens(stem) or "model"


def format_class_name(index: int, name) -> str:
    """编号与标签分开：纯数字类别写成「数字 N」，已有名称保持原样。"""
    text = str(name).strip() if name is not None else ""
    if not text:
        return f"类别 {index}"
    if text == str(index) or (text.isdigit() and int(text) == index):
        return f"数字 {index}"
    return text


def format_class_names(names) -> list[str]:
    seq = list(names or [])
    return [format_class_name(i, n) for i, n in enumerate(seq)]


def ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def read_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def normalize_dataset_name(name: str) -> str:
    key = name.strip().lower()
    if key not in DATASET_ALIASES:
        raise ValueError(f"未知数据集: {name}，可选: {', '.join(sorted(set(DATASET_ALIASES.values())))}")
    return DATASET_ALIASES[key]


def normalize_arch_name(name: str) -> str:
    key = name.strip().lower()
    if key not in ARCH_ALIASES:
        raise ValueError(f"未知模型架构: {name}，可选: {', '.join(sorted(set(ARCH_ALIASES.values())))}")
    return ARCH_ALIASES[key]


def find_latest_run(dataset: str | None = None) -> tuple[Path, dict]:
    """在 models_output 中查找最近一次训练结果。"""
    if not MODELS_OUTPUT.exists():
        raise FileNotFoundError(f"未找到模型输出目录: {MODELS_OUTPUT}，请先运行 train.py")

    candidates: list[tuple[float, Path, dict]] = []
    for run_dir in MODELS_OUTPUT.iterdir():
        if not run_dir.is_dir():
            continue
        info_path = run_dir / "train_info.json"
        if not info_path.exists():
            continue
        has_weight = (run_dir / "model.pt").exists() or (run_dir / "model.pth").exists()
        if not has_weight:
            continue
        info = read_json(info_path)
        if dataset:
            ds = str(info.get("dataset", "")).lower()
            name = run_dir.name.lower()
            if dataset.lower() not in ds and not name.startswith(dataset.lower()):
                continue
        candidates.append((run_dir.stat().st_mtime, run_dir, info))

    if not candidates:
        hint = f"（数据集={dataset}）" if dataset else ""
        raise FileNotFoundError(f"未找到可用的训练结果{hint}，请先运行 train.py")

    candidates.sort(key=lambda item: item[0], reverse=True)
    _, run_dir, info = candidates[0]
    return run_dir, info


def update_model_registry(entry: dict) -> None:
    """更新 models_output/model_info.json 索引。"""
    registry = {"models": []}
    if MODEL_REGISTRY.exists():
        try:
            registry = read_json(MODEL_REGISTRY)
        except Exception:
            registry = {"models": []}
    models = registry.get("models", [])
    models = [m for m in models if m.get("run_dir") != entry.get("run_dir")]
    models.insert(0, entry)
    write_json(MODEL_REGISTRY, {"models": models[:50]})
