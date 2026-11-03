"""SSDLite 锚框：生成、匹配、编码，与 ncnn PriorBox 顺序对齐。"""
from __future__ import annotations

import numpy as np

VARIANCES = (0.1, 0.1, 0.2, 0.2)


def expand_aspects(aspects: list[float], flip: bool = True) -> list[float]:
    out: list[float] = []
    for raw in aspects:
        ar = float(raw)
        if abs(ar - 1.0) < 1e-6:
            continue
        if all(abs(ar - x) > 1e-6 for x in out):
            out.append(ar)
        if flip:
            inv = 1.0 / ar
            if abs(inv - 1.0) > 1e-6 and all(abs(inv - x) > 1e-6 for x in out):
                out.append(inv)
    return out


def layer_min_max(image_size: int, num_maps: int, min_scale: float = 0.1, max_scale: float = 0.9):
    scales = np.linspace(min_scale, max_scale, num_maps + 1)
    mins = [float(image_size * s) for s in scales[:-1]]
    maxs = [float(image_size * s) for s in scales[1:]]
    return mins, maxs


def priors_for_map(
    feat_h: int,
    feat_w: int,
    image_size: int,
    min_size: float,
    max_size: float,
    aspects: list[float],
    offset: float = 0.5,
    clip: bool = False,
) -> np.ndarray:
    """返回 (P, 4) xmin ymin xmax ymax，归一化到 [0,1]，扫描顺序与 ncnn PriorBox 一致。"""
    ratios = expand_aspects(aspects, flip=True)
    step_y = image_size / float(feat_h)
    step_x = image_size / float(feat_w)
    boxes = []
    for i in range(feat_h):
        for j in range(feat_w):
            cy = (i + offset) * step_y
            cx = (j + offset) * step_x
            s = float(min_size)
            boxes.append(_center_box(cx, cy, s, s, image_size, clip))
            if max_size and max_size > 0:
                s2 = float(np.sqrt(min_size * max_size))
                boxes.append(_center_box(cx, cy, s2, s2, image_size, clip))
            for ar in ratios:
                w = min_size * np.sqrt(ar)
                h = min_size / np.sqrt(ar)
                boxes.append(_center_box(cx, cy, w, h, image_size, clip))
    if not boxes:
        return np.zeros((0, 4), np.float32)
    return np.asarray(boxes, np.float32)


def _center_box(cx, cy, w, h, image_size, clip: bool) -> list[float]:
    xmin = (cx - w / 2.0) / image_size
    ymin = (cy - h / 2.0) / image_size
    xmax = (cx + w / 2.0) / image_size
    ymax = (cy + h / 2.0) / image_size
    if clip:
        xmin, ymin = max(xmin, 0.0), max(ymin, 0.0)
        xmax, ymax = min(xmax, 1.0), min(ymax, 1.0)
    return [xmin, ymin, xmax, ymax]


def concat_priors(maps: list[dict], image_size: int, aspects: list[float], offset: float = 0.5) -> np.ndarray:
    chunks = [
        priors_for_map(
            int(item["h"]),
            int(item["w"]),
            image_size,
            float(item["min_size"]),
            float(item["max_size"]),
            aspects,
            offset=offset,
        )
        for item in maps
    ]
    return np.concatenate(chunks, axis=0) if chunks else np.zeros((0, 4), np.float32)


def iou_matrix(priors: np.ndarray, boxes: np.ndarray) -> np.ndarray:
    if boxes.size == 0:
        return np.zeros((len(priors), 0), np.float32)
    px1, py1, px2, py2 = priors[:, 0:1], priors[:, 1:2], priors[:, 2:3], priors[:, 3:4]
    gx1, gy1, gx2, gy2 = boxes[:, 0], boxes[:, 1], boxes[:, 2], boxes[:, 3]
    ix1 = np.maximum(px1, gx1)
    iy1 = np.maximum(py1, gy1)
    ix2 = np.minimum(px2, gx2)
    iy2 = np.minimum(py2, gy2)
    iw = np.maximum(ix2 - ix1, 0.0)
    ih = np.maximum(iy2 - iy1, 0.0)
    inter = iw * ih
    pa = np.maximum(px2 - px1, 0.0) * np.maximum(py2 - py1, 0.0)
    ga = np.maximum(gx2 - gx1, 0.0) * np.maximum(gy2 - gy1, 0.0)
    union = pa + ga - inter + 1e-7
    return (inter / union).astype(np.float32)


def decode_offsets(priors: np.ndarray, loc: np.ndarray, variances=VARIANCES) -> np.ndarray:
    """encode_offsets 的逆：loc(N,4) → 归一化 xmin ymin xmax ymax。"""
    pr_cx = (priors[:, 0] + priors[:, 2]) * 0.5
    pr_cy = (priors[:, 1] + priors[:, 3]) * 0.5
    pr_w = np.maximum(priors[:, 2] - priors[:, 0], 1e-6)
    pr_h = np.maximum(priors[:, 3] - priors[:, 1], 1e-6)
    vx, vy, vw, vh = variances
    loc = np.asarray(loc, np.float32)
    cx = loc[:, 0] * vx * pr_w + pr_cx
    cy = loc[:, 1] * vy * pr_h + pr_cy
    ww = np.exp(np.clip(loc[:, 2] * vw, -6.0, 6.0)) * pr_w
    hh = np.exp(np.clip(loc[:, 3] * vh, -6.0, 6.0)) * pr_h
    boxes = np.stack([cx - ww * 0.5, cy - hh * 0.5, cx + ww * 0.5, cy + hh * 0.5], axis=1)
    return np.clip(boxes, 0.0, 1.0).astype(np.float32)


def encode_offsets(priors: np.ndarray, boxes: np.ndarray, variances=VARIANCES) -> np.ndarray:
    pr_cx = (priors[:, 0] + priors[:, 2]) * 0.5
    pr_cy = (priors[:, 1] + priors[:, 3]) * 0.5
    pr_w = np.maximum(priors[:, 2] - priors[:, 0], 1e-6)
    pr_h = np.maximum(priors[:, 3] - priors[:, 1], 1e-6)
    gt_cx = (boxes[:, 0] + boxes[:, 2]) * 0.5
    gt_cy = (boxes[:, 1] + boxes[:, 3]) * 0.5
    gt_w = np.maximum(boxes[:, 2] - boxes[:, 0], 1e-6)
    gt_h = np.maximum(boxes[:, 3] - boxes[:, 1], 1e-6)
    vx, vy, vw, vh = variances
    loc = np.stack(
        [
            (gt_cx - pr_cx) / pr_w / vx,
            (gt_cy - pr_cy) / pr_h / vy,
            np.log(gt_w / pr_w) / vw,
            np.log(gt_h / pr_h) / vh,
        ],
        axis=1,
    )
    return loc.astype(np.float32)


def match_and_encode(
    boxes: np.ndarray,
    labels: np.ndarray,
    priors: np.ndarray,
    overlap_thresh: float = 0.4,
    variances=VARIANCES,
) -> tuple[np.ndarray, np.ndarray]:
    """labels 为 0 起的物体类别。返回 loc(P,4) 与 conf(P,)（0=背景，物体=label+1）。"""
    p = len(priors)
    loc_t = np.zeros((p, 4), np.float32)
    conf_t = np.zeros((p,), np.int32)
    if boxes is None or len(boxes) == 0:
        return loc_t, conf_t
    boxes = np.asarray(boxes, np.float32).reshape(-1, 4)
    labels = np.asarray(labels, np.int32).reshape(-1)
    keep = (boxes[:, 2] - boxes[:, 0] > 1e-4) & (boxes[:, 3] - boxes[:, 1] > 1e-4)
    boxes, labels = boxes[keep], labels[keep]
    if len(boxes) == 0:
        return loc_t, conf_t
    ious = iou_matrix(priors, boxes)
    best_gt = ious.argmax(axis=1)
    best_iou = ious.max(axis=1)
    best_prior = ious.argmax(axis=0)
    matched = best_iou >= overlap_thresh
    matched[best_prior] = True
    gt_for_prior = best_gt.copy()
    gt_for_prior[best_prior] = np.arange(len(boxes))
    pos = matched.copy()
    pos[best_prior] = True
    chosen = boxes[gt_for_prior]
    loc_t[pos] = encode_offsets(priors[pos], chosen[pos], variances)
    conf_t[pos] = labels[gt_for_prior[pos]] + 1
    conf_t[~pos] = 0
    return loc_t, conf_t
