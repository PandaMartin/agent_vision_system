"""简单 CNN：2 层卷积 + 池化，适合 FPGA / ncnn 部署。"""
from torch import nn

from .mobilenet import _NamedNet


def create_model(input_shape=(28, 28, 1), num_classes=10):
    cin = int(input_shape[-1])
    return _NamedNet([
        ("conv1", nn.Conv2d(cin, 8, 3, padding=1)),
        ("relu1", nn.ReLU(inplace=True)),
        ("pool1", nn.MaxPool2d(2)),
        ("conv2", nn.Conv2d(8, 16, 3, padding=1)),
        ("relu2", nn.ReLU(inplace=True)),
        ("pool2", nn.MaxPool2d(2)),
        ("flatten", nn.Flatten()),
        ("fc1", nn.Linear(16 * (int(input_shape[0]) // 4) * (int(input_shape[1]) // 4), 32)),
        ("fc1_relu", nn.ReLU(inplace=True)),
        ("drop", nn.Dropout(0.2)),
        ("fc2", nn.Linear(32, int(num_classes))),
    ], post="classify")
