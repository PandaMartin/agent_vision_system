"""轻量 ResNet-18 结构（BasicBlock），小图输入时自动降低 stem 步长。"""
from torch import nn

from .mobilenet import ConvBNAct


class BasicBlock(nn.Module):
    def __init__(self, cin: int, filters: int, stride: int = 1):
        super().__init__()
        self.conv1 = nn.Conv2d(cin, filters, 3, stride=stride, padding=1, bias=False)
        self.bn1 = nn.BatchNorm2d(filters)
        self.relu1 = nn.ReLU(inplace=True)
        self.conv2 = nn.Conv2d(filters, filters, 3, padding=1, bias=False)
        self.bn2 = nn.BatchNorm2d(filters)
        self.down = None
        if stride != 1 or cin != filters:
            self.down = nn.Sequential(
                nn.Conv2d(cin, filters, 1, stride=stride, padding=0, bias=False),
                nn.BatchNorm2d(filters),
            )
        self.relu_out = nn.ReLU(inplace=True)

    def forward(self, x):
        y = self.relu1(self.bn1(self.conv1(x)))
        y = self.bn2(self.conv2(y))
        shortcut = x if self.down is None else self.down(x)
        return self.relu_out(shortcut + y)


class ResNet18Lite(nn.Module):
    def __init__(self, cin: int, height: int, num_classes: int):
        super().__init__()
        self.post = "classify"
        stem_stride = 2 if height >= 64 else 1
        stage_stride = 2 if height >= 32 else 1
        last_stride = stage_stride if height >= 64 else 1
        self.stem = ConvBNAct(cin, 32, 3, stride=stem_stride, act="relu")
        self.layer1 = BasicBlock(32, 32, 1)
        self.layer2 = BasicBlock(32, 32, 1)
        self.layer3 = BasicBlock(32, 64, stage_stride)
        self.layer4 = BasicBlock(64, 64, 1)
        self.layer5 = BasicBlock(64, 128, stage_stride)
        self.layer6 = BasicBlock(128, 128, 1)
        self.layer7 = BasicBlock(128, 256, last_stride)
        self.layer8 = BasicBlock(256, 256, 1)
        self.gap = nn.AdaptiveAvgPool2d(1)
        self.flatten = nn.Flatten()
        self.fc = nn.Linear(256, int(num_classes))

    def forward(self, x):
        x = self.stem(x)
        x = self.layer1(x)
        x = self.layer2(x)
        x = self.layer3(x)
        x = self.layer4(x)
        x = self.layer5(x)
        x = self.layer6(x)
        x = self.layer7(x)
        x = self.layer8(x)
        x = self.flatten(self.gap(x))
        return self.fc(x)


def create_model(input_shape=(224, 224, 3), num_classes=10):
    return ResNet18Lite(int(input_shape[-1]), int(input_shape[0]), int(num_classes))
