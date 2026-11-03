"""在 PC 上用 loc/conf 特征图解码 SSD 框。

python ncnn 跑 DetectionOutput 会崩或给出空结果；INT8 分类头还会把人脸分数压到 <1%。
这里按训练时的 NHWC 展平顺序解框，与 ssd_priors 锚框对齐。
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from ssd_priors import VARIANCES, decode_offsets, priors_for_map


def load_ssd_cfg(pack: Path, info: dict | None = None) -> dict | None:
    pack = Path(pack)
    if info and isinstance(info.get("ssd_cfg"), dict):
        return info["ssd_cfg"]
    for name in ("train_info.json", "ssd_cfg.json", "deployment_config.json"):
        path = pack / name
        if not path.exists():
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(data.get("ssd_cfg"), dict):
            return data["ssd_cfg"]
        if data.get("maps"):
            return data
    return None


def _as_chw(arr: np.ndarray, channels: int) -> np.ndarray:
    x = np.asarray(arr, np.float32)
    if x.ndim == 4:
        x = x[0]
    if x.ndim == 2:
        x = x[None, ...]
    if x.ndim != 3:
        raise ValueError(f"特征图应为 CHW，实际 {x.shape}")
    if int(x.shape[0]) == int(channels):
        return x
    if int(x.shape[-1]) == int(channels):
        return np.transpose(x, (2, 0, 1))
    return x


def softmax_class(conf_chw: np.ndarray, anchors: int, num_classes: int) -> np.ndarray:
    """conf (A*(C+1), H, W) → (H*W*A, C+1) 概率。"""
    c, h, w = conf_chw.shape
    cols = int(anchors) * (int(num_classes) + 1)
    if c != cols:
        raise ValueError(f"conf 通道 {c} 与 anchors*{num_classes + 1}={cols} 不符")
    logits = np.transpose(conf_chw, (1, 2, 0)).reshape(-1, num_classes + 1)
    logits = logits - logits.max(axis=1, keepdims=True)
    exp = np.exp(np.clip(logits, -80.0, 80.0))
    return (exp / np.maximum(exp.sum(axis=1, keepdims=True), 1e-8)).astype(np.float32)


def loc_to_offsets(loc_chw: np.ndarray, anchors: int) -> np.ndarray:
    c, h, w = loc_chw.shape
    if c != anchors * 4:
        raise ValueError(f"loc 通道 {c} 与 anchors*4={anchors * 4} 不符")
    return np.transpose(loc_chw, (1, 2, 0)).reshape(-1, 4).astype(np.float32)


def decode_pack_maps(
    blobs: dict[str, np.ndarray],
    ssd_cfg: dict,
) -> tuple[np.ndarray, np.ndarray]:
    """返回 boxes(N,4) 归一化 xyxy，scores(N, C+1)。"""
    maps = list(ssd_cfg.get("maps") or [])
    if not maps:
        raise ValueError("ssd_cfg 缺少 maps")
    anchors = int(ssd_cfg.get("anchors") or 4)
    num_classes = int(ssd_cfg.get("num_classes") or 1)
    aspects = [float(a) for a in (ssd_cfg.get("aspects") or [2.0])]
    image_size = int(ssd_cfg.get("image_size") or 300)
    variances = tuple(float(v) for v in (ssd_cfg.get("variances") or VARIANCES))
    offset = float(ssd_cfg.get("offset") or 0.5)
    box_chunks, score_chunks = [], []
    for item in maps:
        loc_name = str(item.get("loc") or f"ssd_loc_{item.get('index', 0)}")
        conf_name = str(item.get("conf") or f"ssd_conf_{item.get('index', 0)}")
        if loc_name not in blobs or conf_name not in blobs:
            raise KeyError(f"缺少特征 {loc_name}/{conf_name}")
        loc = loc_to_offsets(_as_chw(blobs[loc_name], anchors * 4), anchors)
        scores = softmax_class(_as_chw(blobs[conf_name], anchors * (num_classes + 1)), anchors, num_classes)
        h, w = int(item["h"]), int(item["w"])
        priors = priors_for_map(
            h, w, image_size, float(item["min_size"]), float(item["max_size"]),
            aspects, offset=offset,
        )
        n = min(len(priors), len(loc), len(scores))
        box_chunks.append(decode_offsets(priors[:n], loc[:n], variances))
        score_chunks.append(scores[:n])
    return np.concatenate(box_chunks, axis=0), np.concatenate(score_chunks, axis=0)


def detections_from_scores(
    boxes_xyxy: np.ndarray,
    scores: np.ndarray,
    class_names: list[str],
    img_w: int,
    img_h: int,
    score_thresh: float,
    max_keep: int = 32,
) -> list[dict]:
    """scores[:,0] 背景，[:,1:] 物体。"""
    out = []
    if scores.ndim != 2 or scores.shape[1] < 2:
        return out
    for cls in range(1, scores.shape[1]):
        name = class_names[cls] if cls < len(class_names) else str(cls)
        conf = scores[:, cls]
        keep = np.where(conf >= score_thresh)[0]
        for i in keep:
            x0, y0, x1, y1 = [float(v) for v in boxes_xyxy[i]]
            out.append({
                "x": x0 * img_w,
                "y": y0 * img_h,
                "w": (x1 - x0) * img_w,
                "h": (y1 - y0) * img_h,
                "class": int(cls),
                "name": name,
                "conf": float(conf[i]),
            })
    out.sort(key=lambda b: b["conf"], reverse=True)
    return out[: max(max_keep * 8, 64)]


def map_blob_names(ssd_cfg: dict) -> list[str]:
    names = []
    for item in ssd_cfg.get("maps") or []:
        names.append(str(item.get("loc") or f"ssd_loc_{item.get('index', 0)}"))
        names.append(str(item.get("conf") or f"ssd_conf_{item.get('index', 0)}"))
    return names
