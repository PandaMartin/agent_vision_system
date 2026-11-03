"""把 PyTorch 分类/检测模型写成 ncnn fp32 .param / .bin。

用 FX 展开到 Conv / DWConv / BN / ReLU / Pool / GAP / Dense / Add / Concat，
并把 Conv+BN+ReLU 融合成一层，便于板上 INT8。
"""
from __future__ import annotations

import operator
import struct
from pathlib import Path

import numpy as np
import torch
from torch import nn


def convert_torch_to_ncnn(model: nn.Module, input_shape, param_path: Path, bin_path: Path) -> dict:
    """input_shape 为 (H, W, C) 或 (C, H, W)（C 为 1 或 3 时按 HWC 优先）。"""
    h, w, c = _as_hwc(input_shape)
    writer = _GraphWriter(model, (c, h, w))
    writer.build()
    param_path = Path(param_path)
    bin_path = Path(bin_path)
    param_path.parent.mkdir(parents=True, exist_ok=True)
    param_path.write_text(writer.param_text(), encoding="utf-8")
    bin_path.write_bytes(writer.bin_bytes())
    return {
        "layers": writer.layer_count,
        "blobs": writer.blob_count,
        "param": str(param_path),
        "bin": str(bin_path),
        "input": writer.input_shape,
        "output_blob": writer.output_blob,
        "input_blob": "data",
    }


class _LeafTracer(torch.fx.Tracer):
    LEAVES = (
        nn.Conv2d,
        nn.BatchNorm2d,
        nn.ReLU,
        nn.ReLU6,
        nn.Identity,
        nn.MaxPool2d,
        nn.AvgPool2d,
        nn.AdaptiveAvgPool2d,
        nn.Linear,
        nn.Dropout,
        nn.Flatten,
        nn.Softmax,
    )

    def is_leaf_module(self, m, module_qualified_name: str) -> bool:
        return isinstance(m, self.LEAVES)


class _GraphWriter:
    def __init__(self, model: nn.Module, input_nchw: tuple[int, int, int]):
        self.model = model.eval()
        self.lines: list[str] = []
        self.weights: list[bytes] = []
        self.shapes: dict[str, tuple[int, int, int]] = {}
        self.node_blob: dict = {}
        self._copies: dict = {}
        self._names: set[str] = set()
        self.output_blob = "prob"
        self.input_shape = input_nchw
        self._fused_skip: set = set()

    @property
    def layer_count(self) -> int:
        return len(self.lines)

    @property
    def blob_count(self) -> int:
        names = set()
        for line in self.lines:
            parts = line.split()
            if len(parts) < 4:
                continue
            n_in, n_out = int(parts[2]), int(parts[3])
            names.update(parts[4 : 4 + n_in + n_out])
        return len(names)

    def param_text(self) -> str:
        return f"7767517\n{self.layer_count} {self.blob_count}\n" + "\n".join(self.lines) + "\n"

    def bin_bytes(self) -> bytes:
        return b"".join(self.weights)

    def build(self):
        c, h, w = self.input_shape
        tracer = _LeafTracer()
        graph = tracer.trace(self.model)
        gm = torch.fx.GraphModule(self.model, graph)
        modules = dict(gm.named_modules())

        consumers: dict = {node: [] for node in graph.nodes}
        user_count: dict = {node: 0 for node in graph.nodes}
        for node in graph.nodes:
            for inp in node.all_input_nodes:
                consumers[inp].append(node)
                user_count[inp] += 1

        self.shapes["data"] = (c, h, w)
        self._emit(f"Input                    data                     0 1 data 0={w} 1={h} 2={c}")

        placeholder = next(n for n in graph.nodes if n.op == "placeholder")
        self._bind_node(placeholder, "data", user_count)

        for node in graph.nodes:
            if node in self._fused_skip:
                continue
            if node.op == "placeholder":
                continue
            if node.op == "output":
                args = node.args[0]
                if isinstance(args, (list, tuple)):
                    last = args[-1]
                else:
                    last = args
                self.output_blob = self.node_blob[last]
                post = str(getattr(self.model, "post", "classify") or getattr(self.model, "detect_kind", "")).lower()
                if post not in ("detect", "retinaface"):
                    if not self._is_softmax_blob(self.output_blob):
                        sm = self._unique("prob")
                        src = self._feed(self.output_blob)
                        self._emit(f"Softmax                   {sm:22s} 1 1 {src} {sm} 0=0")
                        self.shapes[sm] = self.shapes[src]
                        self.output_blob = sm
                continue
            if node.op == "get_attr":
                continue

            if node.op == "call_module":
                module = modules[node.target]
                if isinstance(module, (nn.Dropout, nn.Identity, nn.Flatten)):
                    src = self._take(node.all_input_nodes[0])
                    self._bind_node(node, src, user_count)
                    continue
                in_blobs = [self._take(n) for n in node.all_input_nodes]
                out_name = self._unique(_sanitize(str(node.target).replace(".", "_")))
                bn_node = relu_node = None
                if isinstance(module, nn.Conv2d):
                    bn_node, relu_node = self._find_bn_relu(node, consumers, modules)
                    if bn_node is not None:
                        self._fused_skip.add(bn_node)
                    if relu_node is not None:
                        self._fused_skip.add(relu_node)
                self._convert_module(
                    module, in_blobs, out_name,
                    bn=modules[bn_node.target] if bn_node else None,
                    relu=modules[relu_node.target] if relu_node else None,
                )
                self._bind_node(node, out_name, user_count)
                if bn_node is not None:
                    self._bind_node(bn_node, out_name, user_count)
                if relu_node is not None:
                    self._bind_node(relu_node, out_name, user_count)
                continue

            if node.op in ("call_function", "call_method"):
                self._convert_function(node, user_count)
                continue
            raise ValueError(f"不支持的 FX 节点: {node.op} {node.target}")

    def _find_bn_relu(self, conv_node, consumers, modules):
        """只融合 Conv+BN。ReLU6 保持独立 Clip，与板上 VOC SSDLite 预设一致。"""
        users = [n for n in consumers[conv_node] if n not in self._fused_skip]
        if len(users) != 1:
            return None, None
        nxt = users[0]
        if nxt.op == "call_module" and isinstance(modules.get(nxt.target), nn.BatchNorm2d):
            return nxt, None
        return None, None

    def _convert_module(self, module, in_blobs, out_name, bn=None, relu=None):
        src = in_blobs[0]
        if isinstance(module, nn.Conv2d):
            self._conv(module, src, out_name, bn=bn, relu=relu)
        elif isinstance(module, nn.BatchNorm2d):
            self._batchnorm(module, src, out_name)
        elif isinstance(module, nn.ReLU6):
            self._clip(src, out_name, 0.0, 6.0)
        elif isinstance(module, nn.ReLU):
            src = self._feed(src)
            cin, hin, win = self.shapes[src]
            self._emit(f"ReLU                      {out_name:22s} 1 1 {src} {out_name}")
            self.shapes[out_name] = (cin, hin, win)
        elif isinstance(module, nn.MaxPool2d):
            self._pool(module, src, out_name, 0)
        elif isinstance(module, nn.AvgPool2d):
            self._pool(module, src, out_name, 1)
        elif isinstance(module, nn.AdaptiveAvgPool2d):
            self._gap(src, out_name)
        elif isinstance(module, nn.Linear):
            self._dense(module, src, out_name)
        elif isinstance(module, nn.Softmax):
            src = self._feed(src)
            self._emit(f"Softmax                   {out_name:22s} 1 1 {src} {out_name} 0=0")
            self.shapes[out_name] = self.shapes[src]
        else:
            raise ValueError(f"不支持导出到 ncnn 的模块: {type(module).__name__}")

    def _convert_function(self, node, user_count):
        target = node.target
        inputs = node.all_input_nodes
        if target in (operator.add, torch.add) or (node.op == "call_method" and target == "add"):
            blobs = [self._take(n) for n in inputs[:2]]
            out_name = self._unique("add")
            self._add(blobs, out_name)
            self._bind_node(node, out_name, user_count)
            return
        if target in (torch.cat, torch.concat):
            args = node.args
            tensors = args[0]
            dim = 1
            if len(args) > 1:
                dim = int(args[1])
            dim = int(node.kwargs.get("dim", node.kwargs.get("axis", dim)))
            blobs = [self._take(n) for n in tensors]
            out_name = self._unique("concat")
            self._concat(blobs, out_name, dim)
            self._bind_node(node, out_name, user_count)
            return
        if target in (torch.flatten,) or (node.op == "call_method" and target in ("flatten", "reshape", "view", "contiguous", "permute")):
            src = self._take(inputs[0])
            self._bind_node(node, src, user_count)
            return
        if target in (operator.getitem,):
            src_node = inputs[0]
            src = self._take(src_node)
            self._bind_node(node, src, user_count)
            return
        name = getattr(target, "__name__", "")
        if name == "interpolate" or target is torch.nn.functional.interpolate:
            src = self._take(inputs[0])
            scale = node.kwargs.get("scale_factor", 2.0)
            if scale is None:
                scale = 2.0
            if isinstance(scale, (tuple, list)):
                sy, sx = float(scale[0]), float(scale[-1])
            else:
                sy = sx = float(scale)
            mode = str(node.kwargs.get("mode", "nearest"))
            resize_type = 1 if mode == "nearest" else 2
            out_name = self._unique("interp")
            src = self._feed(src)
            cin, hin, win = self.shapes[src]
            hout = max(1, int(round(hin * sy)))
            wout = max(1, int(round(win * sx)))
            # 必须写成科学计数法：python ncnn 把 1=2 当成 int，float 尺度读成 0，Interp 返回 -100
            self._emit(
                f"Interp                    {out_name:22s} 1 1 {src} {out_name} "
                f"0={resize_type} 1={sy:.6e} 2={sx:.6e} 3={hout} 4={wout}"
            )
            self.shapes[out_name] = (cin, hout, wout)
            self._bind_node(node, out_name, user_count)
            return
        if name == "relu" or target in (torch.relu, torch.nn.functional.relu):
            src = self._take(inputs[0])
            out_name = self._unique("relu")
            src = self._feed(src)
            self._emit(f"ReLU                      {out_name:22s} 1 1 {src} {out_name}")
            self.shapes[out_name] = self.shapes[src]
            self._bind_node(node, out_name, user_count)
            return
        raise ValueError(f"不支持导出到 ncnn 的算子: {target}")

    def _conv(self, module: nn.Conv2d, src: str, out_name: str, bn=None, relu=None):
        src = self._feed(src)
        cin, hin, win = self.shapes[src]
        kh, kw = _pair(module.kernel_size)
        sh, sw = _pair(module.stride)
        pad = _padding_mode(module)
        groups = int(module.groups)
        weight = module.weight.detach().cpu().numpy().astype(np.float32)
        num_output = int(module.out_channels)
        if module.bias is not None:
            bias = module.bias.detach().cpu().numpy().astype(np.float32)
            use_bias = True
        else:
            bias = np.zeros((num_output,), np.float32)
            use_bias = False
        depthwise = groups == cin and groups > 1
        if depthwise:
            # (cout, 1, kh, kw) -> (cout, kh, kw)
            w = weight.reshape(num_output, kh, kw)
            wsize = int(num_output * kh * kw)
        else:
            w = weight
            wsize = int(np.prod(w.shape))
        if bn is not None:
            w, bias = _fold_bn(w, bias, bn, num_output)
            use_bias = True
        ph, pw = _pair(module.padding)
        if pad == "same" and ph == 0 and pw == 0:
            pad_arg = "4=-233"
        else:
            pad_arg = f"4={int(pw)} 14={int(pw)} 15={int(ph)} 16={int(ph)}"
        act_type, act_params = 0, ""
        if relu is not None:
            if isinstance(relu, nn.ReLU6):
                act_type, act_params = 3, _clip_act_params(0.0, 6.0)
            else:
                act_type, act_params = 1, ""
        extra = f" {act_params}" if act_params else ""
        if depthwise:
            line = (
                f"ConvolutionDepthWise      {out_name:22s} 1 1 {src} {out_name} "
                f"0={num_output} 1={kw} 11={kh} 3={sw} 13={sh} {pad_arg} "
                f"5={1 if use_bias else 0} 6={wsize} 7={groups} 9={act_type}{extra}"
            )
        else:
            line = (
                f"Convolution               {out_name:22s} 1 1 {src} {out_name} "
                f"0={num_output} 1={kw} 11={kh} 3={sw} 13={sh} {pad_arg} "
                f"5={1 if use_bias else 0} 6={wsize} 9={act_type}{extra}"
            )
        self._emit(line)
        self._write_weight_tagged(w.reshape(-1))
        if use_bias:
            self._write_bias(np.asarray(bias, np.float32).reshape(-1))
        hout, wout = _conv_out(hin, win, kh, kw, sh, sw, pad)
        self.shapes[out_name] = (num_output, hout, wout)

    def _pool(self, module, src: str, out_name: str, pool_type: int):
        src = self._feed(src)
        cin, hin, win = self.shapes[src]
        kh, kw = _pair(module.kernel_size)
        sh, sw = _pair(module.stride)
        pad = "same" if int(_pair(module.padding)[0]) > 0 or str(getattr(module, "padding", "")) == "same" else "valid"
        # ceil_mode / padding=1 on 3x3 s2 ≈ Keras same
        if int(_pair(module.padding)[0]) > 0:
            pad = "same"
        pad_arg = "3=-233" if pad == "same" else "3=0"
        self._emit(
            f"Pooling                   {out_name:22s} 1 1 {src} {out_name} "
            f"0={pool_type} 1={kw} 11={kh} 2={sw} 12={sh} {pad_arg}"
        )
        hout, wout = _conv_out(hin, win, kh, kw, sh, sw, pad)
        self.shapes[out_name] = (cin, hout, wout)

    def _gap(self, src: str, out_name: str):
        src = self._feed(src)
        cin, _, _ = self.shapes[src]
        self._emit(
            f"Pooling                   {out_name:22s} 1 1 {src} {out_name} "
            f"0=1 4=1"
        )
        self.shapes[out_name] = (cin, 1, 1)

    def _dense(self, module: nn.Linear, src: str, out_name: str):
        src = self._feed(src)
        units = int(module.out_features)
        use_bias = module.bias is not None
        k = module.weight.detach().cpu().numpy().astype(np.float32)
        bias = (
            module.bias.detach().cpu().numpy().astype(np.float32)
            if use_bias
            else np.zeros((units,), np.float32)
        )
        wsize = int(k.size)
        self._emit(
            f"InnerProduct              {out_name:22s} 1 1 {src} {out_name} "
            f"0={units} 1={1 if use_bias else 0} 2={wsize} 9=0"
        )
        self._write_weight_tagged(k.reshape(-1))
        if use_bias:
            self._write_bias(bias.reshape(-1))
        self.shapes[out_name] = (units, 1, 1)

    def _batchnorm(self, module: nn.BatchNorm2d, src: str, out_name: str):
        src = self._feed(src)
        cin, hin, win = self.shapes[src]
        gamma = module.weight.detach().cpu().numpy().astype(np.float32).reshape(-1)
        beta = module.bias.detach().cpu().numpy().astype(np.float32).reshape(-1)
        mean = module.running_mean.detach().cpu().numpy().astype(np.float32).reshape(-1)
        var = module.running_var.detach().cpu().numpy().astype(np.float32).reshape(-1)
        eps = float(module.eps)
        self._emit(
            f"BatchNorm                 {out_name:22s} 1 1 {src} {out_name} "
            f"0={cin} 1={eps}"
        )
        for arr in (gamma, mean, var, beta):
            self._write_bias(arr)
        self.shapes[out_name] = (cin, hin, win)

    def _clip(self, src: str, out_name: str, lo: float, hi: float):
        src = self._feed(src)
        cin, hin, win = self.shapes[src]
        self._emit(
            f"Clip                      {out_name:22s} 1 1 {src} {out_name} "
            f"0={float(lo):.6f} 1={float(hi):.6f}"
        )
        self.shapes[out_name] = (cin, hin, win)

    def _add(self, in_blobs: list[str], out_name: str):
        a = self._feed(in_blobs[0])
        b = self._feed(in_blobs[1])
        self._emit(f"Eltwise {out_name} 2 1 {a} {b} {out_name} 0=1")
        self.shapes[out_name] = self.shapes[a]

    def _concat(self, blobs: list[str], out_name: str, dim: int):
        blobs = [self._feed(b) for b in blobs]
        # PyTorch NCHW: dim=1 通道 → ncnn 轴 0
        ncnn_axis = 0 if dim in (1, -3) else max(int(dim) - 1, 0)
        n = len(blobs)
        self._emit(
            f"Concat {out_name} {n} 1 " + " ".join(blobs) + f" {out_name} 0={ncnn_axis}"
        )
        ch = sum(self.shapes[b][0] for b in blobs)
        _, h, w = self.shapes[blobs[0]]
        self.shapes[out_name] = (ch, h, w)

    def _bind_node(self, node, blob: str, user_count):
        if node in self.node_blob or node in self._copies:
            return
        n = int(user_count.get(node, 0) or 0)
        if n <= 1:
            self.node_blob[node] = blob
            return
        copies = [self._unique(f"{blob}_c{i}") for i in range(n)]
        self._emit(f"Split {self._unique('split')} 1 {n} {blob} " + " ".join(copies))
        for copy in copies:
            self.shapes[copy] = self.shapes[blob]
        self._copies[node] = copies
        self.node_blob[node] = copies[0]

    def _take(self, node) -> str:
        if node in self._copies and self._copies[node]:
            return self._copies[node].pop(0)
        blob = self.node_blob.get(node)
        if blob is None:
            raise ValueError(f"找不到节点对应 blob: {node}")
        return blob

    def _feed(self, blob: str) -> str:
        return blob

    def _emit(self, line: str):
        self.lines.append(" ".join(line.split()))

    def _unique(self, name: str) -> str:
        base = _sanitize(name)
        cand = base
        i = 1
        while cand in self._names:
            cand = f"{base}_{i}"
            i += 1
        self._names.add(cand)
        return cand

    def _write_weight_tagged(self, arr: np.ndarray):
        data = np.asarray(arr, dtype="<f4").reshape(-1)
        self.weights.append(struct.pack("<I", 0) + data.tobytes())

    def _write_bias(self, arr: np.ndarray):
        data = np.asarray(arr, dtype="<f4").reshape(-1)
        self.weights.append(data.tobytes())

    def _is_softmax_blob(self, blob: str) -> bool:
        for line in self.lines:
            parts = line.split()
            if len(parts) >= 2 and parts[0] == "Softmax" and parts[1] == blob:
                return True
        return False


def _as_hwc(shape) -> tuple[int, int, int]:
    vals = [int(x) for x in shape]
    if len(vals) == 4:
        vals = vals[1:]
    if len(vals) != 3:
        raise ValueError(f"input_shape 应为 HWC 或 CHW: {shape}")
    a, b, c = vals
    if c in (1, 3) and a >= 8 and b >= 8:
        return a, b, c
    if a in (1, 3) and b >= 8 and c >= 8:
        return b, c, a
    return a, b, c


def _pair(value) -> tuple[int, int]:
    if isinstance(value, int):
        return value, value
    return int(value[0]), int(value[1])


def _padding_mode(module: nn.Conv2d) -> str:
    if str(module.padding_mode) == "same":
        return "same"
    pad = module.padding
    if pad == "same":
        return "same"
    if isinstance(pad, str):
        return pad
    ph, pw = _pair(pad)
    if ph > 0 or pw > 0:
        return "same"
    return "valid"


def _fold_bn(weight: np.ndarray, bias: np.ndarray, bn: nn.BatchNorm2d, num_output: int):
    gamma = bn.weight.detach().cpu().numpy().astype(np.float32).reshape(-1)
    beta = bn.bias.detach().cpu().numpy().astype(np.float32).reshape(-1)
    mean = bn.running_mean.detach().cpu().numpy().astype(np.float32).reshape(-1)
    var = bn.running_var.detach().cpu().numpy().astype(np.float32).reshape(-1)
    eps = float(bn.eps)
    scale = gamma / np.sqrt(var + eps)
    w = np.asarray(weight, np.float32).copy()
    b = np.asarray(bias, np.float32).reshape(-1).copy()
    for oc in range(int(num_output)):
        w[oc] *= scale[oc]
        b[oc] = scale[oc] * (b[oc] - mean[oc]) + beta[oc]
    return w, b


def _clip_act_params(lo: float, hi: float) -> str:
    # 必须带小数点。python ncnn 把 -23310=2,0,6 里的整数 0,6 解析坏，整层输出变成 0。
    return f"-23310=2,{float(lo):.6f},{float(hi):.6f}"


def _conv_out(h, w, kh, kw, sh, sw, pad: str) -> tuple[int, int]:
    if pad == "same":
        return int(np.ceil(h / sh)), int(np.ceil(w / sw))
    return (h - kh) // sh + 1, (w - kw) // sw + 1


def _sanitize(name: str) -> str:
    text = "".join(ch if ch.isalnum() or ch in "._" else "_" for ch in name)
    return text.strip("_") or "layer"
