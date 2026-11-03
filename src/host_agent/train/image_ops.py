"""可串联的图像处理积木。输入输出均为 HWC uint8，训练前在归一化之前套用。"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

OP_SPECS = [
    {
        "id": "gaussian",
        "name": "高斯滤波",
        "hint": "平滑噪声",
        "params": [
            {"key": "ksize", "label": "核大小", "type": "int", "min": 3, "max": 21, "step": 2, "default": 5},
            {"key": "sigma", "label": "Sigma", "type": "float", "min": 0.0, "max": 8.0, "step": 0.1, "default": 1.0},
        ],
    },
    {
        "id": "median",
        "name": "中值滤波",
        "hint": "去椒盐噪声",
        "params": [
            {"key": "ksize", "label": "核大小", "type": "int", "min": 3, "max": 15, "step": 2, "default": 5},
        ],
    },
    {
        "id": "box_blur",
        "name": "均值滤波",
        "hint": "简单模糊",
        "params": [
            {"key": "ksize", "label": "核大小", "type": "int", "min": 3, "max": 21, "step": 2, "default": 5},
        ],
    },
    {
        "id": "sobel",
        "name": "Sobel 边缘",
        "hint": "一阶梯度边缘",
        "params": [
            {"key": "ksize", "label": "核大小", "type": "int", "min": 1, "max": 7, "step": 2, "default": 3},
            {"key": "axis", "label": "方向", "type": "choice", "choices": ["both", "x", "y"], "default": "both"},
        ],
    },
    {
        "id": "canny",
        "name": "Canny 边缘",
        "hint": "双阈值边缘",
        "params": [
            {"key": "low", "label": "低阈值", "type": "int", "min": 0, "max": 255, "step": 1, "default": 50},
            {"key": "high", "label": "高阈值", "type": "int", "min": 0, "max": 255, "step": 1, "default": 150},
        ],
    },
    {
        "id": "binary",
        "name": "二值化",
        "hint": "阈值分割",
        "params": [
            {"key": "method", "label": "方法", "type": "choice", "choices": ["fixed", "otsu", "adaptive"], "default": "otsu"},
            {"key": "thresh", "label": "阈值", "type": "int", "min": 0, "max": 255, "step": 1, "default": 127},
            {"key": "block", "label": "自适应块", "type": "int", "min": 3, "max": 51, "step": 2, "default": 11},
        ],
    },
    {
        "id": "sharpen",
        "name": "锐化",
        "hint": "反锐化掩模",
        "params": [
            {"key": "amount", "label": "强度", "type": "float", "min": 0.1, "max": 5.0, "step": 0.1, "default": 1.5},
            {"key": "radius", "label": "半径", "type": "int", "min": 1, "max": 9, "step": 2, "default": 3},
        ],
    },
    {
        "id": "white_balance",
        "name": "白平衡",
        "hint": "校正色偏",
        "params": [
            {"key": "method", "label": "方法", "type": "choice", "choices": ["gray_world", "manual"], "default": "gray_world"},
            {"key": "r", "label": "R 增益", "type": "float", "min": 0.2, "max": 3.0, "step": 0.05, "default": 1.0},
            {"key": "g", "label": "G 增益", "type": "float", "min": 0.2, "max": 3.0, "step": 0.05, "default": 1.0},
            {"key": "b", "label": "B 增益", "type": "float", "min": 0.2, "max": 3.0, "step": 0.05, "default": 1.0},
        ],
    },
    {
        "id": "brightness",
        "name": "亮度",
        "hint": "加减像素",
        "params": [
            {"key": "beta", "label": "偏移", "type": "int", "min": -100, "max": 100, "step": 1, "default": 20},
        ],
    },
    {
        "id": "contrast",
        "name": "对比度",
        "hint": "乘性拉伸",
        "params": [
            {"key": "alpha", "label": "系数", "type": "float", "min": 0.2, "max": 3.0, "step": 0.05, "default": 1.3},
        ],
    },
    {
        "id": "hist_eq",
        "name": "直方图均衡",
        "hint": "拉开灰度分布",
        "params": [],
    },
    {
        "id": "clahe",
        "name": "CLAHE",
        "hint": "局部均衡",
        "params": [
            {"key": "clip", "label": "裁剪限制", "type": "float", "min": 0.5, "max": 8.0, "step": 0.1, "default": 2.0},
            {"key": "grid", "label": "网格", "type": "int", "min": 2, "max": 16, "step": 1, "default": 8},
        ],
    },
    {
        "id": "grayscale",
        "name": "灰度",
        "hint": "转灰度再还原通道",
        "params": [],
    },
    {
        "id": "invert",
        "name": "反色",
        "hint": "255 减像素",
        "params": [],
    },
]

_SPEC_BY_ID = {item["id"]: item for item in OP_SPECS}


def spec_of(op_id: str) -> dict:
    spec = _SPEC_BY_ID.get(op_id)
    if spec is None:
        raise ValueError(f"未知图像处理积木: {op_id}")
    return spec


def default_params(op_id: str) -> dict:
    return {item["key"]: item["default"] for item in spec_of(op_id).get("params") or [] if "default" in item}


def normalize_steps(raw) -> list[dict]:
    steps = []
    for item in raw or []:
        if isinstance(item, str):
            op_id = item
            params = {}
        else:
            op_id = str(item.get("op") or item.get("id") or "").strip()
            params = dict(item.get("params") or {})
        if not op_id:
            continue
        spec = spec_of(op_id)
        merged = default_params(op_id)
        merged.update({k: params[k] for k in params if k in merged or not spec.get("params")})
        if spec.get("params"):
            allowed = {p["key"] for p in spec["params"]}
            merged = {k: v for k, v in merged.items() if k in allowed}
            for p in spec["params"]:
                if p["key"] not in merged and "default" in p:
                    merged[p["key"]] = p["default"]
        steps.append({"op": op_id, "params": merged})
    return steps


def parse_image_ops(raw) -> list[dict]:
    if raw is None or raw == "" or raw == []:
        return []
    if isinstance(raw, list):
        return normalize_steps(raw)
    text = str(raw).strip()
    if not text:
        return []
    path = Path(text)
    if path.is_file():
        text = path.read_text(encoding="utf-8")
    data = json.loads(text)
    if isinstance(data, dict):
        data = data.get("ops") or data.get("steps") or []
    return normalize_steps(data)


def to_uint8(image: np.ndarray) -> np.ndarray:
    arr = np.asarray(image)
    if arr.ndim == 2:
        arr = arr[..., None]
    if arr.dtype != np.uint8:
        arr = arr.astype(np.float32)
        if arr.size and float(np.max(arr)) <= 1.5:
            arr = arr * 255.0
        arr = np.clip(arr, 0, 255).astype(np.uint8)
    return np.ascontiguousarray(arr)


def apply_pipeline(image: np.ndarray, steps) -> np.ndarray:
    arr = to_uint8(image)
    for step in normalize_steps(steps):
        arr = apply_op(arr, step["op"], step.get("params") or {})
    return arr


def apply_pipeline_batch(images: np.ndarray, steps) -> np.ndarray:
    steps = normalize_steps(steps)
    arr = np.asarray(images)
    if not steps or arr.size == 0:
        return arr
    was_float = np.issubdtype(arr.dtype, np.floating)
    scaled = bool(was_float and arr.size and float(np.max(arr)) <= 1.5)
    out = [apply_pipeline(im, steps) for im in arr]
    stacked = np.stack(out, axis=0)
    if scaled:
        return stacked.astype(np.float32) / 255.0
    if was_float:
        return stacked.astype(np.float32)
    return stacked


def apply_op(image: np.ndarray, op_id: str, params: dict | None = None) -> np.ndarray:
    params = params or {}
    src = to_uint8(image)
    fn = _OPS.get(op_id)
    if fn is None:
        raise ValueError(f"未知图像处理积木: {op_id}")
    out = fn(src, params)
    out = to_uint8(out)
    if out.shape[-1] != src.shape[-1]:
        if src.shape[-1] == 1 and out.shape[-1] == 3:
            out = _as_gray(out)[..., None]
        elif src.shape[-1] == 3 and out.shape[-1] == 1:
            out = np.repeat(out, 3, axis=-1)
    if out.shape[:2] != src.shape[:2]:
        out = _resize_hw(out, src.shape[0], src.shape[1])
    return out


def _odd(value, default=3, minimum=1) -> int:
    try:
        v = int(round(float(value)))
    except (TypeError, ValueError):
        v = default
    v = max(minimum, v)
    if v % 2 == 0:
        v += 1
    return v


def _float(value, default=0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return float(default)


def _int(value, default=0) -> int:
    try:
        return int(round(float(value)))
    except (TypeError, ValueError):
        return int(default)


def _cv2():
    import cv2

    return cv2


def _as_gray(img: np.ndarray) -> np.ndarray:
    if img.ndim == 2:
        return img
    if img.shape[-1] == 1:
        return img[..., 0]
    r, g, b = img[..., 0].astype(np.float32), img[..., 1].astype(np.float32), img[..., 2].astype(np.float32)
    return np.clip(0.299 * r + 0.587 * g + 0.114 * b, 0, 255).astype(np.uint8)


def _restore(gray: np.ndarray, like: np.ndarray) -> np.ndarray:
    gray = np.asarray(gray)
    if gray.ndim == 3:
        gray = gray[..., 0]
    if like.ndim == 2:
        return gray
    if like.shape[-1] == 1:
        return gray[..., None]
    return np.repeat(gray[..., None], like.shape[-1], axis=-1)


def _resize_hw(img: np.ndarray, h: int, w: int) -> np.ndarray:
    cv2 = _cv2()
    return cv2.resize(img, (w, h), interpolation=cv2.INTER_LINEAR)


def _cv_in(img: np.ndarray) -> np.ndarray:
    if img.ndim == 3 and img.shape[-1] == 1:
        return img[..., 0]
    return img


def _cv_out(out: np.ndarray, like: np.ndarray) -> np.ndarray:
    out = np.asarray(out)
    if like.ndim == 3 and like.shape[-1] == 1 and out.ndim == 2:
        return out[..., None]
    if out.ndim == 2 and like.ndim == 3 and like.shape[-1] == 3:
        return np.repeat(out[..., None], 3, axis=-1)
    return out


def _op_gaussian(img, params):
    cv2 = _cv2()
    k = _odd(params.get("ksize"), 5)
    sigma = _float(params.get("sigma"), 1.0)
    return _cv_out(cv2.GaussianBlur(_cv_in(img), (k, k), sigmaX=sigma), img)


def _op_median(img, params):
    cv2 = _cv2()
    k = _odd(params.get("ksize"), 5)
    return _cv_out(cv2.medianBlur(_cv_in(img), k), img)


def _op_box(img, params):
    cv2 = _cv2()
    k = _odd(params.get("ksize"), 5)
    return _cv_out(cv2.blur(_cv_in(img), (k, k)), img)


def _op_sobel(img, params):
    cv2 = _cv2()
    gray = _as_gray(img)
    k = _odd(params.get("ksize"), 3)
    k = 1 if k < 1 else min(k, 7)
    axis = str(params.get("axis") or "both").lower()
    dx = 0 if axis == "y" else 1
    dy = 0 if axis == "x" else 1
    if dx == 0 and dy == 0:
        dx, dy = 1, 1
    zeros = np.zeros(gray.shape, np.float32)
    gx = cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=k) if dx else zeros
    gy = cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=k) if dy else zeros
    mag = cv2.magnitude(gx, gy)
    mag = cv2.normalize(mag, None, 0, 255, cv2.NORM_MINMAX)
    return _restore(mag.astype(np.uint8), img)


def _op_canny(img, params):
    cv2 = _cv2()
    low = _int(params.get("low"), 50)
    high = max(_int(params.get("high"), 150), low + 1)
    edges = cv2.Canny(_as_gray(img), low, high)
    return _restore(edges, img)


def _op_binary(img, params):
    cv2 = _cv2()
    gray = _as_gray(img)
    method = str(params.get("method") or "otsu").lower()
    thresh = _int(params.get("thresh"), 127)
    if method == "adaptive":
        block = _odd(params.get("block"), 11, minimum=3)
        out = cv2.adaptiveThreshold(gray, 255, cv2.ADAPTIVE_THRESH_MEAN_C, cv2.THRESH_BINARY, block, 2)
    elif method == "fixed":
        _, out = cv2.threshold(gray, thresh, 255, cv2.THRESH_BINARY)
    else:
        _, out = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    return _restore(out, img)


def _op_sharpen(img, params):
    cv2 = _cv2()
    amount = _float(params.get("amount"), 1.5)
    radius = _odd(params.get("radius"), 3)
    src = _cv_in(img)
    blur = cv2.GaussianBlur(src, (radius, radius), 0)
    sharp = cv2.addWeighted(src, 1.0 + amount, blur, -amount, 0)
    return _cv_out(sharp, img)


def _op_white_balance(img, params):
    method = str(params.get("method") or "gray_world").lower()
    if img.shape[-1] == 1:
        return img
    pix = img.astype(np.float32)
    if method == "manual":
        gains = np.array([_float(params.get("r"), 1.0), _float(params.get("g"), 1.0), _float(params.get("b"), 1.0)], np.float32)
    else:
        means = np.maximum(pix.reshape(-1, 3).mean(axis=0), 1.0)
        gray = float(means.mean())
        gains = gray / means
    out = pix * gains.reshape(1, 1, 3)
    return np.clip(out, 0, 255).astype(np.uint8)


def _op_brightness(img, params):
    cv2 = _cv2()
    beta = _int(params.get("beta"), 20)
    return _cv_out(cv2.convertScaleAbs(_cv_in(img), alpha=1.0, beta=beta), img)


def _op_contrast(img, params):
    cv2 = _cv2()
    alpha = _float(params.get("alpha"), 1.3)
    return _cv_out(cv2.convertScaleAbs(_cv_in(img), alpha=alpha, beta=0), img)


def _op_hist_eq(img, params):
    cv2 = _cv2()
    if img.shape[-1] == 1:
        return cv2.equalizeHist(_as_gray(img))[..., None]
    yuv = cv2.cvtColor(img, cv2.COLOR_RGB2YCrCb)
    yuv[:, :, 0] = cv2.equalizeHist(yuv[:, :, 0])
    return cv2.cvtColor(yuv, cv2.COLOR_YCrCb2RGB)


def _op_clahe(img, params):
    cv2 = _cv2()
    clip = _float(params.get("clip"), 2.0)
    grid = max(2, _int(params.get("grid"), 8))
    clahe = cv2.createCLAHE(clipLimit=clip, tileGridSize=(grid, grid))
    if img.shape[-1] == 1:
        return clahe.apply(_as_gray(img))[..., None]
    lab = cv2.cvtColor(img, cv2.COLOR_RGB2LAB)
    lab[:, :, 0] = clahe.apply(lab[:, :, 0])
    return cv2.cvtColor(lab, cv2.COLOR_LAB2RGB)


def _op_gray(img, params):
    return _restore(_as_gray(img), img)


def _op_invert(img, params):
    return 255 - img


_OPS = {
    "gaussian": _op_gaussian,
    "median": _op_median,
    "box_blur": _op_box,
    "sobel": _op_sobel,
    "canny": _op_canny,
    "binary": _op_binary,
    "sharpen": _op_sharpen,
    "white_balance": _op_white_balance,
    "brightness": _op_brightness,
    "contrast": _op_contrast,
    "hist_eq": _op_hist_eq,
    "clahe": _op_clahe,
    "grayscale": _op_gray,
    "invert": _op_invert,
}


def format_ops_chain(steps) -> str:
    bits = []
    for step in normalize_steps(steps):
        spec = spec_of(step["op"])
        extra = summarize_step(step)
        bits.append(f"{spec['name']}({extra})" if extra else spec["name"])
    return " → ".join(bits)


def write_image_ops_cfg(path, steps) -> Path:
    """上位机训练记录。板子不读本文件，封包时把同一条链写进 model.mod。"""
    path = Path(path)
    steps = normalize_steps(steps)
    lines = [
        "# 上位机训练前图像预处理记录。板子不读本文件。",
        "# 积木在 RGB uint8 上计算，封包时写进 model.mod，在进网络之前对整帧执行。",
        "# 顺序: ops → pixel → mean/norm。",
        f"count = {len(steps)}",
        f"chain = {format_ops_chain(steps) or 'none'}",
        "color = rgb",
        "order = ops,pixel,mean_norm",
    ]
    for i, step in enumerate(steps):
        spec = spec_of(step["op"])
        lines.append(f"op{i} = {step['op']}")
        lines.append(f"op{i}.name = {spec['name']}")
        for key, val in (step.get("params") or {}).items():
            lines.append(f"op{i}.{key} = {val}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def summarize_step(step: dict) -> str:
    spec = spec_of(step["op"])
    params = step.get("params") or {}
    bits = []
    for item in spec.get("params") or []:
        key = item["key"]
        if key not in params:
            continue
        val = params[key]
        if item["type"] == "choice" and str(params.get(item["key"])) == str(item.get("default")):
            if len(spec["params"]) > 1:
                continue
        if item["type"] == "float":
            bits.append(f"{item['label']}={float(val):g}")
        else:
            bits.append(f"{item['label']}={val}")
    return "  ".join(bits)
