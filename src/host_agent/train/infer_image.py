"""用已导出的 ncnn 模型做单张图识别，或滑窗检测。"""
from __future__ import annotations

import stdio_utf8  # noqa: F401

import argparse
import os
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

PREVIEW_DIR = Path(tempfile.gettempdir()) / "fpga_ncnn_preview"

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ncnn_infer import NcnnSimulator, load_pack_param_bin  # noqa: E402
from simulate_fpga import find_ncnn_pack, to_nchw  # noqa: E402


def preprocess_array(arr: np.ndarray, height: int, width: int, channels: int, normalize: bool) -> np.ndarray:
    from PIL import Image

    if arr.ndim == 2:
        image = Image.fromarray(arr.astype(np.uint8), mode="L")
    else:
        image = Image.fromarray(arr.astype(np.uint8))
    if channels == 1:
        image = image.convert("L")
    else:
        image = image.convert("RGB")
    image = image.resize((width, height), Image.BILINEAR)
    data = np.asarray(image, dtype=np.float32)
    if channels == 1 and data.ndim == 2:
        data = data[..., None]
    if normalize and data.max() > 1.5:
        data = data / 255.0
    return data


def load_user_image(path: Path) -> np.ndarray:
    from PIL import Image

    image = Image.open(path)
    image = image.convert("RGB")
    return np.asarray(image, dtype=np.uint8)


def softmax(logits: np.ndarray) -> np.ndarray:
    x = np.asarray(logits, np.float32).reshape(-1)
    if x.size == 0:
        return x
    x = x - np.max(x)
    exp = np.exp(x)
    total = float(np.sum(exp))
    if total <= 0:
        return np.zeros_like(x)
    return exp / total


def _is_retina_pack(cfg: dict) -> bool:
    post = str(cfg.get("post") or "").lower()
    if post == "retinaface":
        return True
    name = str(cfg.get("name") or cfg.get("folder") or cfg.get("arch") or "").lower()
    return "retina" in name


def _is_detect_pack(cfg: dict) -> bool:
    post = str(cfg.get("post") or "").lower()
    if post in ("detect", "retinaface"):
        return True
    name = str(cfg.get("name") or cfg.get("folder") or "").lower()
    return "ssdlite" in name or "ssd" in name or "retina" in name


def resolve_pack(dataset: str | None, pack: str | None, prefer: str | None = None) -> Path:
    if pack:
        return Path(pack)
    return find_ncnn_pack(dataset, prefer=prefer)


def preprocess_for_pack(rgb: np.ndarray, cfg: dict) -> np.ndarray:
    """对齐板上 from_pixels + substract_mean_normalize（像素 0–255）。"""
    from PIL import Image

    size = int(cfg.get("image_size") or cfg.get("input_size") or 224)
    shape = cfg.get("input_shape_nchw")
    channels = int(shape[0]) if shape else int(cfg.get("channels") or 3)
    pixel = str(cfg.get("pixel") or "rgb").lower()
    image = Image.fromarray(rgb.astype(np.uint8))
    if channels == 1:
        image = image.convert("L").resize((size, size), Image.BILINEAR)
        chw = np.asarray(image, dtype=np.float32)[None, ...]
    else:
        image = image.convert("RGB").resize((size, size), Image.BILINEAR)
        arr = np.asarray(image, dtype=np.float32)
        if pixel in ("bgr", "rgb2bgr"):
            arr = arr[..., ::-1]
        chw = np.transpose(arr, (2, 0, 1))
    mean = cfg.get("mean")
    norm = cfg.get("norm")
    if mean is not None and norm is not None:
        mean_t = np.asarray(mean, np.float32).reshape(-1)
        norm_t = np.asarray(norm, np.float32).reshape(-1)
        c = chw.shape[0]
        if mean_t.size == 1:
            mean_t = np.repeat(mean_t, c)
            norm_t = np.repeat(norm_t, c)
        chw = (chw - mean_t[:c, None, None]) * norm_t[:c, None, None]
    elif cfg.get("normalize", True):
        chw = chw / 255.0
    return chw


def _merge_train_info(pack: Path, cfg: dict) -> dict:
    info_path = Path(pack) / "train_info.json"
    if not info_path.exists():
        return cfg
    try:
        import json

        info = json.loads(info_path.read_text(encoding="utf-8"))
    except Exception:
        return cfg
    merged = dict(cfg)
    if isinstance(info.get("ssd_cfg"), dict) and not merged.get("ssd_cfg"):
        merged["ssd_cfg"] = info["ssd_cfg"]
    if isinstance(info.get("retina_cfg"), dict) and not merged.get("retina_cfg"):
        merged["retina_cfg"] = info["retina_cfg"]
    if info.get("post") and not merged.get("post"):
        merged["post"] = info["post"]
    if info.get("class_names") and not merged.get("class_names"):
        merged["class_names"] = list(info["class_names"])
    for key in ("fp32_param", "fp32_bin"):
        if info.get(key):
            merged.setdefault(key, info[key])
    return merged


def _prefer_fp32_weights(pack: Path, param: Path, bin_path: Path, cfg: dict) -> tuple[Path, Path, dict]:
    """PC 检测避开 INT8：conf 量化后人脸 softmax 只有约 0.6%，DetectionOutput 还会崩。"""
    pack = Path(pack)
    cfg = dict(cfg)
    named_param = pack / str(cfg.get("fp32_param") or "")
    named_bin = pack / str(cfg.get("fp32_bin") or "")
    if named_param.is_file() and named_bin.is_file():
        fp_param, fp_bin = named_param, named_bin
    else:
        params = sorted(pack.glob("*_fp32.param"))
        bins = sorted(p for p in pack.glob("*_fp32.bin") if p.name != "model.param.bin")
        if not params or not bins:
            return param, bin_path, cfg
        fp_param, fp_bin = params[0], bins[0]
    if fp_param.resolve() == Path(param).resolve() and fp_bin.resolve() == Path(bin_path).resolve():
        return param, bin_path, cfg
    print(f"PC 检测使用 fp32 权重: {fp_param.name} + {fp_bin.name}", flush=True)
    cfg["pc_weights"] = "fp32"
    cfg["input_blob"] = cfg.get("input_blob") if isinstance(cfg.get("input_blob"), str) else "data"
    return fp_param, fp_bin, cfg


def open_simulator(pack: Path) -> tuple[NcnnSimulator, dict]:
    param, bin_path, cfg = load_pack_param_bin(pack)
    cfg = _merge_train_info(pack, cfg)
    if _is_detect_pack(cfg):
        param, bin_path, cfg = _prefer_fp32_weights(pack, param, bin_path, cfg)
    sim = NcnnSimulator(
        param,
        bin_path,
        input_blob=cfg.get("input_blob", "data"),
        output_blob=cfg.get("output_blob"),
        config=cfg,
    )
    return sim, cfg


def _run_net(sim: NcnnSimulator, rgb: np.ndarray, cfg: dict) -> tuple[np.ndarray, float]:
    if sim.backend == "ncnn":
        return sim.invoke_rgb(rgb)
    return sim.invoke(preprocess_for_pack(rgb, cfg))


def write_preview(rgb: np.ndarray, boxes: list[dict]) -> Path:
    """把带框预览写到临时目录，不污染用户原图所在文件夹。"""
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
    for old in PREVIEW_DIR.glob("*.png"):
        try:
            old.unlink()
        except OSError:
            pass
    path = PREVIEW_DIR / f"preview_{os.getpid()}_{int(time.time() * 1000)}.png"
    draw_boxes(rgb, boxes, path)
    return path


def classify_array(sim: NcnnSimulator, image_hwc: np.ndarray, class_names: list[str], cfg: dict | None = None) -> dict:
    cfg = cfg or sim.config or {}
    if sim.backend == "ncnn":
        output, elapsed_us = sim.invoke_rgb(image_hwc)
    elif image_hwc.ndim == 3 and image_hwc.shape[0] in (1, 3) and image_hwc.shape[-1] not in (1, 3):
        output, elapsed_us = sim.invoke(to_nchw(image_hwc))
    else:
        output, elapsed_us = sim.invoke(preprocess_for_pack(image_hwc, cfg))
    arr = np.asarray(output, np.float32)
    if arr.size == 0:
        return {
            "class": -1,
            "name": "none",
            "conf": 0.0,
            "top": [],
            "elapsed_us": float(elapsed_us),
        }
    probs = softmax(arr)
    if probs.size == 0:
        return {
            "class": -1,
            "name": "none",
            "conf": 0.0,
            "top": [],
            "elapsed_us": float(elapsed_us),
        }
    order = np.argsort(probs)[::-1]
    top = []
    for idx in order[:3]:
        name = class_names[idx] if idx < len(class_names) else str(int(idx))
        top.append({"class": int(idx), "name": name, "conf": float(probs[idx])})
    best = top[0]
    return {
        "class": best["class"],
        "name": best["name"],
        "conf": best["conf"],
        "top": top,
        "elapsed_us": float(elapsed_us),
    }


def nms(boxes: list[dict], iou_thresh: float = 0.35) -> list[dict]:
    if not boxes:
        return []
    boxes = sorted(boxes, key=lambda b: b["conf"], reverse=True)
    keep = []
    while boxes:
        cur = boxes.pop(0)
        keep.append(cur)
        boxes = [b for b in boxes if _iou(cur, b) < iou_thresh or b["class"] != cur["class"]]
    return keep


def _iou(a: dict, b: dict) -> float:
    ax2, ay2 = a["x"] + a["w"], a["y"] + a["h"]
    bx2, by2 = b["x"] + b["w"], b["y"] + b["h"]
    ix1, iy1 = max(a["x"], b["x"]), max(a["y"], b["y"])
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    inter = max(0, ix2 - ix1) * max(0, iy2 - iy1)
    union = a["w"] * a["h"] + b["w"] * b["h"] - inter
    return inter / union if union else 0.0


def _clip_box(box: dict, width: int, height: int) -> dict | None:
    """把检测框裁到图像内。与图像几乎不相交的巨框直接丢掉，避免被拉成整张图。"""
    try:
        x = float(box.get("x", 0))
        y = float(box.get("y", 0))
        w = float(box.get("w", 0))
        h = float(box.get("h", 0))
    except (TypeError, ValueError):
        return None
    if not np.isfinite([x, y, w, h]).all() or w < 1 or h < 1:
        return None
    x2, y2 = x + w, y + h
    ix1, iy1 = max(x, 0.0), max(y, 0.0)
    ix2, iy2 = min(x2, float(width)), min(y2, float(height))
    inter = max(0.0, ix2 - ix1) * max(0.0, iy2 - iy1)
    if inter < 4 or inter / (w * h) < 0.25:
        return None
    x1 = int(np.floor(ix1))
    y1 = int(np.floor(iy1))
    x2i = int(np.ceil(ix2))
    y2i = int(np.ceil(iy2))
    if x2i - x1 < 2 or y2i - y1 < 2:
        return None
    out = dict(box)
    out.update({"x": x1, "y": y1, "w": x2i - x1, "h": y2i - y1})
    return out


def _as_probability(score: float) -> float:
    """DetectionOutput 分数应为 0–1。约 1–20 当成 logits，避免 3.0 被显示成 300%。"""
    if not np.isfinite(score) or score <= 0:
        return 0.0
    if score <= 1.0:
        return float(score)
    if score <= 20.0:
        return float(1.0 / (1.0 + np.exp(-float(score))))
    if score <= 100.0:
        return float(score) / 100.0
    return 0.0


def _detect_class_names(cfg: dict) -> list[str]:
    names = [str(n) for n in (cfg.get("class_names") or [])]
    if names and names[0].strip().lower() not in ("background", "bg", "__background__"):
        names = ["background"] + names
    if not names:
        names = ["background", "object"]
    return names


def _decode_corners(x0: float, y0: float, x1: float, y1: float, img_w: int, img_h: int, net: int) -> tuple[float, float, float, float] | None:
    vals = np.array([x0, y0, x1, y1], np.float64)
    if not np.isfinite(vals).all():
        return None
    if x1 < x0:
        x0, x1 = x1, x0
    if y1 < y0:
        y0, y1 = y1, y0
    span = max(abs(x0), abs(y0), abs(x1), abs(y1))
    if span <= 2.0:
        x0, x1 = x0 * img_w, x1 * img_w
        y0, y1 = y0 * img_h, y1 * img_h
    elif span <= net * 4:
        x0, x1 = x0 / net * img_w, x1 / net * img_w
        y0, y1 = y0 / net * img_h, y1 / net * img_h
    else:
        return None
    return x0, y0, x1, y1


def draw_boxes(rgb: np.ndarray, boxes: list[dict], out_path: Path) -> Path:
    from PIL import Image, ImageDraw

    arr = np.asarray(rgb)
    if arr.dtype != np.uint8:
        if arr.max() <= 1.5:
            arr = np.clip(arr * 255.0, 0, 255).astype(np.uint8)
        else:
            arr = np.clip(arr, 0, 255).astype(np.uint8)
    if arr.ndim == 2:
        image = Image.fromarray(arr, mode="L").convert("RGB")
    else:
        image = Image.fromarray(arr[..., :3] if arr.shape[-1] >= 3 else arr, mode="RGB")
    draw = ImageDraw.Draw(image)
    iw, ih = image.size
    for box in boxes:
        clipped = _clip_box(box, iw, ih)
        if clipped is None:
            continue
        x, y, bw, bh = clipped["x"], clipped["y"], clipped["w"], clipped["h"]
        try:
            draw.rectangle([x, y, x + bw, y + bh], outline=(0, 120, 212), width=2)
            label = f"{clipped.get('name', '')} {_as_probability(float(clipped.get('conf') or 0)):.0%}"
            ty2 = y
            ty1 = max(0, y - 14)
            if ty2 <= ty1:
                ty1, ty2 = y, min(ih, y + 14)
            tx2 = min(iw, x + max(12, 8 * len(label)))
            if tx2 > x and ty2 > ty1:
                draw.rectangle([x, ty1, tx2, ty2], fill=(0, 120, 212))
                draw.text((x + 2, ty1 + 1), label, fill=(255, 255, 255))
        except Exception:
            continue
    out_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(out_path)
    return out_path


def _hwc_from_cfg(cfg: dict) -> tuple[int, int, int]:
    shape = cfg.get("input_shape_nchw")
    if shape and len(shape) >= 3:
        c, h, w = int(shape[0]), int(shape[1]), int(shape[2])
        return h, w, c
    size = int(cfg.get("image_size") or cfg.get("input_size") or 28)
    channels = int(cfg.get("channels") or 3)
    return size, size, channels


def run_classify(image_path: Path, pack: Path) -> dict:
    sim, cfg = open_simulator(pack)
    if _is_retina_pack(cfg):
        print("该权重是 RetinaFace 检测模型，识别将按人脸检测运行。", flush=True)
        score = float(cfg.get("score") or cfg.get("score_th") or 0.5)
        return run_retina_detect(image_path, pack, sim, cfg, score)
    if _is_detect_pack(cfg):
        print("该权重是检测模型（SSDLite / post=detect），识别将按目标检测运行。", flush=True)
        score = float(cfg.get("score") or cfg.get("score_th") or 0.2)
        return run_ssd_detect(image_path, pack, sim, cfg, score)
    names = list(cfg.get("class_names") or [str(i) for i in range(int(cfg.get("num_classes") or 10))])
    rgb = load_user_image(image_path)
    result = classify_array(sim, rgb, names, cfg)
    result.update({
        "image": str(image_path),
        "pack": str(pack),
        "mode": "classify",
    })
    return result


def _ssd_rows(output: np.ndarray) -> np.ndarray:
    arr = np.asarray(output, np.float32)
    if arr.size == 0:
        return np.zeros((0, 6), dtype=np.float32)
    if arr.ndim == 1:
        if arr.size % 6 != 0:
            raise RuntimeError(f"检测输出长度 {arr.size} 不是 6 的倍数，不是 SSD detection_out")
        return arr.reshape(-1, 6)
    if arr.ndim == 2:
        if arr.shape[-1] == 6:
            return arr
        if arr.shape[0] == 6:
            return arr.T
    if arr.ndim == 3 and arr.shape[-1] == 6:
        return arr.reshape(-1, 6)
    raise RuntimeError(f"无法解析检测输出形状 {arr.shape}")


def _detect_thresh(cfg: dict, score_thresh: float) -> float:
    pack_th = cfg.get("score_th")
    if pack_th is None:
        pack_th = cfg.get("score")
    ssd = cfg.get("ssd_cfg") if isinstance(cfg.get("ssd_cfg"), dict) else {}
    if pack_th is None and ssd.get("confidence_threshold") is not None:
        pack_th = ssd["confidence_threshold"]
    requested = float(score_thresh) if score_thresh is not None else 0.2
    if pack_th is not None:
        return min(float(pack_th), requested)
    return min(requested, 0.25)


def _boxes_from_maps(sim, rgb: np.ndarray, pack: Path, cfg: dict, names: list[str],
                     thresh: float, img_w: int, img_h: int, max_keep: int) -> tuple[list[dict], float, float]:
    from ssd_decode import decode_pack_maps, detections_from_scores, load_ssd_cfg, map_blob_names

    ssd_cfg = load_ssd_cfg(pack, cfg)
    if not ssd_cfg:
        raise RuntimeError("缺少 ssd_cfg，无法在 Python 里解 SSD 框")
    if getattr(sim, "backend", "") != "ncnn" or not hasattr(sim, "extract_blobs"):
        raise RuntimeError("当前后端不能抽 loc/conf")
    blobs, elapsed_us = sim.extract_blobs(rgb, map_blob_names(ssd_cfg))
    boxes_xyxy, scores = decode_pack_maps(blobs, ssd_cfg)
    face_max = float(scores[:, 1:].max()) if scores.size and scores.shape[1] > 1 else 0.0
    print(
        f"Python SSD 解码 {len(boxes_xyxy)} 个锚框，人脸最大分数 {face_max:.3f}，阈值 {thresh:.2f}",
        flush=True,
    )
    raw = detections_from_scores(boxes_xyxy, scores, names, img_w, img_h, thresh, max_keep)
    boxes = []
    dropped = 0
    for box in raw:
        clipped = _clip_box(box, img_w, img_h)
        if clipped is None:
            dropped += 1
            continue
        boxes.append(clipped)
    if dropped:
        print(f"已丢弃 {dropped} 条越界框", flush=True)
    return boxes, float(elapsed_us), face_max


def run_ssd_detect(image_path: Path, pack: Path, sim, cfg: dict, score_thresh: float) -> dict:
    names = _detect_class_names(cfg)
    rgb = load_user_image(image_path)
    h, w = int(rgb.shape[0]), int(rgb.shape[1])
    net = int(cfg.get("image_size") or cfg.get("input_size") or min(h, w) or 300)
    thresh = _detect_thresh(cfg, score_thresh)
    iou_th = float(cfg.get("nms_iou_th") or (cfg.get("ssd_cfg") or {}).get("nms_threshold") or 0.45)
    max_keep = int(cfg.get("max_keep") or (cfg.get("ssd_cfg") or {}).get("keep_top_k") or 32)
    boxes: list[dict] = []
    elapsed_us = 0.0
    face_max = 0.0
    used_python = False
    try:
        boxes, elapsed_us, face_max = _boxes_from_maps(sim, rgb, pack, cfg, names, thresh, w, h, max_keep)
        used_python = True
    except Exception as exc:
        print(f"Python loc/conf 解码失败，回退 DetectionOutput: {exc}", flush=True)

    if not used_python:
        output, elapsed_us = _run_net(sim, rgb, cfg)
        rows = _ssd_rows(output)
        print(f"检测输出 {len(rows)} 条，阈值 {thresh:.2f}", flush=True)
        dropped = 0
        for row in rows:
            vals = [float(v) for v in row[:6]]
            if not np.all(np.isfinite(vals)):
                dropped += 1
                continue
            lab = int(vals[0] + 0.5)
            score = _as_probability(vals[1])
            if lab <= 0 or lab >= len(names) or score < thresh:
                continue
            corners = _decode_corners(vals[2], vals[3], vals[4], vals[5], w, h, net)
            if corners is None:
                dropped += 1
                continue
            x0, y0, x1, y1 = corners
            name = names[lab]
            clipped = _clip_box(
                {"x": x0, "y": y0, "w": x1 - x0, "h": y1 - y0, "class": lab, "name": name, "conf": score},
                w, h,
            )
            if clipped is None:
                dropped += 1
                continue
            boxes.append(clipped)
        if dropped:
            print(f"已丢弃 {dropped} 条非法框（坐标越界或分数无效）", flush=True)
        if not boxes and len(rows):
            finite = sum(1 for row in rows if np.all(np.isfinite(np.asarray(row[:6], dtype=np.float64))))
            if finite == 0:
                print("检测输出全是 NaN：多半是 INT8 把框回归打坏，请用 fp32 重新导出后再测", flush=True)

    boxes = nms(boxes, iou_thresh=iou_th)[:max_keep]
    if not boxes:
        hint = f"无人脸框。峰值分数 {face_max:.3f}（阈值 {thresh:.2f}）"
        if face_max < 0.05:
            hint += "。INT8 分类头会把分数压没，PC 应使用 fp32；板上 INT8 需重新导出并保留 conf 为 fp32"
        elif face_max < thresh:
            hint += "。模型测试准确率约 51%，可把阈值降到 0.15 再试"
        print(hint, flush=True)
    preview = write_preview(rgb, boxes)
    top = boxes[0] if boxes else {"class": -1, "name": "none", "conf": 0.0}
    return {
        "mode": "detect",
        "image": str(image_path),
        "pack": str(pack),
        "class": top.get("class", -1),
        "name": top.get("name", "none"),
        "conf": float(top.get("conf", 0)),
        "boxes": boxes,
        "preview": str(preview),
        "elapsed_us": float(elapsed_us),
    }


def run_retina_detect(image_path: Path, pack: Path, sim, cfg: dict, score_thresh: float) -> dict:
    from retina_decode import blob_names, decode_maps, letterbox_infer

    rgb = load_user_image(image_path)
    h, w = int(rgb.shape[0]), int(rgb.shape[1])
    net = int(cfg.get("image_size") or cfg.get("input_size") or 320)
    thresh = float(cfg.get("score_th") or cfg.get("score") or score_thresh or 0.5)
    if score_thresh is not None:
        thresh = min(thresh, float(score_thresh)) if float(score_thresh) > 0 else thresh
    iou_th = float(cfg.get("nms_iou_th") or (cfg.get("retina_cfg") or {}).get("nms_threshold") or 0.4)
    max_keep = int(cfg.get("max_keep") or (cfg.get("retina_cfg") or {}).get("keep_top_k") or 32)
    # 与板上一致：letterbox 到 input_size（默认 320），不要按官方 example 喂原图
    canvas, scale, pad_x, pad_y = letterbox_infer(rgb, net)
    print(
        f"RetinaFace PC 按板端模拟：letterbox {net}×{net}（scale={scale:.4f} pad={pad_x},{pad_y}）",
        flush=True,
    )
    if getattr(sim, "backend", "") != "ncnn" or not hasattr(sim, "extract_blobs"):
        raise RuntimeError("RetinaFace PC 推理需要 python ncnn 抽 9 个头")
    names = blob_names()
    try:
        blobs, elapsed_us = sim.extract_blobs(canvas, names)
    except Exception as exc:
        ids = [cfg.get(k) for k in (
            "blob_cls8", "blob_bbox8", "blob_lmk8",
            "blob_cls16", "blob_bbox16", "blob_lmk16",
            "blob_cls32", "blob_bbox32", "blob_lmk32",
        )]
        if not all(isinstance(v, int) and int(v) >= 0 for v in ids):
            raise RuntimeError(f"抽取 RetinaFace 头失败: {exc}") from exc
        print(f"按名称抽 blob 失败，改用 cfg 整数 ID: {exc}", flush=True)
        id_list = [int(v) for v in ids]
        raw_blobs, elapsed_us = sim.extract_blobs(canvas, id_list)
        blobs = {names[i]: raw_blobs[id_list[i]] for i in range(len(names))}
    decoded = decode_maps(blobs, net, thresh, w, h, scale, pad_x, pad_y, max_keep)
    if isinstance(decoded, tuple):
        raw, face_max = decoded
    else:
        raw, face_max = decoded, max((float(b["conf"]) for b in decoded), default=0.0)
    print(f"RetinaFace 解码 {len(raw)} 个候选，人脸最大分数 {face_max:.3f}，阈值 {thresh:.2f}", flush=True)
    boxes = []
    dropped = 0
    for box in raw:
        clipped = _clip_box(box, w, h)
        if clipped is None:
            dropped += 1
            continue
        boxes.append(clipped)
    if dropped:
        print(f"已丢弃 {dropped} 条越界框", flush=True)
    boxes = nms(boxes, iou_thresh=iou_th)[:max_keep]
    if not boxes:
        print(f"无人脸框。峰值分数 {face_max:.3f}（阈值 {thresh:.2f}）", flush=True)
    preview = write_preview(rgb, boxes)
    top = boxes[0] if boxes else {"class": -1, "name": "none", "conf": 0.0}
    return {
        "mode": "detect",
        "image": str(image_path),
        "pack": str(pack),
        "class": top.get("class", -1),
        "name": top.get("name", "none"),
        "conf": float(top.get("conf", 0)),
        "boxes": boxes,
        "preview": str(preview),
        "elapsed_us": float(elapsed_us),
    }


def run_detect(
    image_path: Path,
    pack: Path,
    stride: int | None = None,
    score_thresh: float = 0.2,
    max_size: int = 640,
) -> dict:
    sim, cfg = open_simulator(pack)
    if _is_retina_pack(cfg):
        return run_retina_detect(image_path, pack, sim, cfg, score_thresh)
    if str(cfg.get("post") or "").lower() == "detect":
        return run_ssd_detect(image_path, pack, sim, cfg, score_thresh)
    win_h, win_w, _channels = _hwc_from_cfg(cfg)
    names = list(cfg.get("class_names") or [str(i) for i in range(10)])
    rgb = load_user_image(image_path)
    h, w = rgb.shape[0], rgb.shape[1]
    scale = 1.0
    if max(h, w) > max_size:
        scale = max_size / float(max(h, w))
        from PIL import Image
        resized = Image.fromarray(rgb).resize((int(w * scale), int(h * scale)), Image.BILINEAR)
        rgb = np.asarray(resized, dtype=np.uint8)
        h, w = rgb.shape[0], rgb.shape[1]

    if h <= win_h and w <= win_w:
        result = run_classify(image_path, pack)
        result["boxes"] = [{
            "x": 0, "y": 0, "w": w, "h": h,
            "class": result["class"], "name": result["name"], "conf": result["conf"],
        }]
        result["mode"] = "detect"
        return result

    step = stride or max(8, win_w // 2)
    boxes = []
    for y in range(0, max(1, h - win_h + 1), step):
        for x in range(0, max(1, w - win_w + 1), step):
            crop = rgb[y:y + win_h, x:x + win_w]
            if crop.shape[0] != win_h or crop.shape[1] != win_w:
                continue
            pred = classify_array(sim, crop, names, cfg)
            if pred["conf"] < score_thresh:
                continue
            boxes.append({
                "x": int(x / scale),
                "y": int(y / scale),
                "w": int(win_w / scale),
                "h": int(win_h / scale),
                "class": pred["class"],
                "name": pred["name"],
                "conf": pred["conf"],
            })
    boxes = nms(boxes)
    orig = load_user_image(image_path)
    preview = write_preview(orig, boxes)
    top = boxes[0] if boxes else {"class": -1, "name": "none", "conf": 0.0}
    return {
        "mode": "detect",
        "image": str(image_path),
        "pack": str(pack),
        "class": top.get("class", -1),
        "name": top.get("name", "none"),
        "conf": float(top.get("conf", 0)),
        "boxes": boxes,
        "preview": str(preview),
        "windows": ((h - win_h) // step + 1) * ((w - win_w) // step + 1),
    }


def parse_args():
    parser = argparse.ArgumentParser(description="识别或滑窗检测一张图片")
    parser.add_argument("--image", required=True)
    parser.add_argument("--model", default=None, help="数据集名，用于查找最近的 ncnn 部署包")
    parser.add_argument("--pack", default=None)
    parser.add_argument("--detect", action="store_true")
    parser.add_argument("--stride", type=int, default=None)
    parser.add_argument("--score", type=float, default=0.2)
    return parser.parse_args()


def main():
    args = parse_args()
    image_path = Path(args.image)
    if not image_path.exists():
        raise FileNotFoundError(f"图片不存在: {image_path}")
    pack = resolve_pack(args.model, args.pack, prefer="detect" if args.detect else "classify")
    print(f"部署包: {pack}")
    print(f"图片: {image_path}")
    if args.detect:
        result = run_detect(image_path, pack, stride=args.stride, score_thresh=args.score)
    else:
        result = run_classify(image_path, pack)
    _print_result(result, image_path)


def _print_result(result: dict, image_path: Path) -> None:
    if result.get("mode") == "detect":
        print(f"[DETECT_DONE] n={len(result.get('boxes') or [])} top={result['name']} conf={result['conf']:.4f}", flush=True)
        for box in result.get("boxes") or []:
            print(
                f"[DETECT_BOX] x={box['x']} y={box['y']} w={box['w']} h={box['h']} "
                f"cls={box['class']} name={box['name']} conf={box['conf']:.4f}",
                flush=True,
            )
        preview = result.get("preview") or str(image_path)
        print(f"[DETECT_PREVIEW] {preview}", flush=True)
        return
    tops = ",".join(f"{item['name']}:{item['conf']:.3f}" for item in result.get("top") or [])
    print(
        f"[INFER_DONE] class={result['class']} name={result['name']} "
        f"conf={result['conf']:.4f} top={tops}",
        flush=True,
    )
    print(f"[INFER_PREVIEW] {image_path}", flush=True)


if __name__ == "__main__":
    main()
