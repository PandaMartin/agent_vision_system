"""分类模型的量化感知训练。

卷积权重按输出通道做成对称 8bit，ReLU / ReLU6 之后的激活做成对称 8bit。
用 straight-through 把梯度传回浮点权重。不改网络结构，保存前会卸掉，
导出的仍是普通浮点图，再由 ncnn INT8 量化，并走识别率审阅。
"""
from __future__ import annotations

import torch
from torch import nn
from torch.nn import functional as F


def ste_fake_quant(x: torch.Tensor, axis: int | None = None) -> torch.Tensor:
    """对称 INT8 伪量化。反向把量化台阶当成直通。"""
    qmax = 127.0
    detached = x.detach()
    if axis is None:
        scale = detached.abs().amax().clamp(min=1e-8) / qmax
    else:
        dims = [d for d in range(detached.ndim) if d != int(axis)]
        scale = detached.abs().amax(dim=dims, keepdim=True).clamp(min=1e-8) / qmax
    q = torch.round(x / scale).clamp(-qmax, qmax)
    dequant = q * scale
    return x + (dequant - x).detach()


def _quant_conv(module: nn.Conv2d, x: torch.Tensor) -> torch.Tensor:
    weight = ste_fake_quant(module.weight, axis=0)
    return F.conv2d(
        x, weight, module.bias, module.stride, module.padding, module.dilation, module.groups,
    )


def _quant_linear(module: nn.Linear, x: torch.Tensor) -> torch.Tensor:
    weight = ste_fake_quant(module.weight, axis=0)
    return F.linear(x, weight, module.bias)


def enable_qat(model: nn.Module) -> list:
    """挂上伪量化。返回的句柄交给 disable_qat。"""
    restores: list = []
    convs = linears = acts = 0
    for module in model.modules():
        if isinstance(module, nn.Conv2d):
            original = module.forward
            module.forward = lambda x, mod=module: _quant_conv(mod, x)
            restores.append((module, original))
            convs += 1
        elif isinstance(module, nn.Linear):
            original = module.forward
            module.forward = lambda x, mod=module: _quant_linear(mod, x)
            restores.append((module, original))
            linears += 1
        elif isinstance(module, (nn.ReLU, nn.ReLU6)):
            restores.append(module.register_forward_hook(lambda _m, _i, out: ste_fake_quant(out)))
            acts += 1
    print(
        f"QAT：{convs} 个卷积、{linears} 个全连接的权重，以及 {acts} 个 ReLU 激活，按 INT8 伪量化训练",
        flush=True,
    )
    return restores


def disable_qat(restores: list) -> None:
    for item in restores:
        if isinstance(item, tuple):
            module, original = item
            module.forward = original
        else:
            item.remove()
    restores.clear()
