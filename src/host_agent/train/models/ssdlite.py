"""MobileNetV2 SSDLite：可训练检测头，导出时再接 ncnn PriorBox / DetectionOutput。

预处理对齐竞赛板 mobilenetv2_ssdlite_voc：BGR、(x-127.5)/127.5。
图含 Conv / DWConv / BN / ReLU6(导出为独立 Clip) / Add。输出各尺度 loc / conf（NCHW）。
"""
from __future__ import annotations

import torch
from torch import nn

from .mobilenet import ConvBNAct, InvertedResidual
from ssd_priors import concat_priors, layer_min_max

ASPECTS = [2.0]
NUM_MAPS = 6
ANCHORS = 4  # 正方形 min、sqrt(min*max)、ar=2、ar=0.5


class SSDLite(nn.Module):
    def __init__(self, cin: int, height: int, num_classes: int):
        super().__init__()
        self.post = "detect"
        self.num_classes = max(int(num_classes), 1)
        self.height = int(height)
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
        self.stem = ConvBNAct(cin, 32, 3, stride=stem_stride, act="relu6")
        self.blocks = nn.ModuleList()
        self.tap_after = None
        channels = 32
        idx = 0
        for expand, out_ch, repeats, stride in blocks:
            for i in range(repeats):
                s = stride if i == 0 else 1
                self.blocks.append(InvertedResidual(channels, out_ch, stride=s, expansion=expand))
                channels = out_ch
                idx += 1
            if out_ch == 96:
                self.tap_after = idx - 1
        head_ch = 1280 if height >= 64 else 256
        self.conv_head = ConvBNAct(channels, head_ch, 1, act="relu6")
        extras = [(256, 512), (128, 256), (128, 256), (64, 128)]
        self.extras = nn.ModuleList()
        cur_ch = head_ch
        for i, (hid, out_ch) in enumerate(extras):
            self.extras.append(_Extra(cur_ch, hid, out_ch))
            cur_ch = out_ch

        feat_channels = []
        # tap_96 通道 96；conv_head；再 4 个 extra
        feat_channels.append(96)
        feat_channels.append(head_ch)
        for _, out_ch in extras:
            feat_channels.append(out_ch)
        feat_channels = feat_channels[:NUM_MAPS]

        for i, fch in enumerate(feat_channels):
            loc_ch = ANCHORS * 4
            conf_ch = ANCHORS * (self.num_classes + 1)
            self.add_module(f"ssd_loc_{i}_dw", nn.Conv2d(fch, fch, 3, padding=1, groups=fch, bias=False))
            self.add_module(f"ssd_loc_{i}_bn", nn.BatchNorm2d(fch))
            self.add_module(f"ssd_loc_{i}_relu", nn.ReLU6(inplace=True))
            self.add_module(f"ssd_loc_{i}", nn.Conv2d(fch, loc_ch, 1, padding=0))
            self.add_module(f"ssd_conf_{i}_dw", nn.Conv2d(fch, fch, 3, padding=1, groups=fch, bias=False))
            self.add_module(f"ssd_conf_{i}_bn", nn.BatchNorm2d(fch))
            self.add_module(f"ssd_conf_{i}_relu", nn.ReLU6(inplace=True))
            self.add_module(f"ssd_conf_{i}", nn.Conv2d(fch, conf_ch, 1, padding=0))

        self.ssd_cfg = None
        self.priors = None

    def _head(self, feat, index: int, kind: str):
        dw = getattr(self, f"ssd_{kind}_{index}_dw")
        bn = getattr(self, f"ssd_{kind}_{index}_bn")
        relu = getattr(self, f"ssd_{kind}_{index}_relu")
        proj = getattr(self, f"ssd_{kind}_{index}")
        return proj(relu(bn(dw(feat))))

    def forward(self, x):
        x = self.stem(x)
        tap = None
        for i, block in enumerate(self.blocks):
            x = block(x)
            if self.tap_after is not None and i == self.tap_after:
                tap = x
        x = self.conv_head(x)
        feats = [tap if tap is not None else x, x]
        cur = x
        for extra in self.extras:
            cur = extra(cur)
            feats.append(cur)
        feats = feats[:NUM_MAPS]
        locs = []
        confs = []
        for i, feat in enumerate(feats):
            locs.append(self._head(feat, i, "loc"))
            confs.append(self._head(feat, i, "conf"))
        return tuple(locs + confs)


class _Extra(nn.Module):
    def __init__(self, cin: int, hidden: int, out_ch: int):
        super().__init__()
        self.pw = ConvBNAct(cin, hidden, 1, act="relu6")
        self.dw = ConvBNAct(hidden, hidden, 3, stride=2, groups=hidden, act="relu6")
        self.proj = ConvBNAct(hidden, out_ch, 1, act="relu6")

    def forward(self, x):
        return self.proj(self.dw(self.pw(x)))


def _attach_priors(model: SSDLite, height: int, width: int):
    model.eval()
    with torch.no_grad():
        cin = model.stem.conv.in_channels
        dummy = torch.zeros(1, int(cin), int(height), int(width))
        outs = model(dummy)
    n = len(outs) // 2
    mins, maxs = layer_min_max(height, n, min_scale=0.1, max_scale=0.9)
    maps_meta = []
    for i in range(n):
        loc = outs[i]
        h = int(loc.shape[2])
        w = int(loc.shape[3])
        maps_meta.append({
            "index": i,
            "h": h,
            "w": w,
            "min_size": mins[i],
            "max_size": maxs[i],
            "feat": f"ssd_feat_{i}",
            "loc": f"ssd_loc_{i}",
            "conf": f"ssd_conf_{i}",
        })
    priors = concat_priors(maps_meta, height, ASPECTS)
    model.ssd_cfg = {
        "image_size": height,
        "width": width,
        "num_classes": model.num_classes,
        "num_maps": n,
        "anchors": ANCHORS,
        "aspects": list(ASPECTS),
        "flip": 1,
        "offset": 0.5,
        "variances": [0.1, 0.1, 0.2, 0.2],
        "maps": maps_meta,
        "num_priors": int(priors.shape[0]),
        "nms_threshold": 0.45,
        "nms_top_k": 200,
        "keep_top_k": 32,
        "confidence_threshold": 0.50,
        "pixel": "bgr",
        "mean": [127.5, 127.5, 127.5],
        "norm": [1.0 / 127.5, 1.0 / 127.5, 1.0 / 127.5],
    }
    model.priors = priors
    model.train()
    return model


def create_model(input_shape=(300, 300, 3), num_classes=1):
    height = int(input_shape[0])
    width = int(input_shape[1])
    cin = int(input_shape[-1])
    model = SSDLite(cin, height, num_classes)
    return _attach_priors(model, height, width)
