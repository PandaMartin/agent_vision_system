"""数据集加载与预处理。

数据文件统一放在仓库根目录 datasets/，见 dataset_store.py。
内置：MNIST / Fashion-MNIST / KMNIST / CIFAR-10 / CIFAR-100 / Flowers / RPS / 手势识别 / COCO / WIDER FACE。
自定义：按类别子文件夹扫描图像（与上位机「开始」页一致）。
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from dataset_store import (
    DATASETS_DIR,
    find_class_folder,
    load_cifar10_python,
    load_cifar100_python,
    load_fashion_idx,
    load_kmnist_npz,
    load_mnist_npz,
    prepare_builtin,
    prepare_coco,
)

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff"}

DATASET_SPECS = [
    {"key": "mnist", "name": "MNIST", "size": 28, "channels": 1, "classes": 10, "hint": "手写数字 · 28×28 灰度 · 10类"},
    {"key": "fashion_mnist", "name": "Fashion-MNIST", "size": 28, "channels": 1, "classes": 10, "hint": "服装分类 · 28×28 灰度 · 10类"},
    {"key": "kmnist", "name": "KMNIST", "size": 28, "channels": 1, "classes": 10, "hint": "日文变体假名 · 28×28 灰度 · 10类"},
    {"key": "cifar10", "name": "CIFAR-10", "size": 32, "channels": 3, "classes": 10, "hint": "自然物体 · 32×32 彩色 · 10类"},
    {"key": "cifar100", "name": "CIFAR-100", "size": 32, "channels": 3, "classes": 100, "hint": "细粒度物体 · 32×32 彩色 · 100类"},
    {"key": "flowers", "name": "Flowers", "size": 224, "channels": 3, "classes": 5, "hint": "五种花卉 · 224×224 彩色 · 5类"},
    {"key": "rps", "name": "石头剪刀布", "size": 224, "channels": 3, "classes": 3, "hint": "手势分类 · 224×224 彩色 · 3类"},
    {"key": "hands", "name": "手势识别", "size": 100, "channels": 3, "classes": 10, "hint": "数字手势 0–9 · 100×100 彩色 · 10类"},
    {"key": "coco", "name": "COCO", "size": 224, "channels": 3, "classes": 80, "hint": "COCO 2017 · 80类物体 · 224×224"},
    {"key": "wider_face", "name": "WIDER FACE 人脸检测", "size": 320, "channels": 3, "classes": 1, "hint": "WIDER FACE · 多人脸检测框 · RetinaFace 320 或 SSD 300", "task": "detect"},
    {"key": "custom", "name": "自定义数据集", "size": 224, "channels": 3, "classes": 0, "hint": "按类别子文件夹组织图像"},
]

_SPEC_BY_KEY = {item["key"]: item for item in DATASET_SPECS}

CIFAR100_FINE = [
    "apple", "aquarium_fish", "baby", "bear", "beaver", "bed", "bee", "beetle",
    "bicycle", "bottle", "bowl", "boy", "bridge", "bus", "butterfly", "camel",
    "can", "castle", "caterpillar", "cattle", "chair", "chimpanzee", "clock",
    "cloud", "cockroach", "couch", "crab", "crocodile", "cup", "dinosaur",
    "dolphin", "elephant", "flatfish", "forest", "fox", "girl", "hamster",
    "house", "kangaroo", "keyboard", "lamp", "lawn_mower", "leopard", "lion",
    "lizard", "lobster", "man", "maple_tree", "motorcycle", "mountain", "mouse",
    "mushroom", "oak_tree", "orange", "orchid", "otter", "palm_tree", "pear",
    "pickup_truck", "pine_tree", "plain", "plate", "poppy", "porcupine",
    "possum", "rabbit", "raccoon", "ray", "road", "rocket", "rose", "sea",
    "seal", "shark", "shrew", "skunk", "skyscraper", "snail", "snake", "spider",
    "squirrel", "streetcar", "sunflower", "sweet_pepper", "table", "tank",
    "telephone", "television", "tiger", "tractor", "train", "trout",
    "tulip", "turtle", "wardrobe", "whale", "willow_tree", "wolf", "woman", "worm",
]

KMNIST_NAMES = ["お", "き", "す", "つ", "な", "は", "ま", "や", "れ", "を"]
HANDS_NAMES = [f"手势 {i}" for i in range(10)]
FASHION_NAMES = [
    "T-shirt", "Trouser", "Pullover", "Dress", "Coat",
    "Sandal", "Shirt", "Sneaker", "Bag", "Ankle boot",
]
CIFAR10_NAMES = [
    "airplane", "automobile", "bird", "cat", "deer",
    "dog", "frog", "horse", "ship", "truck",
]
COCO_CLASS_NAMES = [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier",
    "toothbrush",
]
SKIP_CLASS_DIRS = {"__macosx", "license", ".git", ".svn"}

if len(CIFAR100_FINE) != 100:
    raise RuntimeError(f"CIFAR-100 细类名数量应为 100，当前 {len(CIFAR100_FINE)}")
if len(COCO_CLASS_NAMES) != 80:
    raise RuntimeError(f"COCO 类名数量应为 80，当前 {len(COCO_CLASS_NAMES)}")


def dataset_help_text() -> str:
    return " / ".join(item["key"] for item in DATASET_SPECS)


def dataset_enum_keys() -> list[str]:
    return [item["key"] for item in DATASET_SPECS]


def dataset_prompt_names() -> str:
    return " / ".join(item["name"] for item in DATASET_SPECS)


def resolve_dataset_key(text: str) -> str:
    raw = (text or "").strip()
    if not raw:
        return "custom"
    lowered = raw.lower()
    if lowered in _SPEC_BY_KEY:
        return lowered
    from common import DATASET_ALIASES

    if lowered in DATASET_ALIASES:
        return DATASET_ALIASES[lowered]
    if raw in DATASET_ALIASES:
        return DATASET_ALIASES[raw]
    for spec in DATASET_SPECS:
        name = spec["name"]
        if raw == name or raw.startswith(name + " ") or raw.startswith(name + " -"):
            return spec["key"]
    if lowered.startswith("mnist"):
        return "mnist"
    if "cifar-100" in lowered or "cifar100" in lowered:
        return "cifar100"
    if "cifar" in lowered:
        return "cifar10"
    if "fashion" in lowered:
        return "fashion_mnist"
    if "kmnist" in lowered or "假名" in raw:
        return "kmnist"
    if "flower" in lowered or "花卉" in raw:
        return "flowers"
    if "rps" in lowered or "剪刀" in raw:
        return "rps"
    if "hands" in lowered or "手势" in raw or "gesture" in lowered:
        return "hands"
    if "coco" in lowered:
        return "coco"
    if "wider" in lowered or "人脸" in raw or "face" in lowered:
        return "wider_face"
    return "custom"


def resolve_dataset_spec(text: str) -> dict:
    return _SPEC_BY_KEY.get(resolve_dataset_key(text)) or _SPEC_BY_KEY["custom"]


def _register_spec_aliases() -> None:
    from common import DATASET_ALIASES

    for item in DATASET_SPECS:
        key = item["key"]
        DATASET_ALIASES.setdefault(key, key)
        name = str(item.get("name") or "").strip()
        if name:
            DATASET_ALIASES.setdefault(name.lower(), key)


_register_spec_aliases()


def default_image_size(dataset: str) -> int:
    spec = _SPEC_BY_KEY.get((dataset or "").lower())
    return int(spec["size"]) if spec else 224


def default_channels(dataset: str) -> int:
    spec = _SPEC_BY_KEY.get((dataset or "").lower())
    return int(spec["channels"]) if spec else 3


def load_dataset(
    dataset: str,
    image_size: int | None = None,
    normalize: bool = True,
    custom_path: str | None = None,
    keep_classes=None,
    min_classes: int = 2,
) -> dict:
    """加载并预处理数据集。

    返回:
        x_train, y_train, x_test, y_test: numpy 数组
        class_names, num_classes, channels, image_size
    """
    dataset = dataset.lower()
    image_size = image_size or default_image_size(dataset)

    if dataset == "custom":
        if not custom_path:
            raise ValueError("自定义数据集需要提供 --custom-path")
        data = _load_custom_dataset(custom_path, image_size, normalize)
    elif dataset == "mnist":
        folder = prepare_builtin("mnist")
        x_train, y_train, x_test, y_test = load_mnist_npz(folder)
        class_names = [f"数字 {i}" for i in range(10)]
        data = _finalize_builtin(x_train, y_train, x_test, y_test, image_size, 1, class_names, normalize)
    elif dataset == "fashion_mnist":
        folder = prepare_builtin("fashion_mnist")
        x_train, y_train, x_test, y_test = load_fashion_idx(folder)
        data = _finalize_builtin(x_train, y_train, x_test, y_test, image_size, 1, list(FASHION_NAMES), normalize)
    elif dataset == "kmnist":
        folder = prepare_builtin("kmnist")
        x_train, y_train, x_test, y_test = load_kmnist_npz(folder)
        data = _finalize_builtin(x_train, y_train, x_test, y_test, image_size, 1, list(KMNIST_NAMES), normalize)
    elif dataset == "cifar10":
        folder = prepare_builtin("cifar10")
        x_train, y_train, x_test, y_test = load_cifar10_python(folder)
        data = _finalize_builtin(x_train, y_train, x_test, y_test, image_size, 3, list(CIFAR10_NAMES), normalize)
    elif dataset == "cifar100":
        folder = prepare_builtin("cifar100")
        x_train, y_train, x_test, y_test = load_cifar100_python(folder)
        data = _finalize_builtin(x_train, y_train, x_test, y_test, image_size, 3, list(CIFAR100_FINE), normalize)
    elif dataset == "flowers":
        folder = prepare_builtin("flowers")
        root = find_class_folder(folder, "flower_photos")
        data = _load_custom_dataset(str(root), image_size, normalize)
        data["dataset"] = "flowers"
    elif dataset == "rps":
        folder = prepare_builtin("rps")
        root = find_class_folder(folder, "rps")
        data = _load_custom_dataset(str(root), image_size, normalize)
        data["dataset"] = "rps"
    elif dataset == "hands":
        root = find_class_folder(DATASETS_DIR / "hands", "hands")
        data = _load_custom_dataset(str(root), image_size, normalize)
        data["dataset"] = "hands"
        data["class_names"] = _pretty_hands_names(data.get("class_names") or [])
        data["num_classes"] = len(data["class_names"])
    elif dataset == "coco":
        root = prepare_coco(download_missing=True)
        data = _load_coco_classify(root, image_size, normalize)
    elif dataset == "wider_face":
        raise ValueError("WIDER FACE 是人脸检测数据集，请选择 MobileNetV2 SSD（检测）训练，才能在图中框出所有人脸。")
    else:
        raise ValueError(f"不支持的数据集: {dataset}")

    data = apply_class_filter(data, keep_classes, min_classes=min_classes)
    print(f"训练集: {data['x_train'].shape}, 标签: {data['y_train'].shape}")
    print(f"测试集: {data['x_test'].shape}, 标签: {data['y_test'].shape}")
    print(f"类别数: {data['num_classes']}, 类别: {data['class_names']}")
    return data


def parse_keep_classes(value) -> list[str] | None:
    if value is None:
        return None
    if isinstance(value, str):
        items = [part.strip() for part in value.split(",") if part.strip()]
    elif isinstance(value, (list, tuple, set)):
        items = [str(part).strip() for part in value if str(part).strip()]
    else:
        items = [str(value).strip()]
    return items or None


def resolve_class_indices(class_names: list[str], keep_classes) -> list[int] | None:
    items = parse_keep_classes(keep_classes)
    if not items:
        return None
    names = [str(n) for n in class_names]
    lower = {n.lower(): i for i, n in enumerate(names)}
    indices: list[int] = []
    seen: set[int] = set()
    missing: list[str] = []
    for item in items:
        idx = None
        if item in names:
            idx = names.index(item)
        elif item.lower() in lower:
            idx = lower[item.lower()]
        elif item.isdigit():
            num = int(item)
            if 0 <= num < len(names):
                idx = num
        if idx is None:
            missing.append(item)
            continue
        if idx not in seen:
            seen.add(idx)
            indices.append(idx)
    if missing:
        raise ValueError("找不到这些类别: " + ", ".join(missing))
    if not indices:
        raise ValueError("没有匹配的训练类别")
    return indices


def apply_class_filter(data: dict, keep_classes, min_classes: int = 2) -> dict:
    """按名称或索引保留部分类别，并将标签重新编号为 0..K-1。"""
    names = [str(n) for n in (data.get("class_names") or [])]
    indices = resolve_class_indices(names, keep_classes)
    if indices is None:
        if len(names) < min_classes:
            raise ValueError(_class_count_error(min_classes))
        return data
    if indices == list(range(len(names))):
        if len(names) < min_classes:
            raise ValueError(_class_count_error(min_classes))
        return data
    if len(indices) < min_classes:
        raise ValueError(_class_count_error(min_classes))
    remap = {old: new for new, old in enumerate(indices)}
    new_names = [names[i] for i in indices]

    def _subset(x, y):
        y = np.asarray(y).reshape(-1)
        mask = np.isin(y, indices)
        if not np.any(mask):
            raise ValueError("筛选类别后没有可用样本")
        y2 = np.array([remap[int(v)] for v in y[mask]], dtype=np.int32)
        return x[mask], y2

    x_train, y_train = _subset(data["x_train"], data["y_train"])
    x_test, y_test = _subset(data["x_test"], data["y_test"])
    out = dict(data)
    out.update({
        "x_train": x_train,
        "y_train": y_train,
        "x_test": x_test,
        "y_test": y_test,
        "class_names": new_names,
        "num_classes": len(new_names),
        "kept_classes": new_names,
    })
    return out


def _class_count_error(min_classes: int) -> str:
    if min_classes <= 1:
        return "请至少选择 1 个类别"
    return "分类任务至少选择 2 个类别；检测网络可以只选 1 类"


def _finalize_builtin(
    x_train, y_train, x_test, y_test,
    image_size: int, channels: int, class_names: list[str], normalize: bool,
) -> dict:
    x_train = _ensure_nhwc(np.asarray(x_train), channels)
    x_test = _ensure_nhwc(np.asarray(x_test), channels)
    x_train = _resize(x_train, image_size)
    x_test = _resize(x_test, image_size)

    x_train = x_train.astype(np.float32)
    x_test = x_test.astype(np.float32)
    if normalize:
        if x_train.max() > 1.5:
            x_train /= 255.0
            x_test /= 255.0
    else:
        # 保持 0-255，但仍用 float32，便于后续量化校准
        if x_train.max() <= 1.5:
            x_train *= 255.0
            x_test *= 255.0

    y_train = np.asarray(y_train).reshape(-1).astype(np.int32)
    y_test = np.asarray(y_test).reshape(-1).astype(np.int32)
    return {
        "x_train": x_train,
        "y_train": y_train,
        "x_test": x_test,
        "y_test": y_test,
        "class_names": class_names,
        "num_classes": len(class_names),
        "channels": int(x_train.shape[-1]),
        "image_size": image_size,
        "normalize": normalize,
    }


def _ensure_nhwc(x: np.ndarray, channels: int) -> np.ndarray:
    if x.ndim == 3:
        x = np.expand_dims(x, axis=-1)
    if channels == 3 and x.shape[-1] == 1:
        x = np.repeat(x, 3, axis=-1)
    return x


def _resize(x: np.ndarray, image_size: int) -> np.ndarray:
    if x.shape[1] == image_size and x.shape[2] == image_size:
        return x
    try:
        import cv2
    except ImportError:
        cv2 = None
    out = np.empty((len(x), image_size, image_size, x.shape[-1]), dtype=x.dtype)
    for i, im in enumerate(x):
        if cv2 is not None:
            resized = cv2.resize(im, (image_size, image_size), interpolation=cv2.INTER_LINEAR)
        else:
            from PIL import Image

            arr = im if im.dtype == np.uint8 else np.clip(im, 0, 255).astype(np.uint8) if im.max() > 1.5 else np.clip(im * 255.0, 0, 255).astype(np.uint8)
            mode = "L" if arr.ndim == 2 or arr.shape[-1] == 1 else "RGB"
            if arr.ndim == 3 and arr.shape[-1] == 1:
                arr = arr[..., 0]
            img = Image.fromarray(arr, mode=mode)
            img = img.resize((image_size, image_size), Image.BILINEAR)
            resized = np.asarray(img)
        if resized.ndim == 2:
            resized = resized[..., None]
        out[i] = resized.astype(x.dtype, copy=False)
    return out


def _pretty_hands_names(names: list[str]) -> list[str]:
    out = []
    for name in names:
        text = str(name).strip()
        if text.isdigit():
            out.append(f"手势 {int(text)}")
        else:
            out.append(text or name)
    return out or list(HANDS_NAMES)


def _usable_class_dir(folder: Path) -> bool:
    if not folder.is_dir():
        return False
    name = folder.name.lower()
    if name.startswith(".") or name in SKIP_CLASS_DIRS:
        return False
    try:
        return any(p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS for p in folder.iterdir())
    except OSError:
        return False


def _load_image_list(paths: list[Path], labels: list[int], image_size: int, normalize: bool) -> tuple[np.ndarray, np.ndarray]:
    from PIL import Image

    images: list[np.ndarray] = []
    kept: list[int] = []
    for path, label in zip(paths, labels):
        try:
            with Image.open(path) as img:
                img = img.convert("RGB")
                img = img.resize((image_size, image_size))
                arr = np.asarray(img, dtype=np.float32)
        except OSError as exc:
            print(f"跳过损坏图像 {path}: {exc}")
            continue
        images.append(arr)
        kept.append(label)
    if not images:
        raise ValueError("没有可用图像")
    x = np.stack(images, axis=0)
    y = np.asarray(kept, dtype=np.int32)
    if normalize:
        x = x / 255.0
    return x, y


def _coco_labeled_paths(root: Path, split: str, max_per_class: int) -> tuple[list[str], list[Path], list[int]]:
    ann_path = root / "annotations" / f"instances_{split}.json"
    if not ann_path.is_file():
        raise FileNotFoundError(f"缺少 COCO 标注: {ann_path}")
    print(f"读取 COCO 标注 {ann_path.name} ...")
    with open(ann_path, "r", encoding="utf-8") as fh:
        ann = json.load(fh)
    cats = sorted(ann["categories"], key=lambda c: c["id"])
    names = [c["name"] for c in cats]
    cat_id_to_idx = {c["id"]: i for i, c in enumerate(cats)}
    images = {im["id"]: im["file_name"] for im in ann["images"]}
    best: dict[int, tuple[float, int]] = {}
    for item in ann["annotations"]:
        if item.get("iscrowd"):
            continue
        idx = cat_id_to_idx.get(item["category_id"])
        if idx is None:
            continue
        area = float(item.get("area") or 0)
        prev = best.get(item["image_id"])
        if prev is None or area > prev[0]:
            best[item["image_id"]] = (area, idx)
    buckets: list[list[Path]] = [[] for _ in names]
    img_root = root / split
    for image_id, (_, idx) in best.items():
        fname = images.get(image_id)
        if not fname:
            continue
        path = img_root / fname
        if path.is_file():
            buckets[idx].append(path)
    rng = np.random.default_rng(42)
    paths: list[Path] = []
    labels: list[int] = []
    for idx, items in enumerate(buckets):
        rng.shuffle(items)
        take = items[:max_per_class] if max_per_class else items
        for path in take:
            paths.append(path)
            labels.append(idx)
    return names, paths, labels


def _load_coco_classify(
    root: Path,
    image_size: int,
    normalize: bool,
    max_train_per_class: int = 40,
    max_test_per_class: int = 10,
) -> dict:
    """用每张图面积最大的物体作为类别；默认每类抽样，避免 11 万张图撑爆内存。"""
    names, train_paths, train_labels = _coco_labeled_paths(root, "train2017", max_train_per_class)
    _, test_paths, test_labels = _coco_labeled_paths(root, "val2017", max_test_per_class)
    print(
        f"COCO 分类子集: 训练 {len(train_paths)} 张 / 验证 {len(test_paths)} 张"
        f"（每类最多 {max_train_per_class}/{max_test_per_class}，完整图像在 {root}）"
    )
    x_train, y_train = _load_image_list(train_paths, train_labels, image_size, normalize)
    x_test, y_test = _load_image_list(test_paths, test_labels, image_size, normalize)
    return {
        "x_train": x_train,
        "y_train": y_train,
        "x_test": x_test,
        "y_test": y_test,
        "class_names": names,
        "num_classes": len(names),
        "channels": 3,
        "image_size": image_size,
        "normalize": normalize,
        "custom_path": str(root.resolve()),
        "dataset": "coco",
    }


def _load_custom_dataset(folder_path: str, image_size: int, normalize: bool) -> dict:
    from PIL import Image

    root = Path(folder_path)
    if not root.is_dir():
        raise FileNotFoundError(f"自定义数据集路径不存在: {root}")

    class_dirs = sorted([d for d in root.iterdir() if _usable_class_dir(d)])
    if not class_dirs:
        raise ValueError(f"文件夹中没有含图像的类别子目录: {root}")

    class_names = [d.name for d in class_dirs]
    images: list[np.ndarray] = []
    labels: list[int] = []

    for label, class_dir in enumerate(class_dirs):
        files = [p for p in class_dir.iterdir() if p.suffix.lower() in IMAGE_EXTENSIONS]
        if not files:
            print(f"警告: 类别 {class_dir.name} 没有图像文件")
            continue
        for img_path in files:
            with Image.open(img_path) as img:
                img = img.convert("RGB")
                img = img.resize((image_size, image_size))
                arr = np.asarray(img, dtype=np.float32)
            images.append(arr)
            labels.append(label)

    if not images:
        raise ValueError("自定义数据集中没有可用图像")

    x = np.stack(images, axis=0)
    y = np.asarray(labels, dtype=np.int32)
    if normalize:
        x = x / 255.0

    # 简单分层划分：每类约 10% 作为测试集
    rng = np.random.default_rng(42)
    train_idx, test_idx = [], []
    for label in range(len(class_names)):
        idxs = np.where(y == label)[0]
        rng.shuffle(idxs)
        split = max(1, int(len(idxs) * 0.1)) if len(idxs) >= 10 else max(1, len(idxs) // 5)
        test_idx.extend(idxs[:split].tolist())
        train_idx.extend(idxs[split:].tolist())

    if not train_idx:
        raise ValueError("自定义数据集样本过少，无法划分训练集")

    return {
        "x_train": x[train_idx],
        "y_train": y[train_idx],
        "x_test": x[test_idx],
        "y_test": y[test_idx],
        "class_names": class_names,
        "num_classes": len(class_names),
        "channels": 3,
        "image_size": image_size,
        "normalize": normalize,
        "custom_path": str(root.resolve()),
    }

