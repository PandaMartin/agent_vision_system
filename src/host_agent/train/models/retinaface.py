"""RetinaFace MobileNet0.25：可训练人脸检测，导出对齐 ncnn mnet.25-opt。

输入 320×320 RGB 0–255（与 ncnn examples/retinaface.cpp 的 BGR2RGB、不减均值一致）。
输出 9 个特征图：stride 8/16/32 的 cls(4) / bbox(8) / landmark(20)。
cls 通道布局 [bg0, bg1, face0, face1]，与官方 decode 的 channel(q+num_anchors) 一致。
"""
from __future__ import annotations

import torch
from torch import nn
from torch.nn import functional as F

from .mobilenet import ConvBNAct


class ConvBN(nn.Module):
    def __init__(self, cin, cout, kernel=3, stride=1, groups=1):
        super().__init__()
        pad = (int(kernel) - 1) // 2
        self.conv = nn.Conv2d(cin, cout, kernel, stride=stride, padding=pad, groups=groups, bias=False)
        self.bn = nn.BatchNorm2d(cout)

    def forward(self, x):
        return self.bn(self.conv(x))


class SSH(nn.Module):
    def __init__(self, cin: int, cout: int = 64):
        super().__init__()
        self.conv3 = ConvBN(cin, cout // 2, 3)
        self.ctx1 = ConvBNAct(cin, cout // 4, 3, act="relu")
        self.ctx2 = ConvBN(cout // 4, cout // 4, 3)
        self.ctx3a = ConvBNAct(cout // 4, cout // 4, 3, act="relu")
        self.ctx3b = ConvBN(cout // 4, cout // 4, 3)

    def forward(self, x):
        a = self.conv3(x)
        t = self.ctx1(x)
        b = self.ctx2(t)
        c = self.ctx3b(self.ctx3a(t))
        return F.relu(torch.cat([a, b, c], dim=1))


class MobileNet025(nn.Module):
    """与 mnet.25 相近的 0.25 宽度骨干，抽 stride 8/16/32。"""

    def __init__(self, cin: int = 3):
        super().__init__()
        self.stem = ConvBNAct(cin, 8, 3, stride=2, act="relu")
        self.stage = nn.ModuleList([
            ConvBNAct(8, 8, 3, stride=1, groups=8, act="relu"),
            ConvBNAct(8, 16, 1, act="relu"),
            ConvBNAct(16, 16, 3, stride=2, groups=16, act="relu"),
            ConvBNAct(16, 32, 1, act="relu"),
            ConvBNAct(32, 32, 3, stride=1, groups=32, act="relu"),
            ConvBNAct(32, 32, 1, act="relu"),
            ConvBNAct(32, 32, 3, stride=2, groups=32, act="relu"),
            ConvBNAct(32, 64, 1, act="relu"),
            ConvBNAct(64, 64, 3, stride=1, groups=64, act="relu"),
            ConvBNAct(64, 64, 1, act="relu"),  # c1 stride 8
            ConvBNAct(64, 64, 3, stride=2, groups=64, act="relu"),
            ConvBNAct(64, 128, 1, act="relu"),
            ConvBNAct(128, 128, 3, stride=1, groups=128, act="relu"),
            ConvBNAct(128, 128, 1, act="relu"),
            ConvBNAct(128, 128, 3, stride=1, groups=128, act="relu"),
            ConvBNAct(128, 128, 1, act="relu"),
            ConvBNAct(128, 128, 3, stride=1, groups=128, act="relu"),
            ConvBNAct(128, 128, 1, act="relu"),
            ConvBNAct(128, 128, 3, stride=1, groups=128, act="relu"),
            ConvBNAct(128, 128, 1, act="relu"),
            ConvBNAct(128, 128, 3, stride=1, groups=128, act="relu"),
            ConvBNAct(128, 128, 1, act="relu"),  # c2 stride 16
            ConvBNAct(128, 128, 3, stride=2, groups=128, act="relu"),
            ConvBNAct(128, 256, 1, act="relu"),
            ConvBNAct(256, 256, 3, stride=1, groups=256, act="relu"),
            ConvBNAct(256, 256, 1, act="relu"),  # c3 stride 32
        ])
        self._c1 = 9
        self._c2 = 21
        self._c3 = 25

    def forward(self, x):
        x = self.stem(x)
        c1 = c2 = c3 = x
        for i, layer in enumerate(self.stage):
            x = layer(x)
            if i == self._c1:
                c1 = x
            elif i == self._c2:
                c2 = x
            elif i == self._c3:
                c3 = x
        return c1, c2, c3


class RetinaFace(nn.Module):
    def __init__(self, cin: int, image_size: int):
        super().__init__()
        self.post = "detect"
        self.detect_kind = "retinaface"
        self.image_size = int(image_size)
        self.num_classes = 1
        self.backbone = MobileNet025(cin)
        self.l1 = ConvBNAct(64, 64, 1, act="relu")
        self.l2 = ConvBNAct(128, 64, 1, act="relu")
        self.l3 = ConvBNAct(256, 64, 1, act="relu")
        self.a1 = ConvBNAct(64, 64, 3, act="relu")
        self.a2 = ConvBNAct(64, 64, 3, act="relu")
        self.ssh1 = SSH(64, 64)
        self.ssh2 = SSH(64, 64)
        self.ssh3 = SSH(64, 64)
        for stride in (8, 16, 32):
            self.add_module(f"face_rpn_cls_score_stride{stride}", nn.Conv2d(64, 4, 1))
            self.add_module(f"face_rpn_bbox_pred_stride{stride}", nn.Conv2d(64, 8, 1))
            self.add_module(f"face_rpn_landmark_pred_stride{stride}", nn.Conv2d(64, 20, 1))
        self.priors = None
        self.ssd_cfg = None
        self.retina_cfg = None

    def forward(self, x):
        c1, c2, c3 = self.backbone(x)
        p3 = self.l3(c3)
        p2 = self.a2(self.l2(c2) + F.interpolate(p3, scale_factor=2.0, mode="nearest"))
        p1 = self.a1(self.l1(c1) + F.interpolate(p2, scale_factor=2.0, mode="nearest"))
        f1, f2, f3 = self.ssh1(p1), self.ssh2(p2), self.ssh3(p3)
        outs = []
        for feat, stride in ((f1, 8), (f2, 16), (f3, 32)):
            outs.append(getattr(self, f"face_rpn_cls_score_stride{stride}")(feat))
            outs.append(getattr(self, f"face_rpn_bbox_pred_stride{stride}")(feat))
            outs.append(getattr(self, f"face_rpn_landmark_pred_stride{stride}")(feat))
        return tuple(outs)


def create_model(input_shape=(320, 320, 3), num_classes=1):
    from retina_priors import attach_retina_meta

    height = int(input_shape[0])
    cin = int(input_shape[-1])
    model = RetinaFace(cin, height)
    return attach_retina_meta(model, height)
