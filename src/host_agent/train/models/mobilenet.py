"""MobileNetV2 分类骨干，对齐竞赛板 inverted residual。

算子：Conv / DWConv / BN / ReLU6 / Add，便于 ncnn INT8 上板。
小图输入时减少下采样，避免 28x28 被步长打穿。
"""
from __future__ import annotations

import torch
from torch import nn


def same_pad(kernel: int) -> int:
    return (int(kernel) - 1) // 2


class ConvBNAct(nn.Module):
    def __init__(self, cin, cout, kernel=3, stride=1, groups=1, act="relu6", bias=False):
        super().__init__()
        self.conv = nn.Conv2d(
            cin, cout, kernel, stride=stride, padding=same_pad(kernel), groups=groups, bias=bias,
        )
        self.bn = nn.BatchNorm2d(cout)
        if act == "relu6":
            self.act = nn.ReLU6(inplace=True)
        elif act == "relu":
            self.act = nn.ReLU(inplace=True)
        else:
            self.act = nn.Identity()

    def forward(self, x):
        return self.act(self.bn(self.conv(x)))


class InvertedResidual(nn.Module):
    def __init__(self, cin: int, cout: int, stride: int, expansion: int = 6):
        super().__init__()
        hidden = cin * expansion
        self.use_residual = stride == 1 and cin == cout
        self.expand = ConvBNAct(cin, hidden, kernel=1, act="relu6") if expansion != 1 else None
        self.dw = ConvBNAct(hidden, hidden, kernel=3, stride=stride, groups=hidden, act="relu6")
        self.proj = nn.Conv2d(hidden, cout, 1, padding=0, bias=False)
        self.proj_bn = nn.BatchNorm2d(cout)

    def forward(self, x):
        y = x if self.expand is None else self.expand(x)
        y = self.dw(y)
        y = self.proj_bn(self.proj(y))
        if self.use_residual:
            return x + y
        return y


def create_model(input_shape=(224, 224, 3), num_classes=10):
    height = int(input_shape[0])
    cin = int(input_shape[-1])
    blocks = [
        (1, 16, 1, 1),
        (6, 24, 2, 2),
        (6, 32, 3, 2),
        (6, 64, 4, 2),
        (6, 96, 3, 1),
        (6, 160, 3, 2),
        (6, 320, 1, 1),
    ]
    stem_stride = 2 if height >= 64 else 1
    spatial = height // stem_stride
    layers: list[tuple[str, nn.Module]] = [
        ("stem", ConvBNAct(cin, 32, 3, stride=stem_stride, act="relu6")),
    ]
    channels = 32
    idx = 0
    for expand, out_ch, repeats, stride in blocks:
        for i in range(repeats):
            s = stride if i == 0 else 1
            if s == 2 and spatial < 14:
                s = 1
            layers.append((f"ir{idx}", InvertedResidual(channels, out_ch, stride=s, expansion=expand)))
            channels = out_ch
            if s == 2:
                spatial = max(spatial // 2, 1)
            idx += 1
    head_ch = 1280 if height >= 64 else 256
    layers.append(("head", ConvBNAct(channels, head_ch, 1, act="relu6")))
    layers.append(("gap", nn.AdaptiveAvgPool2d(1)))
    layers.append(("flatten", nn.Flatten()))
    layers.append(("fc", nn.Linear(head_ch, int(num_classes))))
    return _NamedNet(layers, post="classify")


class _NamedNet(nn.Module):
    def __init__(self, items: list[tuple[str, nn.Module]], post: str = "classify"):
        super().__init__()
        self.post = post
        for name, module in items:
            self.add_module(name, module)

    def forward(self, x):
        for module in self.children():
            x = module(x)
        return x
