"""PyTorch 设备选择。Windows 上用 CUDA 调用本机 NVIDIA 显卡。"""
from __future__ import annotations

import os


def select_device():
    import torch

    if torch.cuda.is_available():
        torch.backends.cudnn.benchmark = True
        device = torch.device("cuda")
        name = torch.cuda.get_device_name(0)
        print(f"训练设备: GPU  {name}  ({torch.cuda.get_device_properties(0).total_memory / 1024 ** 3:.1f} GB)", flush=True)
        return device
    print("训练设备: CPU（未检测到 CUDA）", flush=True)
    return torch.device("cpu")


def seed_all(seed: int):
    import random

    import numpy as np
    import torch

    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
    os.environ["PYTHONHASHSEED"] = str(seed)


def count_params(model) -> int:
    return int(sum(p.numel() for p in model.parameters()))


def print_model(model, device) -> None:
    n = count_params(model)
    print(f"PyTorch 模型: {type(model).__name__}  参数 {n:,}  设备 {device}", flush=True)
