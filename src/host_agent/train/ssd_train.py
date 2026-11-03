"""检测量化校准图。检测训练在 PyTorch 里完成。"""
from __future__ import annotations

import numpy as np


def calib_images_from_samples(samples: list, image_size: int, count: int, pipeline=None) -> np.ndarray:
    from detect_data import _read_image

    out = []
    for sample in samples[:count]:
        image, _, _ = _read_image(sample, image_size)
        if pipeline:
            from image_ops import apply_pipeline

            image = apply_pipeline(image, pipeline)
        if image.max() > 1.5:
            image = image / 255.0
        out.append(image.astype(np.float32))
        if len(out) >= count:
            break
    if not out:
        raise ValueError("没有可用于校准的检测图像")
    return np.stack(out, axis=0)
