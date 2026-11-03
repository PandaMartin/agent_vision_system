"""RetinaFace 锚框：与 ncnn examples/retinaface.cpp 的 generate_anchors / proposals 对齐。"""
from __future__ import annotations

import numpy as np
from PIL import Image

from ssd_priors import encode_offsets, match_and_encode

RETINA_VARIANCES = (1.0, 1.0, 1.0, 1.0)
STRIDES = (8, 16, 32)
# 与 example 中 scales 顺序一致（channel 上锚 0 再锚 1）
SCALES = ((2.0, 1.0), (8.0, 4.0), (32.0, 16.0))
BASE_SIZE = 16.0


def generate_retina_priors(image_size: int) -> np.ndarray:
    """(P,4) xmin ymin xmax ymax，归一化到输入正方形。顺序：(stride) → 行 → 列 → 锚。"""
    chunks = []
    size = int(image_size)
    for stride, scales in zip(STRIDES, SCALES):
        feat = max(1, size // int(stride))
        layer = []
        for i in range(feat):
            for j in range(feat):
                for sc in scales:
                    rs = BASE_SIZE * float(sc)
                    cx = BASE_SIZE * 0.5
                    cy = BASE_SIZE * 0.5
                    x1 = (cx - rs * 0.5) + j * stride
                    y1 = (cy - rs * 0.5) + i * stride
                    x2 = x1 + rs
                    y2 = y1 + rs
                    layer.append([x1 / size, y1 / size, x2 / size, y2 / size])
        chunks.append(np.asarray(layer, np.float32))
    return np.concatenate(chunks, axis=0) if chunks else np.zeros((0, 4), np.float32)


def attach_retina_meta(model, image_size: int):
    priors = generate_retina_priors(image_size)
    model.priors = priors
    model.retina_cfg = {
        "image_size": int(image_size),
        "strides": list(STRIDES),
        "scales": [list(s) for s in SCALES],
        "base_size": BASE_SIZE,
        "num_priors": int(priors.shape[0]),
        "pixel": "bgr2rgb",
        "mean": [0.0, 0.0, 0.0],
        "norm": [1.0, 1.0, 1.0],
        "nms_threshold": 0.4,
        "confidence_threshold": 0.5,
        "keep_top_k": 32,
        "cls_blobs": [
            "face_rpn_cls_prob_reshape_stride8",
            "face_rpn_cls_prob_reshape_stride16",
            "face_rpn_cls_prob_reshape_stride32",
        ],
        "bbox_blobs": [
            "face_rpn_bbox_pred_stride8",
            "face_rpn_bbox_pred_stride16",
            "face_rpn_bbox_pred_stride32",
        ],
        "lmk_blobs": [
            "face_rpn_landmark_pred_stride8",
            "face_rpn_landmark_pred_stride16",
            "face_rpn_landmark_pred_stride32",
        ],
        "score_blobs": [
            "face_rpn_cls_score_stride8",
            "face_rpn_cls_score_stride16",
            "face_rpn_cls_score_stride32",
        ],
    }
    model.ssd_cfg = None
    return model


def letterbox_rgb(image: np.ndarray, boxes: np.ndarray, size: int):
    h, w = int(image.shape[0]), int(image.shape[1])
    scale = min(size / max(w, 1), size / max(h, 1))
    nw, nh = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    arr = np.clip(image, 0, 255).astype(np.uint8)
    if arr.ndim == 2:
        arr = np.repeat(arr[..., None], 3, axis=-1)
    resized = np.asarray(Image.fromarray(arr).resize((nw, nh), Image.BILINEAR), np.float32)
    canvas = np.zeros((size, size, 3), np.float32)
    pad_x = (size - nw) // 2
    pad_y = (size - nh) // 2
    canvas[pad_y:pad_y + nh, pad_x:pad_x + nw] = resized[..., :3]
    out = np.asarray(boxes, np.float32).reshape(-1, 4).copy()
    if len(out):
        out[:, [0, 2]] = out[:, [0, 2]] * nw + pad_x
        out[:, [1, 3]] = out[:, [1, 3]] * nh + pad_y
        out /= float(size)
        out = np.clip(out, 0.0, 1.0)
    return canvas, out


def encode_retina_sample(sample: dict, priors: np.ndarray, image_size: int, augment: bool, rng: np.random.Generator, pipeline=None):
    from detect_data import _photometric, _read_image_native, _zoom_to_faces

    image, boxes, labels = _read_image_native(sample)
    if augment and len(boxes):
        image = _photometric(image, rng)
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
    image, boxes = letterbox_rgb(image, boxes, int(image_size))
    loc_t, conf_t = match_and_encode(boxes, labels, priors, overlap_thresh=0.35, variances=RETINA_VARIANCES)
    image = image.astype(np.float32)
    if image.max() <= 1.5:
        image = image * 255.0
    # RGB 0–255，不减均值（板上 PIXEL_BGR2RGB + mean=0）
    return image, loc_t, conf_t
