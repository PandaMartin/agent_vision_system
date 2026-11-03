"""SqueezeNet 1.1（Fire 模块），对齐竞赛板 squeezenet_int8。"""
import torch
from torch import nn


class Fire(nn.Module):
    def __init__(self, cin: int, squeeze: int, expand: int):
        super().__init__()
        self.squeeze = nn.Conv2d(cin, squeeze, 1, padding=0)
        self.squeeze_relu = nn.ReLU(inplace=True)
        self.expand1x1 = nn.Conv2d(squeeze, expand, 1, padding=0)
        self.expand1x1_relu = nn.ReLU(inplace=True)
        self.expand3x3 = nn.Conv2d(squeeze, expand, 3, padding=1)
        self.expand3x3_relu = nn.ReLU(inplace=True)

    def forward(self, x):
        x = self.squeeze_relu(self.squeeze(x))
        a = self.expand1x1_relu(self.expand1x1(x))
        b = self.expand3x3_relu(self.expand3x3(x))
        return torch.cat([a, b], dim=1)


class SqueezeNet(nn.Module):
    def __init__(self, cin: int, height: int, num_classes: int):
        super().__init__()
        self.post = "classify"
        stem_s = 2 if height >= 64 else 1
        pool_s = 2 if height >= 32 else 1
        last_pool = 2 if height >= 64 else 1
        self.conv1 = nn.Conv2d(cin, 64, 3, stride=stem_s, padding=1)
        self.relu1 = nn.ReLU(inplace=True)
        self.pool1 = nn.MaxPool2d(3, stride=pool_s, padding=1)
        self.fire2 = Fire(64, 16, 64)
        self.fire3 = Fire(128, 16, 64)
        self.pool3 = nn.MaxPool2d(3, stride=pool_s, padding=1)
        self.fire4 = Fire(128, 32, 128)
        self.fire5 = Fire(256, 32, 128)
        self.pool5 = nn.MaxPool2d(3, stride=last_pool, padding=1)
        self.fire6 = Fire(256, 48, 192)
        self.fire7 = Fire(384, 48, 192)
        self.fire8 = Fire(384, 64, 256)
        self.fire9 = Fire(512, 64, 256)
        self.drop9 = nn.Dropout(0.5)
        self.conv10 = nn.Conv2d(512, int(num_classes), 1, padding=0)
        self.relu10 = nn.ReLU(inplace=True)
        self.gap = nn.AdaptiveAvgPool2d(1)
        self.flatten = nn.Flatten()

    def forward(self, x):
        x = self.relu1(self.conv1(x))
        x = self.pool1(x)
        x = self.fire2(x)
        x = self.fire3(x)
        x = self.pool3(x)
        x = self.fire4(x)
        x = self.fire5(x)
        x = self.pool5(x)
        x = self.fire6(x)
        x = self.fire7(x)
        x = self.fire8(x)
        x = self.fire9(x)
        x = self.drop9(x)
        x = self.relu10(self.conv10(x))
        return self.flatten(self.gap(x))


def create_model(input_shape=(227, 227, 3), num_classes=10):
    return SqueezeNet(int(input_shape[-1]), int(input_shape[0]), int(num_classes))
