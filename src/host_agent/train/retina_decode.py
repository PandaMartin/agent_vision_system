"""PC 端 RetinaFace 解码：对齐 ncnn examples/retinaface.cpp（variance=1）。"""
from __future__ import annotations

import numpy as np
from PIL import Image

CLS_BLOBS = (
    "face_rpn_cls_prob_reshape_stride8",
    "face_rpn_cls_prob_reshape_stride16",
    "face_rpn_cls_prob_reshape_stride32",
)
BBOX_BLOBS = (
    "face_rpn_bbox_pred_stride8",
    "face_rpn_bbox_pred_stride16",
    "face_rpn_bbox_pred_stride32",
)
LMK_BLOBS = (
    "face_rpn_landmark_pred_stride8",
    "face_rpn_landmark_pred_stride16",
    "face_rpn_landmark_pred_stride32",
)
CFG_IDS = (
    ("blob_cls8", "blob_bbox8", "blob_lmk8"),
    ("blob_cls16", "blob_bbox16", "blob_lmk16"),
    ("blob_cls32", "blob_bbox32", "blob_lmk32"),
)


def letterbox_infer(rgb: np.ndarray, size: int) -> tuple[np.ndarray, float, int, int]:
    arr = np.asarray(rgb)
    if arr.ndim == 2:
        arr = np.repeat(arr[..., None], 3, axis=-1)
    h, w = int(arr.shape[0]), int(arr.shape[1])
    scale = min(size / max(w, 1), size / max(h, 1))
    nw, nh = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    resized = np.asarray(
        Image.fromarray(arr[..., :3].astype(np.uint8)).resize((nw, nh), Image.BILINEAR),
        np.uint8,
    )
    canvas = np.zeros((size, size, 3), np.uint8)
    pad_x = (size - nw) // 2
    pad_y = (size - nh) // 2
    canvas[pad_y:pad_y + nh, pad_x:pad_x + nw] = resized
    return canvas, float(scale), int(pad_x), int(pad_y)


def unletterbox_xyxy(x0, y0, x1, y1, scale: float, pad_x: int, pad_y: int):
    inv = 1.0 / max(float(scale), 1e-6)
    return (
        (float(x0) - pad_x) * inv,
        (float(y0) - pad_y) * inv,
        (float(x1) - pad_x) * inv,
        (float(y1) - pad_y) * inv,
    )


def blob_names() -> list[str]:
    names = []
    for cls, box, lmk in zip(CLS_BLOBS, BBOX_BLOBS, LMK_BLOBS):
        names.extend([cls, box, lmk])
    return names


def blob_keys(cfg: dict | None) -> list:
    """优先用 blob 名；python ncnn 失败时回退 model.cfg 里的整数 ID。"""
    names = blob_names()
    cfg = cfg or {}
    ids = []
    for keys in CFG_IDS:
        ids.extend([cfg.get(k) for k in keys])
    if all(isinstance(v, int) and v >= 0 for v in ids):
        return names, ids
    return names, None


def _as_chw(arr: np.ndarray) -> np.ndarray:
    x = np.asarray(arr, np.float32)
    if x.ndim == 4:
        x = x[0]
    if x.ndim == 2:
        x = x[None, ...]
    if x.ndim != 3:
        raise RuntimeError(f"RetinaFace 特征图维度异常 {x.shape}")
    if x.shape[0] not in (4, 8, 20) and x.shape[-1] in (4, 8, 20):
        x = np.transpose(x, (2, 0, 1))
    return x


def _flatten_heads(cls: np.ndarray, box: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    cls = _as_chw(cls)
    box = _as_chw(box)
    _, h, w = box.shape
    loc = np.transpose(box, (1, 2, 0)).reshape(h * w, 2, 4).reshape(-1, 4)
    raw = np.transpose(cls, (1, 2, 0)).reshape(h * w, 4)
    a0 = np.stack([raw[:, 0], raw[:, 2]], axis=-1)
    a1 = np.stack([raw[:, 1], raw[:, 3]], axis=-1)
    conf = np.stack([a0, a1], axis=1).reshape(-1, 2)
    return loc.astype(np.float32), conf.astype(np.float32)


def decode_maps(
    blobs: dict,
    image_size: int,
    score_th: float,
    img_w: int,
    img_h: int,
    scale: float,
    pad_x: int,
    pad_y: int,
    max_keep: int = 32,
) -> tuple[list[dict], float]:
    """按 ncnn examples/retinaface.cpp 的 generate_proposals：用特征图实际宽高，而不是假定 320/stride。

    返回 (原图像素框, 阈值前的人脸分数峰值)。
    """
    from retina_priors import BASE_SIZE, SCALES, STRIDES

    boxes = []
    peak = 0.0
    thresh = float(score_th)
    for stride, scales, cls_name, box_name in zip(STRIDES, SCALES, CLS_BLOBS, BBOX_BLOBS):
        if cls_name not in blobs or box_name not in blobs:
            raise RuntimeError(f"缺少 RetinaFace 头 {cls_name} / {box_name}")
        cls = _as_chw(blobs[cls_name])
        box = _as_chw(blobs[box_name])
        _c, feat_h, feat_w = box.shape
        n_anc = len(scales)
        if cls.shape[0] < n_anc * 2 or box.shape[0] < n_anc * 4:
            raise RuntimeError(f"{cls_name}/{box_name} 通道数异常 {cls.shape} {box.shape}")
        if cls.shape[1] != feat_h or cls.shape[2] != feat_w:
            cls = cls[:, :feat_h, :feat_w]
        for q, sc in enumerate(scales):
            face = cls[q + n_anc]
            if face.size:
                peak = max(peak, float(np.max(face)))
            dx, dy, dw, dh = (box[q * 4 + k] for k in range(4))
            rs = BASE_SIZE * float(sc)
            ax0 = BASE_SIZE * 0.5 - rs * 0.5
            ay0 = BASE_SIZE * 0.5 - rs * 0.5
            ys, xs = np.where(face >= thresh)
            if ys.size == 0:
                continue
            dw_c = np.clip(dw[ys, xs], -8.0, 8.0)
            dh_c = np.clip(dh[ys, xs], -8.0, 8.0)
            anchor_x = ax0 + xs.astype(np.float32) * stride
            anchor_y = ay0 + ys.astype(np.float32) * stride
            cx = anchor_x + rs * 0.5
            cy = anchor_y + rs * 0.5
            pb_w = rs * np.exp(dw_c)
            pb_h = rs * np.exp(dh_c)
            pb_cx = cx + rs * dx[ys, xs]
            pb_cy = cy + rs * dy[ys, xs]
            x0 = pb_cx - pb_w * 0.5
            y0 = pb_cy - pb_h * 0.5
            x1 = pb_cx + pb_w * 0.5
            y1 = pb_cy + pb_h * 0.5
            scores = face[ys, xs]
            for i in range(int(ys.size)):
                ux0, uy0, ux1, uy1 = unletterbox_xyxy(
                    float(x0[i]), float(y0[i]), float(x1[i]), float(y1[i]),
                    scale, pad_x, pad_y,
                )
                if ux1 < ux0:
                    ux0, ux1 = ux1, ux0
                if uy1 < uy0:
                    uy0, uy1 = uy1, uy0
                boxes.append({
                    "x": float(ux0),
                    "y": float(uy0),
                    "w": float(ux1 - ux0),
                    "h": float(uy1 - uy0),
                    "class": 1,
                    "name": "face",
                    "conf": float(scores[i]),
                })
    boxes.sort(key=lambda b: b["conf"], reverse=True)
    _ = image_size, img_w, img_h
    return boxes[: max(1, int(max_keep)) * 8], float(peak)


def calib_letterbox_images(samples: list, image_size: int, count: int, pipeline=None) -> np.ndarray:
    from detect_data import _read_image_native
    from retina_priors import letterbox_rgb

    out = []
    for sample in samples[: max(int(count) * 2, int(count))]:
        image, boxes, _ = _read_image_native(sample)
        if pipeline:
            from image_ops import apply_pipeline

            image = apply_pipeline(image, pipeline)
        canvas, _ = letterbox_rgb(image, boxes, int(image_size))
        if canvas.max() > 1.5:
            canvas = canvas / 255.0
        out.append(canvas.astype(np.float32))
        if len(out) >= int(count):
            break
    if not out:
        raise ValueError("没有可用于校准的 RetinaFace 图像")
    return np.stack(out, axis=0)
