"""在导出图的 cls logits 后接 Reshape→Softmax→Reshape，对齐 mnet.25-opt。"""
from __future__ import annotations

from pathlib import Path


def attach_retina_softmax(param_path: Path, retina_cfg: dict | None = None) -> dict:
    path = Path(param_path)
    text = path.read_text(encoding="utf-8")
    lines = [ln.rstrip() for ln in text.splitlines() if ln.strip()]
    if len(lines) < 2:
        raise ValueError(f"ncnn param 无效: {path}")
    body = lines[2:]
    if any("face_rpn_cls_prob_reshape_stride" in ln for ln in body):
        return {
            "layers": len(body),
            "blobs": _count_blobs(body),
            "output_blob": "face_rpn_cls_prob_reshape_stride8",
        }

    scores = list((retina_cfg or {}).get("score_blobs") or [
        "face_rpn_cls_score_stride8",
        "face_rpn_cls_score_stride16",
        "face_rpn_cls_score_stride32",
    ])
    extra = []
    for name in scores:
        src = _output_of(body, name)
        if not src:
            raise ValueError(f"找不到 RetinaFace 分类头 {name}，请确认训练的是 retinaface 架构")
        stride = name.rsplit("stride", 1)[-1]
        rs = f"face_rpn_cls_score_reshape_stride{stride}"
        sm = f"face_rpn_cls_prob_stride{stride}"
        pr = f"face_rpn_cls_prob_reshape_stride{stride}"
        extra.append(f"Reshape {rs} 1 1 {src} {rs} 0=0 1=-1 2=2")
        extra.append(f"Softmax {sm} 1 1 {rs} {sm} 0=0")
        extra.append(f"Reshape {pr} 1 1 {sm} {pr} 0=0 1=-1 2=4")
    body.extend(extra)
    blob_count = _count_blobs(body)
    path.write_text(f"{lines[0]}\n{len(body)} {blob_count}\n" + "\n".join(body) + "\n", encoding="utf-8")
    return {
        "layers": len(body),
        "blobs": blob_count,
        "output_blob": extra[-1].split()[1] if extra else "data",
    }


def _output_of(body: list[str], layer_name: str) -> str | None:
    for ln in body:
        parts = ln.split()
        if len(parts) < 5:
            continue
        if parts[1] == layer_name:
            n_in, n_out = int(parts[2]), int(parts[3])
            outs = parts[4 + n_in: 4 + n_in + n_out]
            return outs[0] if outs else None
    # FX 可能给层名加了后缀，用包含匹配
    for ln in body:
        parts = ln.split()
        if len(parts) < 5:
            continue
        if layer_name in parts[1]:
            n_in, n_out = int(parts[2]), int(parts[3])
            outs = parts[4 + n_in: 4 + n_in + n_out]
            return outs[0] if outs else None
    return None


def _count_blobs(body: list[str]) -> int:
    names = set()
    for ln in body:
        parts = ln.split()
        if len(parts) < 4:
            continue
        n_in, n_out = int(parts[2]), int(parts[3])
        names.update(parts[4: 4 + n_in + n_out])
    return len(names)
