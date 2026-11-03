"""检测训练样本：COCO 实例框、VOC XML，或类别文件夹（整图一框）。"""
from __future__ import annotations

import json
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image

from dataset_store import (
    WIDER_CLASS_NAMES,
    find_class_folder,
    iter_wider_records,
    prepare_builtin,
    prepare_coco,
    prepare_wider_face,
)
from datasets import (
    COCO_CLASS_NAMES,
    HANDS_NAMES,
    IMAGE_EXTENSIONS,
    _pretty_hands_names,
    _usable_class_dir,
    resolve_class_indices,
)
from ssd_priors import match_and_encode

MAX_BOXES = 64


def _sample_cap(max_samples) -> int | None:
    if max_samples is None:
        return None
    n = int(max_samples)
    return None if n <= 0 else n


def load_detection_pack(
    dataset: str,
    image_size: int,
    custom_path: str | None = None,
    keep_classes=None,
    max_samples: int | None = None,
) -> dict:
    dataset = (dataset or "").lower()
    max_samples = _sample_cap(max_samples)
    if dataset == "coco":
        names, train, val = _from_coco(keep_classes, max_samples)
        note = "COCO 实例框"
    elif dataset == "custom":
        if not custom_path:
            raise ValueError("检测自定义数据需要 --custom-path")
        names, train, val, note = _from_folder(custom_path, keep_classes, max_samples)
    elif dataset == "flowers":
        root = find_class_folder(prepare_builtin("flowers"), "flower_photos")
        names, train, val, note = _from_folder(str(root), keep_classes, max_samples)
        note = "花卉（整图框，建议改用带标注的检测集）"
    elif dataset == "rps":
        root = find_class_folder(prepare_builtin("rps"), "rps")
        names, train, val, note = _from_folder(str(root), keep_classes, max_samples)
        note = "手势（整图框）"
    elif dataset == "hands":
        from dataset_store import DATASETS_DIR

        root = find_class_folder(DATASETS_DIR / "hands", "hands")
        names, train, val, note = _from_folder(str(root), keep_classes, max_samples)
        names = _pretty_hands_names(names) if names else list(HANDS_NAMES)
        note = "数字手势（整图框，建议分类训练）"
    elif dataset == "wider_face":
        names, train, val = _from_wider_face(max_samples)
        note = "WIDER FACE 人脸框"
    else:
        names, train, val, note = _from_class_arrays(dataset, image_size, keep_classes, max_samples)
    if keep_classes:
        idx = resolve_class_indices(names, keep_classes)
        if idx is not None:
            mapping = {old: new for new, old in enumerate(idx)}
            names = [names[i] for i in idx]
            train = _remap_samples(train, mapping)
            val = _remap_samples(val, mapping)
    if len(names) < 1:
        raise ValueError("检测至少需要 1 个物体类别")
    if not train:
        raise ValueError("没有可用的检测训练样本，请检查数据集或所选类别")
    if not val:
        val = train[: max(1, len(train) // 10)] or train
    print(f"检测样本: 训练 {len(train)} / 验证 {len(val)}，类别 {names}（{note}）")
    return {
        "task": "detect",
        "class_names": names,
        "num_classes": len(names),
        "train_samples": train,
        "val_samples": val,
        "image_size": image_size,
        "channels": 3,
        "note": note,
    }


def encode_sample(sample: dict, priors: np.ndarray, image_size: int, augment: bool, rng: np.random.Generator, pipeline=None):
    image, boxes, labels = _read_image(sample, image_size)
    if augment and len(boxes):
        image = _photometric(image, rng)
        if rng.random() < 0.5:
            image, boxes = _random_expand(image, boxes, rng)
        # 不要每张小脸都裁近：验证集是原尺度，训练必须保留一部分全图。
        if rng.random() < 0.65:
            image, boxes, labels = _zoom_to_faces(image, boxes, labels, rng)
        if rng.random() < 0.5:
            image = image[:, ::-1, ...].copy()
            x1 = 1.0 - boxes[:, 2]
            x2 = 1.0 - boxes[:, 0]
            boxes = boxes.copy()
            boxes[:, 0] = x1
            boxes[:, 2] = x2
    if pipeline:
        from image_ops import apply_pipeline

        image = apply_pipeline(image, pipeline)
    loc_t, conf_t = match_and_encode(boxes, labels, priors)
    if image.ndim == 2:
        image = np.repeat(image[..., None], 3, axis=-1)
    if image.shape[-1] == 1:
        image = np.repeat(image, 3, axis=-1)
    image = image.astype(np.float32)
    if image.max() <= 1.5:
        image = image * 255.0
    # 与板上 mobilenetv2_ssdlite_voc 一致：BGR、(x-127.5)/127.5
    image = image[..., ::-1]
    image = (image - 127.5) * (1.0 / 127.5)
    return image, loc_t, conf_t


def _remap_samples(samples: list, mapping: dict[int, int]) -> list:
    out = []
    for item in samples:
        labels = np.asarray(item["labels"], np.int32)
        boxes = np.asarray(item["boxes"], np.float32)
        keep = np.array([int(v) in mapping for v in labels], dtype=bool)
        if not np.any(keep):
            continue
        new_item = dict(item)
        new_item["boxes"] = boxes[keep]
        new_item["labels"] = np.array([mapping[int(v)] for v in labels[keep]], np.int32)
        out.append(new_item)
    return out


def _photometric(image: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    out = image.astype(np.float32, copy=True)
    if rng.random() < 0.8:
        out = np.clip(out + float(rng.uniform(-28.0, 28.0)), 0.0, 255.0)
    if rng.random() < 0.8:
        mean = float(out.mean())
        out = np.clip((out - mean) * float(rng.uniform(0.72, 1.28)) + mean, 0.0, 255.0)
    if out.ndim == 3 and rng.random() < 0.5:
        gray = out.mean(axis=2, keepdims=True)
        out = np.clip(gray + (out - gray) * float(rng.uniform(0.72, 1.28)), 0.0, 255.0)
    return out


def _random_expand(image: np.ndarray, boxes: np.ndarray, rng: np.random.Generator):
    """随机垫边再缩回，让训练也见到更小的人脸（接近验证集尺度）。"""
    boxes = np.asarray(boxes, np.float32).reshape(-1, 4)
    scale = float(rng.uniform(1.05, 1.75))
    h, w = int(image.shape[0]), int(image.shape[1])
    nh, nw = max(h + 1, int(round(h * scale))), max(w + 1, int(round(w * scale)))
    top = int(rng.integers(0, nh - h + 1))
    left = int(rng.integers(0, nw - w + 1))
    canvas = np.full((nh, nw, image.shape[2]), 127.0, dtype=np.float32)
    canvas[top:top + h, left:left + w] = image
    boxes = boxes.copy()
    boxes[:, [0, 2]] = (boxes[:, [0, 2]] * w + left) / float(nw)
    boxes[:, [1, 3]] = (boxes[:, [1, 3]] * h + top) / float(nh)
    arr = np.clip(canvas, 0, 255).astype(np.uint8)
    out = np.asarray(Image.fromarray(arr).resize((w, h), Image.BILINEAR), np.float32)
    return out, np.clip(boxes, 0.0, 1.0)


def _zoom_to_faces(image: np.ndarray, boxes: np.ndarray, labels: np.ndarray, rng: np.random.Generator):
    """把过小的人脸放大到锚框能匹配的尺度。"""
    boxes = np.asarray(boxes, np.float32).reshape(-1, 4)
    labels = np.asarray(labels, np.int32).reshape(-1)
    if boxes.shape[0] == 0:
        return image, boxes, labels
    heights = boxes[:, 3] - boxes[:, 1]
    if float(heights.max()) >= 0.22 and rng.random() > 0.25:
        return image, boxes, labels
    x1 = max(0.0, float(boxes[:, 0].min()) - 0.08)
    y1 = max(0.0, float(boxes[:, 1].min()) - 0.08)
    x2 = min(1.0, float(boxes[:, 2].max()) + 0.08)
    y2 = min(1.0, float(boxes[:, 3].max()) + 0.08)
    side = max(x2 - x1, y2 - y1, 0.42) * float(rng.uniform(0.88, 1.18))
    cx, cy = (x1 + x2) * 0.5, (y1 + y2) * 0.5
    cx = float(np.clip(cx + rng.uniform(-0.06, 0.06), 0.0, 1.0))
    cy = float(np.clip(cy + rng.uniform(-0.06, 0.06), 0.0, 1.0))
    x1 = float(np.clip(cx - side * 0.5, 0.0, 1.0))
    y1 = float(np.clip(cy - side * 0.5, 0.0, 1.0))
    x2 = min(1.0, x1 + side)
    y2 = min(1.0, y1 + side)
    h, w = image.shape[:2]
    px1, py1 = int(x1 * w), int(y1 * h)
    px2, py2 = max(px1 + 8, int(x2 * w)), max(py1 + 8, int(y2 * h))
    crop = image[py1:py2, px1:px2]
    if crop.size == 0:
        return image, boxes, labels
    arr = np.clip(crop, 0, 255).astype(np.uint8)
    if arr.ndim == 2:
        arr = np.repeat(arr[..., None], 3, axis=-1)
    out = np.asarray(Image.fromarray(arr).resize((w, h), Image.BILINEAR), np.float32)
    nw, nh = max(x2 - x1, 1e-6), max(y2 - y1, 1e-6)
    boxes = boxes.copy()
    boxes[:, 0] = (boxes[:, 0] - x1) / nw
    boxes[:, 2] = (boxes[:, 2] - x1) / nw
    boxes[:, 1] = (boxes[:, 1] - y1) / nh
    boxes[:, 3] = (boxes[:, 3] - y1) / nh
    boxes = np.clip(boxes, 0.0, 1.0)
    keep = (boxes[:, 2] - boxes[:, 0] > 0.02) & (boxes[:, 3] - boxes[:, 1] > 0.02)
    if not np.any(keep):
        return image, np.asarray(boxes, np.float32), labels
    return out, boxes[keep], labels[keep]


def _as_boxes(value) -> np.ndarray:
    if value is None:
        return np.zeros((0, 4), np.float32)
    boxes = np.asarray(value, np.float32).reshape(-1, 4)
    return boxes if boxes.size else np.zeros((0, 4), np.float32)


def _as_labels(value, n_boxes: int) -> np.ndarray:
    if value is None:
        return np.zeros((n_boxes,), np.int32)
    labels = np.asarray(value, np.int32).reshape(-1)
    if labels.size == n_boxes:
        return labels
    if labels.size == 0:
        return np.zeros((n_boxes,), np.int32)
    if labels.size < n_boxes:
        return np.pad(labels, (0, n_boxes - labels.size))
    return labels[:n_boxes]


def _read_image(sample: dict, image_size: int):
    boxes = _as_boxes(sample.get("boxes"))
    labels = _as_labels(sample.get("labels"), len(boxes))
    if sample.get("image") is not None:
        arr = np.asarray(sample["image"])
        if arr.dtype != np.uint8:
            arr = np.clip(arr, 0, 255).astype(np.uint8) if arr.max() > 1.5 else (arr * 255).astype(np.uint8)
        img = Image.fromarray(arr if arr.ndim == 2 or arr.shape[-1] != 1 else arr[..., 0])
        if img.mode != "RGB":
            img = img.convert("RGB")
        img = img.resize((image_size, image_size), Image.BILINEAR)
        return np.asarray(img, np.float32), boxes, labels
    path = Path(sample["path"])
    with Image.open(path) as im:
        rgb = im.convert("RGB")
        rgb = rgb.resize((image_size, image_size), Image.BILINEAR)
        arr = np.asarray(rgb, np.float32)
    return arr, boxes, labels


def _read_image_native(sample: dict):
    """读原图，不拉伸。RetinaFace 训练用 letterbox。"""
    boxes = _as_boxes(sample.get("boxes"))
    labels = _as_labels(sample.get("labels"), len(boxes))
    if sample.get("image") is not None:
        arr = np.asarray(sample["image"])
        if arr.dtype != np.uint8:
            arr = np.clip(arr, 0, 255).astype(np.uint8) if arr.max() > 1.5 else (arr * 255).astype(np.uint8)
        img = Image.fromarray(arr if arr.ndim == 2 or arr.shape[-1] != 1 else arr[..., 0])
        if img.mode != "RGB":
            img = img.convert("RGB")
        return np.asarray(img, np.float32), boxes, labels
    path = Path(sample["path"])
    with Image.open(path) as im:
        arr = np.asarray(im.convert("RGB"), np.float32)
    return arr, boxes, labels


def _split_samples(items: list, seed: int = 42, val_ratio: float = 0.1):
    rng = np.random.default_rng(seed)
    order = rng.permutation(len(items))
    n_val = max(1, int(len(items) * val_ratio)) if len(items) >= 10 else max(1, len(items) // 5)
    val_idx = set(order[:n_val].tolist())
    train, val = [], []
    for i, item in enumerate(items):
        (val if i in val_idx else train).append(item)
    if not train:
        train, val = items, items[: max(1, len(items) // 5)]
    return train, val


def _from_folder(folder_path: str, keep_classes, max_samples):
    root = Path(folder_path)
    voc_ann = root / "Annotations"
    voc_img = root / "JPEGImages"
    if voc_ann.is_dir() and voc_img.is_dir():
        names, items = _from_voc(root, max_samples)
        train, val = _split_samples(items)
        return names, train, val, "VOC XML 标注"
    class_dirs = sorted(d for d in root.iterdir() if _usable_class_dir(d))
    if not class_dirs:
        raise ValueError(f"未找到类别子目录或 VOC 标注: {root}")
    names = [d.name for d in class_dirs]
    items = []
    for label, class_dir in enumerate(class_dirs):
        files = [p for p in class_dir.iterdir() if p.suffix.lower() in IMAGE_EXTENSIONS]
        for path in files:
            items.append({
                "path": str(path),
                "boxes": np.array([[0.02, 0.02, 0.98, 0.98]], np.float32),
                "labels": np.array([label], np.int32),
            })
            if max_samples and len(items) >= max_samples:
                break
        if max_samples and len(items) >= max_samples:
            break
    train, val = _split_samples(items)
    return names, train, val, "类别文件夹（每张图一个整图框，精度有限）"


def _from_voc(root: Path, max_samples):
    names_set: list[str] = []
    items = []
    xmls = sorted((root / "Annotations").glob("*.xml"))
    for xml_path in xmls:
        tree = ET.parse(xml_path)
        xroot = tree.getroot()
        file_name = xroot.findtext("filename") or (xml_path.stem + ".jpg")
        img_path = root / "JPEGImages" / file_name
        if not img_path.is_file():
            matches = list((root / "JPEGImages").glob(xml_path.stem + ".*"))
            if not matches:
                continue
            img_path = matches[0]
        size = xroot.find("size")
        iw = float(size.findtext("width") or 1)
        ih = float(size.findtext("height") or 1)
        boxes, labels = [], []
        for obj in xroot.findall("object"):
            name = (obj.findtext("name") or "object").strip()
            if name not in names_set:
                names_set.append(name)
            bb = obj.find("bndbox")
            if bb is None:
                continue
            xmin = float(bb.findtext("xmin") or 0) / iw
            ymin = float(bb.findtext("ymin") or 0) / ih
            xmax = float(bb.findtext("xmax") or 0) / iw
            ymax = float(bb.findtext("ymax") or 0) / ih
            boxes.append([xmin, ymin, xmax, ymax])
            labels.append(names_set.index(name))
        if not boxes:
            continue
        items.append({
            "path": str(img_path),
            "boxes": np.asarray(boxes, np.float32),
            "labels": np.asarray(labels, np.int32),
        })
        if max_samples and len(items) >= max_samples:
            break
    return names_set, items


def _from_wider_face(max_samples):
    root = prepare_wider_face(download_missing=True)
    # 只丢掉退化框。8px 以下已在 iter_wider_records 滤掉；不再按画面 6% 丢小脸，也不再整图丢掉人群。
    min_face = 1e-4

    def load_split(split: str, cap: int | None, seed: int):
        scan = None if not cap else max(int(cap) * 6, 3000)
        pool = []
        for path, pixel_boxes in iter_wider_records(root, split, max_samples=scan):
            try:
                with Image.open(path) as im:
                    iw, ih = im.size
            except OSError:
                continue
            iw = max(float(iw), 1.0)
            ih = max(float(ih), 1.0)
            boxes = []
            for x, y, w, h in pixel_boxes:
                x1 = max(0.0, x / iw)
                y1 = max(0.0, y / ih)
                x2 = min(1.0, (x + w) / iw)
                y2 = min(1.0, (y + h) / ih)
                if min(x2 - x1, y2 - y1) < min_face:
                    continue
                boxes.append([x1, y1, x2, y2])
            if not boxes:
                continue
            boxes.sort(key=lambda b: (b[2] - b[0]) * (b[3] - b[1]), reverse=True)
            n = min(len(boxes), MAX_BOXES)
            pool.append({
                "path": str(path),
                "boxes": np.asarray(boxes[:n], np.float32),
                "labels": np.zeros((n,), np.int32),
            })
            if cap and len(pool) >= int(cap) * 2:
                break
        rng = np.random.default_rng(seed)
        order = rng.permutation(len(pool))
        picked = [pool[int(i)] for i in order]
        if cap:
            picked = picked[: int(cap)]
        print(f"WIDER FACE {split}: 可用 {len(pool)} 张，训练取 {len(picked)}（每图最多 {MAX_BOXES} 框，优先较大脸）")
        return picked

    train_cap = _sample_cap(max_samples)
    val_cap = None if train_cap is None else min(800, train_cap)
    train = load_split("train", train_cap, 42)
    val = load_split("val", val_cap, 7)
    if not train:
        train = val
    if not train:
        raise FileNotFoundError("WIDER FACE 图像或人脸框标注不完整")
    return list(WIDER_CLASS_NAMES), train, val or train[: max(1, len(train) // 10)]


def _from_coco(keep_classes, max_samples):
    root = prepare_coco(download_missing=True)

    def load_split(split: str, cap: int | None):
        ann_path = root / "annotations" / f"instances_{split}.json"
        img_dir = root / split
        if not ann_path.is_file() or not img_dir.is_dir():
            return [], []
        with open(ann_path, "r", encoding="utf-8") as fh:
            ann = json.load(fh)
        cats = sorted(ann.get("categories") or [], key=lambda c: c.get("id", 0))
        names = [c.get("name") or str(c["id"]) for c in cats]
        cat_id_to_idx = {c["id"]: i for i, c in enumerate(cats)}
        keep_idx = None
        if keep_classes:
            resolved = resolve_class_indices(names, keep_classes)
            keep_idx = set(resolved) if resolved is not None else None
        images = {im["id"]: im for im in ann.get("images") or []}
        buckets: dict[int, list] = {}
        for item in ann.get("annotations") or []:
            if item.get("iscrowd"):
                continue
            idx = cat_id_to_idx.get(item.get("category_id"))
            if idx is None:
                continue
            if keep_idx is not None and idx not in keep_idx:
                continue
            bb = item.get("bbox") or [0, 0, 0, 0]
            im = images.get(item.get("image_id"))
            if not im:
                continue
            iw, ih = float(im.get("width") or 1), float(im.get("height") or 1)
            x, y, w, h = [float(v) for v in bb]
            box = [x / iw, y / ih, (x + w) / iw, (y + h) / ih]
            buckets.setdefault(int(im["id"]), []).append((box, idx))
        samples = []
        for image_id, pairs in buckets.items():
            im = images[image_id]
            path = img_dir / im["file_name"]
            if not path.is_file():
                continue
            boxes = np.asarray([p[0] for p in pairs], np.float32)
            labels = np.asarray([p[1] for p in pairs], np.int32)
            samples.append({"path": str(path), "boxes": boxes, "labels": labels})
            if cap and len(samples) >= cap:
                break
        return names, samples

    val_cap = min(800, max_samples) if max_samples else 800
    names, train = load_split("train2017", max_samples)
    _, val = load_split("val2017", val_cap)
    if not names:
        names = list(COCO_CLASS_NAMES)
    if not train:
        raise FileNotFoundError("COCO 检测标注或图像不完整")
    return names, train, val or train[: max(1, len(train) // 10)]


def _from_class_arrays(dataset: str, image_size: int, keep_classes, max_samples):
    from datasets import load_dataset

    data = load_dataset(dataset, image_size=image_size, normalize=False, keep_classes=keep_classes, min_classes=1)
    names = list(data["class_names"])
    items = []

    def pack(x, y, cap):
        out = []
        y = np.asarray(y).reshape(-1)
        n = len(y) if not cap else min(len(y), cap)
        for i in range(n):
            out.append({
                "image": x[i],
                "boxes": np.array([[0.05, 0.05, 0.95, 0.95]], np.float32),
                "labels": np.array([int(y[i])], np.int32),
            })
        return out

    train = pack(data["x_train"], data["y_train"], max_samples)
    val = pack(data["x_test"], data["y_test"], min(800, max_samples or 800))
    return names, train, val, f"{dataset} 分类图转整图框（弱检测，建议 COCO/VOC）"
