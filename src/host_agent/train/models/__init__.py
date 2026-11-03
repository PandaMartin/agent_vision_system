"""可选模型架构（PyTorch）。"""
from __future__ import annotations

import importlib.util
from pathlib import Path

from . import deep_cnn, mobilenet, resnet18, retinaface, simple_cnn, squeezenet, ssdlite

ARCH_BUILDERS = {
    "squeezenet": squeezenet.create_model,
    "mobilenet": mobilenet.create_model,
    "mobilenetv2": mobilenet.create_model,
    "ssdlite": ssdlite.create_model,
    "mobilenet_ssdlite": ssdlite.create_model,
    "retinaface": retinaface.create_model,
    "retinaface_mnet": retinaface.create_model,
    "simple_cnn": simple_cnn.create_model,
    "deep_cnn": deep_cnn.create_model,
    "resnet18": resnet18.create_model,
}


def resolve_train_arch(arch: str, post: str = "classify") -> str:
    arch = (arch or "simple_cnn").lower()
    if str(post or "").lower() in ("detect", "retinaface"):
        if arch in ("retinaface", "retinaface_mnet", "retina", "mnet25"):
            return "retinaface"
        if arch in ("mobilenet", "mobilenetv2", "ssdlite", "mobilenet_ssdlite"):
            return "ssdlite"
        raise ValueError("检测训练请选择 MobileNetV2 SSDLite 或 RetinaFace")
    return arch


def create_model(arch: str, input_shape, num_classes: int, arch_file: str | None = None, post: str = "classify"):
    if arch_file:
        return _load_from_file(arch_file, input_shape, num_classes)
    arch = resolve_train_arch(arch, post)
    if arch not in ARCH_BUILDERS:
        raise ValueError(f"未知架构: {arch}，可选: {', '.join(ARCH_BUILDERS)}")
    return ARCH_BUILDERS[arch](input_shape=input_shape, num_classes=num_classes)


def rebuild_from_checkpoint(ckpt: dict, arch_file: str | None = None):
    import torch

    arch = str(ckpt.get("arch") or "simple_cnn")
    post = str(ckpt.get("post") or "classify")
    input_shape = tuple(ckpt.get("input_shape") or (28, 28, 1))
    num_classes = int(ckpt.get("num_classes") or 10)
    model = create_model(arch, input_shape, num_classes, arch_file=arch_file or ckpt.get("arch_file"), post=post)
    model.load_state_dict(ckpt["state_dict"])
    model.eval()
    return model


def load_checkpoint(path, map_location="cpu") -> dict:
    import torch

    return torch.load(path, map_location=map_location, weights_only=False)


def _load_from_file(path: str, input_shape, num_classes: int):
    file_path = Path(path)
    if not file_path.exists():
        raise FileNotFoundError(f"架构文件不存在: {file_path}")
    text = file_path.read_text(encoding="utf-8", errors="ignore")
    if "tensorflow" in text or "keras" in text.lower():
        raise ValueError(
            f"{file_path.name} 仍是 Keras 架构文件。请在开始页重新保存自定义模型（训练已改为 PyTorch）。"
        )
    spec = importlib.util.spec_from_file_location("custom_arch", file_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"无法加载架构文件: {file_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not hasattr(module, "create_model"):
        raise AttributeError(f"{file_path} 中没有 create_model()")
    model = module.create_model(input_shape=input_shape, num_classes=num_classes)
    if not hasattr(model, "post"):
        model.post = "classify"
    return model
