"""深度 CNN：4 层卷积，准确率更高，训练更慢。"""
from torch import nn

from .mobilenet import _NamedNet


def create_model(input_shape=(32, 32, 3), num_classes=10):
    cin = int(input_shape[-1])
    return _NamedNet([
        ("conv1", nn.Conv2d(cin, 16, 3, padding=1)),
        ("relu1", nn.ReLU(inplace=True)),
        ("conv2", nn.Conv2d(16, 16, 3, padding=1)),
        ("relu2", nn.ReLU(inplace=True)),
        ("pool1", nn.MaxPool2d(2)),
        ("conv3", nn.Conv2d(16, 32, 3, padding=1)),
        ("relu3", nn.ReLU(inplace=True)),
        ("conv4", nn.Conv2d(32, 32, 3, padding=1)),
        ("relu4", nn.ReLU(inplace=True)),
        ("pool2", nn.MaxPool2d(2)),
        ("gap", nn.AdaptiveAvgPool2d(1)),
        ("flatten", nn.Flatten()),
        ("fc1", nn.Linear(32, 64)),
        ("fc1_relu", nn.ReLU(inplace=True)),
        ("drop", nn.Dropout(0.3)),
        ("fc2", nn.Linear(64, int(num_classes))),
    ], post="classify")
