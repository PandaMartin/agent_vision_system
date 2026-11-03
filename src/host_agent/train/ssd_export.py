"""在 Keras 导出的 ncnn 图末尾接上 PriorBox / Flatten / DetectionOutput。"""
from __future__ import annotations

from pathlib import Path


def attach_ssd_detection(param_path: Path, ssd_cfg: dict) -> dict:
    path = Path(param_path)
    text = path.read_text(encoding="utf-8")
    lines = [ln.rstrip() for ln in text.splitlines() if ln.strip()]
    if len(lines) < 2:
        raise ValueError(f"ncnn param 无效: {path}")
    magic = lines[0]
    body = lines[2:]
    if any((ln.split() or [""])[0] == "DetectionOutput" for ln in body):
        return {
            "layers": len(body),
            "blobs": _count_blobs(body),
            "output_blob": "detection_out",
            "input_blob": "data",
        }
    loc_blobs, conf_blobs, feat_blobs = [], [], []
    maps = ssd_cfg.get("maps") or []
    for item in maps:
        loc_name = item.get("loc") or f"ssd_loc_{item.get('index', 0)}"
        conf_name = item.get("conf") or f"ssd_conf_{item.get('index', 0)}"
        loc_blob = _blob_of(body, loc_name)
        conf_blob = _blob_of(body, conf_name)
        feat_blob = _input_of(body, loc_name + "_dw") or _input_of(body, loc_name)
        if not loc_blob or not conf_blob or not feat_blob:
            raise ValueError(f"找不到 SSD 头 {loc_name}/{conf_name}，请确认保存的是 SSDLite 模型")
        loc_blobs.append(loc_blob)
        conf_blobs.append(conf_blob)
        feat_blobs.append(feat_blob)

    extra: list[str] = []
    flat_loc, flat_conf, priors = [], [], []
    img = int(ssd_cfg.get("image_size") or 300)
    aspects = [float(a) for a in (ssd_cfg.get("aspects") or [2.0])]
    variances = ssd_cfg.get("variances") or [0.1, 0.1, 0.2, 0.2]
    offset = float(ssd_cfg.get("offset") or 0.5)
    flip = int(ssd_cfg.get("flip") or 1)
    num_class = int(ssd_cfg.get("num_classes") or 1) + 1
    for i, item in enumerate(maps):
        loc_p = f"loc_perm_{i}"
        loc_f = f"loc_flat_{i}"
        conf_p = f"conf_perm_{i}"
        conf_f = f"conf_flat_{i}"
        prior = f"prior_{i}"
        extra.append(f"Permute {loc_p} 1 1 {loc_blobs[i]} {loc_p} 0=1")
        extra.append(f"Flatten {loc_f} 1 1 {loc_p} {loc_f}")
        extra.append(f"Permute {conf_p} 1 1 {conf_blobs[i]} {conf_p} 0=1")
        extra.append(f"Flatten {conf_f} 1 1 {conf_p} {conf_f}")
        extra.append(_prior_line(prior, feat_blobs[i], item, img, aspects, variances, offset, flip))
        flat_loc.append(loc_f)
        flat_conf.append(conf_f)
        priors.append(prior)
    n = len(maps)
    extra.append("Concat mbox_loc {} 1 {} mbox_loc 0=0".format(n, " ".join(flat_loc)))
    extra.append("Concat mbox_conf {} 1 {} mbox_conf 0=0".format(n, " ".join(flat_conf)))
    extra.append("Concat mbox_prior {} 1 {} mbox_prior 0=0".format(n, " ".join(priors)))
    # 板上 DetectionOutput 不再做 softmax。预设 VOC 图在 conf 后接 Reshape→Sigmoid→Flatten；
    # 训练是 Caffe 式 softmax CE，这里用 Softmax 把分数变成 0–1，再交给 DetectionOutput。
    nms_th = float(ssd_cfg.get("nms_threshold") or 0.45)
    nms_top_k = int(ssd_cfg.get("nms_top_k") or 200)
    keep_top_k = int(ssd_cfg.get("keep_top_k") or 32)
    conf_th = float(ssd_cfg.get("confidence_threshold") or 0.50)
    num_priors = int(ssd_cfg.get("num_priors") or 0)
    reshape = f"Reshape conf_reshape 1 1 mbox_conf conf_reshape 0={num_class}"
    if num_priors > 0:
        reshape += f" 1={num_priors}"
    extra.append(reshape)
    extra.append("Softmax conf_softmax 1 1 conf_reshape conf_softmax 0=0")
    extra.append("Flatten conf_prob 1 1 conf_softmax conf_prob")
    extra.append(
        "DetectionOutput detection_out 3 1 mbox_loc conf_prob mbox_prior detection_out "
        f"0={num_class} 1={nms_th:.6f} 2={nms_top_k} 3={keep_top_k} 4={conf_th:.6f}"
    )
    body.extend(extra)
    blob_count = _count_blobs(body)
    path.write_text(f"{magic}\n{len(body)} {blob_count}\n" + "\n".join(body) + "\n", encoding="utf-8")
    return {"layers": len(body), "blobs": blob_count, "output_blob": "detection_out", "input_blob": "data"}


def _prior_line(name, feat, item, img, aspects, variances, offset, flip) -> str:
    min_size = float(item["min_size"])
    max_size = float(item["max_size"])
    ar_txt = ",".join(f"{a:.6f}" for a in aspects)
    return (
        f"PriorBox {name} 2 1 {feat} data {name} "
        f"-23300=1,{min_size:.6f} -23301=1,{max_size:.6f} "
        f"-23302={len(aspects)},{ar_txt} "
        f"3={flip} 4=0 "
        f"5={float(variances[0]):.6f} 6={float(variances[1]):.6f} "
        f"7={float(variances[2]):.6f} 8={float(variances[3]):.6f} "
        f"9={img} 10={img} 11=-233 12={offset:.6f}"
    )


def _blob_of(lines: list[str], layer_name: str) -> str:
    for line in lines:
        parts = line.split()
        if len(parts) < 4:
            continue
        if parts[1] == layer_name:
            n_in, n_out = int(parts[2]), int(parts[3])
            blobs = parts[4 : 4 + n_in + n_out]
            if n_out >= 1:
                return blobs[n_in]
    return ""


def _input_of(lines: list[str], layer_name: str) -> str:
    for line in lines:
        parts = line.split()
        if len(parts) < 5:
            continue
        if parts[1] == layer_name:
            n_in = int(parts[2])
            blobs = parts[4 : 4 + n_in]
            return blobs[0] if blobs else ""
    return ""


def _count_blobs(lines: list[str]) -> int:
    names = set()
    for line in lines:
        parts = line.split()
        if len(parts) < 4:
            continue
        n_in, n_out = int(parts[2]), int(parts[3])
        names.update(parts[4 : 4 + n_in + n_out])
    return len(names)
