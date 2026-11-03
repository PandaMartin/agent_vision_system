"""开始页数据集预览：只取少量样本，不触发完整下载、不加载整集。"""
from __future__ import annotations

from pathlib import Path

import numpy as np

from dataset_store import (
    WIDER_CLASS_NAMES,
    dataset_dir,
    ensure_coco_root,
    find_class_folder,
    iter_wider_records,
    load_fashion_idx,
    load_kmnist_npz,
    load_mnist_npz,
    wider_ann_file,
    _unpickle,
)
from datasets import (
    CIFAR10_NAMES,
    CIFAR100_FINE,
    COCO_CLASS_NAMES,
    DATASET_SPECS,
    FASHION_NAMES,
    HANDS_NAMES,
    KMNIST_NAMES,
)

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp"}
THUMB_SIZE = 88
_SPEC = {item["key"]: item for item in DATASET_SPECS}

def list_class_names(key: str, custom_path: str | None = None) -> list[str]:
    if key == "mnist":
        return [f"数字 {i}" for i in range(10)]
    if key == "fashion_mnist":
        return list(FASHION_NAMES)
    if key == "kmnist":
        return list(KMNIST_NAMES)
    if key == "cifar10":
        return list(CIFAR10_NAMES)
    if key == "cifar100":
        return list(CIFAR100_FINE)
    if key == "coco":
        return list(COCO_CLASS_NAMES)
    if key == "wider_face":
        return list(WIDER_CLASS_NAMES)
    if key == "hands":
        return list(HANDS_NAMES)
    if key in {"custom", "flowers", "rps", "hands"} and custom_path:
        try:
            return [d.name for d in _leaf_class_dirs(Path(custom_path))]
        except Exception:
            return []
    return []


def preview_samples(key: str, count: int | None = None, custom_path: str | None = None) -> dict:
    spec = _SPEC.get(key) or {}
    result = {
        "key": key,
        "name": spec.get("name") or key,
        "hint": spec.get("hint") or "",
        "size": spec.get("size") or 0,
        "classes": spec.get("classes") or 0,
        "samples": [],
        "class_names": list_class_names(key, custom_path),
        "class_counts": [],
        "total_images": 0,
        "note": "",
    }
    try:
        if key == "custom":
            result.update(_from_class_folders(custom_path, count))
        elif key == "mnist":
            names = [f"数字 {i}" for i in range(10)]
            x_train, y_train, x_test, y_test = load_mnist_npz(dataset_dir("mnist"))
            counts = _counts_from_y(_concat_labels(y_train, y_test), len(names))
            result["samples"] = _from_arrays(x_train, y_train, names, count, counts)
            result["class_counts"] = counts
        elif key == "fashion_mnist":
            x_train, y_train, x_test, y_test = load_fashion_idx(dataset_dir("fashion_mnist"))
            counts = _counts_from_y(_concat_labels(y_train, y_test), len(FASHION_NAMES))
            result["samples"] = _from_arrays(x_train, y_train, FASHION_NAMES, count, counts)
            result["class_counts"] = counts
        elif key == "kmnist":
            x_train, y_train, x_test, y_test = load_kmnist_npz(dataset_dir("kmnist"))
            counts = _counts_from_y(_concat_labels(y_train, y_test), len(KMNIST_NAMES))
            result["samples"] = _from_arrays(x_train, y_train, list(KMNIST_NAMES), count, counts)
            result["class_counts"] = counts
        elif key == "cifar10":
            counts = _cifar10_label_counts()
            result["samples"] = _from_cifar10_batch(count, counts)
            result["class_counts"] = counts
        elif key == "cifar100":
            result.update(_from_class_folders(str(find_class_folder(dataset_dir("cifar100"), "train")), count))
        elif key == "flowers":
            result.update(_from_class_folders(str(find_class_folder(dataset_dir("flowers"), "flower_photos")), count))
        elif key == "rps":
            result.update(_from_class_folders(str(find_class_folder(dataset_dir("rps"), "rps")), count))
        elif key == "hands":
            result.update(_from_class_folders(str(find_class_folder(dataset_dir("hands"), "hands")), count, names=list(HANDS_NAMES)))
        elif key == "coco":
            result.update(_from_coco(count))
        elif key == "wider_face":
            result.update(_from_wider_face(count))
        else:
            result["note"] = "暂不支持预览"
    except FileNotFoundError:
        if key == "hands":
            result["note"] = "请把类别子文件夹放到 datasets/hands/（数字手势 0–9）。"
        else:
            result["note"] = "本地还没有这个数据集，开始训练时会自动下载。"
    except Exception as exc:
        result["note"] = f"预览失败：{exc}"
    names = result.get("class_names") or []
    if not names:
        names = [str(s.get("label") or "") for s in result.get("samples") or [] if s.get("label")]
    names, counts = _unique_class_entries(names, result.get("class_counts") or [])
    result["class_names"] = names
    result["class_counts"] = counts
    if names:
        result["classes"] = len(names)
    result["samples"] = _one_sample_per_class(result.get("samples") or [], names)
    _stamp_sample_counts(result["samples"], result["class_names"], result["class_counts"])
    if not result.get("total_images"):
        result["total_images"] = int(sum(result["class_counts"]))
    if not result["samples"] and not result["note"]:
        result["note"] = "没有找到可预览的图像"
    return result


def _unique_class_entries(names: list[str], counts) -> tuple[list[str], list[int]]:
    seen: set[str] = set()
    out_names: list[str] = []
    out_counts: list[int] = []
    counts = list(counts or [])
    for i, raw in enumerate(names or []):
        name = str(raw or "").strip()
        if not name or name in seen:
            continue
        seen.add(name)
        out_names.append(name)
        out_counts.append(int(counts[i]) if i < len(counts) else 0)
    if len(out_counts) < len(out_names):
        out_counts.extend([0] * (len(out_names) - len(out_counts)))
    return out_names, out_counts[: len(out_names)]


def _one_sample_per_class(samples: list[dict], names: list[str]) -> list[dict]:
    if not samples:
        return []
    wanted = [str(n) for n in names] if names else []
    picked: dict[str, dict] = {}
    ordered: list[dict] = []
    for sample in samples:
        label = str(sample.get("label") or "").strip()
        if not label or label in picked:
            continue
        if wanted and label not in wanted:
            continue
        picked[label] = sample
        ordered.append(sample)
    if wanted:
        return [picked[name] for name in wanted if name in picked] or ordered[:1]
    return ordered


def _concat_labels(*parts) -> np.ndarray:
    arrays = [np.asarray(p).reshape(-1) for p in parts if p is not None]
    if not arrays:
        return np.zeros((0,), dtype=np.int32)
    return np.concatenate(arrays, axis=0)


def _counts_from_y(y, n_classes: int) -> list[int]:
    if n_classes <= 0:
        return []
    y = np.asarray(y).reshape(-1)
    if y.size == 0:
        return [0] * n_classes
    counts = np.bincount(y.astype(np.int64, copy=False), minlength=n_classes)
    return [int(v) for v in counts[:n_classes]]


def _stamp_sample_counts(samples: list[dict], names: list[str], counts: list[int]) -> None:
    index = {str(name): i for i, name in enumerate(names)}
    for sample in samples:
        if sample.get("count") is not None:
            continue
        i = index.get(str(sample.get("label") or ""))
        if i is not None and i < len(counts):
            sample["count"] = int(counts[i])


def _count_dir_images(folder: Path) -> int:
    n = 0
    try:
        for p in folder.iterdir():
            if p.is_file() and p.suffix.lower() in IMAGE_EXTS:
                n += 1
    except OSError:
        return 0
    return n


def _to_uint8(img: np.ndarray) -> np.ndarray:
    arr = np.asarray(img)
    if arr.ndim == 2:
        pass
    elif arr.ndim == 3 and arr.shape[-1] == 1:
        arr = arr[..., 0]
    if arr.dtype != np.uint8:
        arr = arr.astype(np.float32)
        if arr.max() <= 1.5:
            arr = arr * 255.0
        arr = np.clip(arr, 0, 255).astype(np.uint8)
    return np.ascontiguousarray(arr)


def _read_thumb(path: Path, size: int = THUMB_SIZE) -> np.ndarray:
    from PIL import Image

    with Image.open(path) as img:
        rgb = img.convert("RGB")
        rgb.thumbnail((size, size))
        return np.ascontiguousarray(rgb, dtype=np.uint8)


def _from_arrays(x, y, names: list[str], count: int | None = None, class_counts: list[int] | None = None) -> list[dict]:
    samples = []
    seen = set()
    y = np.asarray(y).reshape(-1)
    target = count if count is not None else len(names)
    for i in range(len(y)):
        lab = int(y[i])
        if lab in seen:
            continue
        seen.add(lab)
        name = names[lab] if 0 <= lab < len(names) else str(lab)
        item = {"image": _to_uint8(x[i]), "label": name}
        if class_counts is not None and 0 <= lab < len(class_counts):
            item["count"] = int(class_counts[lab])
        samples.append(item)
        if len(samples) >= target:
            break
    return samples


def _cifar10_root() -> Path:
    root = dataset_dir("cifar10") / "cifar-10-batches-py"
    if (root / "data_batch_1").is_file():
        return root
    matches = list(dataset_dir("cifar10").rglob("data_batch_1"))
    if not matches:
        raise FileNotFoundError(root / "data_batch_1")
    return matches[0].parent


def _cifar10_label_counts() -> list[int]:
    root = _cifar10_root()
    labels = []
    for i in range(1, 6):
        batch = _unpickle(root / f"data_batch_{i}")
        labels.extend(batch[b"labels"])
    test = root / "test_batch"
    if test.is_file():
        labels.extend(_unpickle(test)[b"labels"])
    return _counts_from_y(labels, len(CIFAR10_NAMES))


def _from_cifar10_batch(count: int | None = None, class_counts: list[int] | None = None) -> list[dict]:
    root = _cifar10_root()
    batch = _unpickle(root / "data_batch_1")
    x = batch[b"data"].reshape(-1, 3, 32, 32).transpose(0, 2, 3, 1)
    y = np.asarray(batch[b"labels"], dtype=np.int32)
    return _from_arrays(x, y, CIFAR10_NAMES, count, class_counts)


def _iter_class_dirs(root: Path) -> list[Path]:
    if not root or not Path(root).is_dir():
        raise FileNotFoundError(root)
    dirs = sorted(
        d for d in Path(root).iterdir()
        if d.is_dir() and not d.name.startswith(".") and d.name.lower() not in {"__macosx", "license"}
    )
    if not dirs:
        raise FileNotFoundError(root)
    return dirs


def _first_image(folder: Path) -> Path | None:
    for p in folder.iterdir():
        if p.is_file() and p.suffix.lower() in IMAGE_EXTS:
            return p
    return None


def _leaf_class_dirs(root: Path) -> list[Path]:
    leaves = []
    for class_dir in _iter_class_dirs(root):
        if _first_image(class_dir) is not None:
            leaves.append(class_dir)
            continue
        nested = sorted(p for p in class_dir.iterdir() if p.is_dir() and not p.name.startswith("."))
        for child in nested:
            if _first_image(child) is not None:
                leaves.append(child)
    if not leaves:
        raise FileNotFoundError(root)
    return leaves


def _from_class_folders(folder_path: str | None, count: int | None = None, names: list[str] | None = None) -> dict:
    if not folder_path:
        return {"samples": [], "note": "请先选择按类别子文件夹组织的图像目录。", "classes": 0}
    root = Path(folder_path)
    dirs = _leaf_class_dirs(root)
    class_names = [names[i] if names and i < len(names) else d.name for i, d in enumerate(dirs)]
    class_counts = [_count_dir_images(d) for d in dirs]
    limit = len(dirs) if count is None else min(len(dirs), count)
    samples = []
    for idx, class_dir in enumerate(dirs[:limit]):
        img_path = _first_image(class_dir)
        if img_path is None:
            continue
        label = class_names[idx] if idx < len(class_names) else class_dir.name
        samples.append({"image": _read_thumb(img_path), "label": label, "count": class_counts[idx]})
    return {
        "samples": samples,
        "classes": len(dirs),
        "class_names": class_names,
        "class_counts": class_counts,
        "total_images": int(sum(class_counts)),
        "note": "",
    }


def _from_wider_face(count: int | None = None) -> dict:
    from PIL import Image, ImageDraw

    root = dataset_dir("wider_face")
    target = 1 if count is None else max(1, count)
    samples = []
    split = "val"
    try:
        records = iter_wider_records(root, split, max_samples=target)
    except FileNotFoundError:
        records = iter_wider_records(root, "train", max_samples=target)
        split = "train"
    for path, boxes in records:
        try:
            with Image.open(path) as im:
                rgb = im.convert("RGB")
                draw = ImageDraw.Draw(rgb)
                width = max(2, rgb.width // 180)
                for x, y, w, h in boxes[:12]:
                    draw.rectangle([x, y, x + w, y + h], outline=(34, 197, 94), width=width)
                rgb.thumbnail((THUMB_SIZE, THUMB_SIZE))
                arr = np.ascontiguousarray(rgb, dtype=np.uint8)
        except OSError:
            continue
        n_box = len(boxes)
        samples.append({"image": arr, "label": "face", "boxes": n_box})
        if len(samples) >= target:
            break
    if not samples:
        raise FileNotFoundError(root)
    n_images = _wider_image_count(root)
    return {
        "samples": samples,
        "classes": 1,
        "class_names": list(WIDER_CLASS_NAMES),
        "class_counts": [n_images],
        "total_images": n_images,
        "note": "绿框为人脸标注，训练请选 MobileNetV2 SSD。",
    }


def _wider_image_count(root: Path) -> int:
    total = 0
    for split in ("train", "val"):
        try:
            total += _wider_split_image_count(root, split)
        except FileNotFoundError:
            continue
    return total


def _wider_split_image_count(root: Path, split: str) -> int:
    lines = wider_ann_file(root, split).read_text(encoding="utf-8", errors="ignore").splitlines()
    n = 0
    i = 0
    while i < len(lines):
        rel = lines[i].strip()
        i += 1
        if not rel:
            continue
        if i >= len(lines):
            break
        try:
            n_box = int(lines[i].strip())
        except ValueError:
            continue
        i += 1
        i += max(n_box, 0)
        if n_box == 0 and i < len(lines):
            peek = lines[i].strip()
            dummy = peek.split()
            if dummy and dummy[0].lstrip("-").isdigit() and not peek.lower().endswith((".jpg", ".jpeg", ".png")):
                i += 1
        n += 1
    return n


def _from_coco(count: int | None = None) -> dict:
    import json
    import os

    root = ensure_coco_root()
    split = root / "val2017"
    split_name = "val2017"
    if not split.is_dir():
        split = root / "train2017"
        split_name = "train2017"
    if not split.is_dir():
        raise FileNotFoundError(split)
    ann_path = root / "annotations" / f"instances_{split_name}.json"
    samples = []
    class_count = 80
    if ann_path.is_file():
        with open(ann_path, "r", encoding="utf-8") as fh:
            ann = json.load(fh)
        cats = sorted(ann.get("categories") or [], key=lambda c: c.get("id", 0))
        class_count = len(cats) or class_count
        names = {c["id"]: c.get("name") or str(c["id"]) for c in cats}
        images = {im["id"]: im.get("file_name") for im in ann.get("images") or []}
        picked: dict[int, Path] = {}
        per_class: dict[int, set] = {c["id"]: set() for c in cats}
        target = count if count is not None else len(cats)
        for item in ann.get("annotations") or []:
            if item.get("iscrowd"):
                continue
            cid = item.get("category_id")
            iid = item.get("image_id")
            if cid not in names:
                continue
            if iid is not None:
                per_class.setdefault(cid, set()).add(iid)
            if cid in picked:
                continue
            fname = images.get(iid)
            if not fname:
                continue
            path = split / fname
            if path.is_file():
                picked[cid] = path
            if len(picked) >= target:
                continue
        class_names = [c.get("name") or str(c["id"]) for c in cats]
        class_counts = [len(per_class.get(c["id"], ())) for c in cats]
        for cat in cats:
            path = picked.get(cat["id"])
            if path is None:
                continue
            try:
                samples.append({
                    "image": _read_thumb(path),
                    "label": names[cat["id"]],
                    "count": len(per_class.get(cat["id"], ())),
                })
            except OSError:
                continue
        return {
            "samples": samples,
            "classes": class_count,
            "class_names": class_names,
            "class_counts": class_counts,
            "total_images": int(sum(class_counts)),
            "note": "每类一张（按主物体），数字是该类出现过的图片数。",
        }

    with os.scandir(split) as it:
        for ent in it:
            if not ent.is_file():
                continue
            name = ent.name.lower()
            if not (name.endswith(".jpg") or name.endswith(".jpeg") or name.endswith(".png")):
                continue
            path = Path(ent.path)
            try:
                samples.append({"image": _read_thumb(path), "label": path.stem[-6:]})
            except OSError:
                continue
            if count is not None and len(samples) >= count:
                break
            if count is None and len(samples) >= 8:
                break
    return {
        "samples": samples,
        "classes": class_count,
        "class_names": list(COCO_CLASS_NAMES),
        "note": "预览原图，训练时按主物体类别抽样。",
    }
