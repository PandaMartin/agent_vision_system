"""加载 ncnn .param/.bin 做 PC 推理。优先官方 python 包，否则走内置 fp32 解释器。"""
from __future__ import annotations

from pathlib import Path

import numpy as np


def load_pack_param_bin(pack_dir: Path) -> tuple[Path, Path, dict]:
    from board_model import LABELS_NAME, WEIGHT_NAME, is_board_pack, load_labels, parse_model_cfg
    from common import read_json

    pack_dir = Path(pack_dir)
    cfg = {}
    cfg_path = pack_dir / "deployment_config.json"
    if cfg_path.exists():
        cfg = read_json(cfg_path)

    if is_board_pack(pack_dir):
        board_cfg = parse_model_cfg(pack_dir / "model.cfg")
        board_cfg.update({k: v for k, v in cfg.items() if v is not None})
        labels = load_labels(pack_dir / LABELS_NAME)
        if labels:
            board_cfg["class_names"] = labels
            board_cfg["num_classes"] = len(labels)
        weight = pack_dir / WEIGHT_NAME
        text_param = pack_dir / "graph.param"
        if not text_param.exists():
            candidates = sorted(p for p in pack_dir.glob("*.param") if p.name != "model.param.bin")
            text_param = candidates[0] if candidates else pack_dir / "model.param.bin"
        if not text_param.exists() and (pack_dir / "model.param.bin").exists():
            text_param = pack_dir / "model.param.bin"
        if not weight.exists():
            raise FileNotFoundError(f"缺少 model.bin: {pack_dir}")
        return text_param, weight, board_cfg

    if cfg.get("param_file"):
        param = pack_dir / cfg["param_file"]
        bin_path = pack_dir / cfg["bin_file"]
        if param.exists() and bin_path.exists():
            return param, bin_path, cfg
    params = sorted(pack_dir.glob("*_int8.param")) or sorted(pack_dir.glob("*.param"))
    bins = [
        p for p in (sorted(pack_dir.glob("*_int8.bin")) or sorted(pack_dir.glob("*.bin")))
        if p.name != "model.param.bin"
    ]
    if not params or not bins:
        raise FileNotFoundError(f"目录中没有 ncnn 模型: {pack_dir}")
    return params[0], bins[0], cfg


def rewrite_relu6_clip_params(param_path: Path) -> Path:
    """python ncnn 把 -23310=2,0,6 的整数 0,6 解析成错误的 Clip，卷积输出全 0。"""
    import re

    path = Path(param_path)
    if path.name.endswith(".param.bin") or not path.is_file():
        return path
    text = path.read_text(encoding="utf-8", errors="replace")
    fixed, n = re.subn(r"-23310=2,0,6(?!\d)", "-23310=2,0.000000,6.000000", text)
    if n:
        path.write_text(fixed, encoding="utf-8")
        print(f"已修正 {path.name} 中 {n} 处 ReLU6 参数（0,6 → 0.0,6.0），否则特征会被清零", flush=True)
        text = fixed
    # Interp 1=2 写成整数时，python ncnn 的 float 尺度读成 0，FPN 上采样返回 -100
    fixed, n_interp = re.subn(
        r"(Interp\s+\S+\s+\d+\s+\d+\s+\S+\s+\S+\s+0=\d+)\s+1=(\d+)(?!\.)\s+2=(\d+)(?!\.)\b",
        lambda m: f"{m.group(1)} 1={float(m.group(2)):.6e} 2={float(m.group(3)):.6e}",
        text,
    )
    if n_interp:
        path.write_text(fixed, encoding="utf-8")
        print(
            f"已修正 {path.name} 中 {n_interp} 处 Interp 尺度（整数 2 → 2.0），"
            "否则 RetinaFace FPN 的 stride8/16 头无法抽取",
            flush=True,
        )
    return path


class NcnnSimulator:
    def __init__(self, param_path: Path, bin_path: Path, input_blob="data", output_blob=None, config: dict | None = None):
        self.param_path = rewrite_relu6_clip_params(Path(param_path))
        self.bin_path = Path(bin_path)
        self.config = config or {}
        self.input_blob = input_blob or self.config.get("input_blob") or "data"
        self.output_blob = output_blob or self.config.get("output_blob")
        self.backend = "numpy"
        self._py = None
        self._net = None
        if not self.output_blob:
            try:
                graph = parse_param(self.param_path)
                self.output_blob = graph[-1]["outputs"][0]
            except Exception:
                self.output_blob = "prob"
        try:
            import ncnn  # type: ignore

            net = ncnn.Net()
            net.opt.num_threads = 1
            if hasattr(net.opt, "lightmode"):
                net.opt.lightmode = False
            # PC 上 python ncnn 开 packing 容易和板上 param.bin 对不齐，关 packing 只影响速度
            net.opt.use_packing_layout = False
            net.opt.use_fp16_arithmetic = False
            net.opt.use_fp16_storage = False
            net.opt.use_winograd_convolution = True
            net.opt.use_sgemm_convolution = True
            param = str(self.param_path)
            if self.param_path.name.endswith(".param.bin"):
                loader = getattr(net, "load_param_bin", None)
                ret = loader(param) if loader else net.load_param(param)
            else:
                ret = net.load_param(param)
            if ret != 0:
                raise RuntimeError("ncnn load_param 失败")
            if net.load_model(str(self.bin_path)) != 0:
                raise RuntimeError("ncnn load_model 失败")
            self._py = ncnn
            self._net = net
            self.backend = "ncnn"
            if self.param_path.name.endswith(".param.bin"):
                if isinstance(self.config.get("blob_in"), int):
                    self.input_blob = int(self.config["blob_in"])
                if isinstance(self.config.get("blob_out"), int):
                    self.output_blob = int(self.config["blob_out"])
        except Exception as exc:
            if self.param_path.name.endswith(".param.bin"):
                raise RuntimeError(
                    f"无法加载 {self.param_path}（{exc}）。"
                    "板上四文件包需要 `pip install ncnn`，"
                    "或先用 export_ncnn.py 导出带 graph.param 的包。"
                ) from exc
            self._graph = parse_param(self.param_path)
            self._weights = load_bin_weights(self.bin_path, self._graph)
            if self.output_blob is None:
                self.output_blob = self._graph[-1]["outputs"][0]

    def _input_mat_from_rgb(self, rgb: np.ndarray):
        """对齐板上 from_pixels_resize + substract_mean_normalize。"""
        if self._net is None:
            raise RuntimeError("numpy 解释器请使用 invoke(nchw)，不要走 from_pixels")

        cfg = self.config or {}
        size = int(cfg.get("image_size") or cfg.get("input_size") or 224)
        pixel = str(cfg.get("pixel") or "bgr").lower()
        arr = np.asarray(rgb)
        if arr.ndim == 2:
            channels = 1
            src = np.ascontiguousarray(arr, np.uint8)
            h, w = src.shape
        else:
            if arr.ndim == 3 and arr.shape[0] in (1, 3) and arr.shape[-1] not in (1, 3):
                arr = np.transpose(arr, (1, 2, 0))
            src = np.ascontiguousarray(arr, np.uint8)
            h, w = int(src.shape[0]), int(src.shape[1])
            channels = 1 if src.ndim == 2 or src.shape[-1] == 1 else 3
            if channels == 1 and src.ndim == 3:
                src = np.ascontiguousarray(src[..., 0], np.uint8)

        ptype = self._py.Mat.PixelType
        if channels == 1:
            pixel_type = ptype.PIXEL_GRAY
        elif pixel in ("rgb", "bgr2rgb"):
            pixel_type = ptype.PIXEL_RGB
        else:
            pixel_type = getattr(ptype, "PIXEL_RGB2BGR", ptype.PIXEL_BGR)

        if w == size and h == size:
            mat_in = self._py.Mat.from_pixels(src, pixel_type, w, h)
        else:
            mat_in = self._py.Mat.from_pixels_resize(src, pixel_type, w, h, size, size)
        mean, norm = _mean_norm_lists(cfg, 1 if channels == 1 else 3)
        mat_in.substract_mean_normalize(mean, norm)
        return mat_in

    def invoke_rgb(self, rgb: np.ndarray) -> tuple[np.ndarray, float]:
        import time

        start = time.perf_counter()
        mat_in = self._input_mat_from_rgb(rgb)
        out = self._forward(mat_in)
        elapsed_us = (time.perf_counter() - start) * 1_000_000
        return out, elapsed_us

    def extract_blobs(self, rgb: np.ndarray, names: list[str]) -> tuple[dict[str, np.ndarray], float]:
        """抽出中间特征（SSD loc/conf），避免 python ncnn 跑 DetectionOutput 崩溃。"""
        import time

        if self._net is None:
            raise RuntimeError("extract_blobs 需要 python ncnn")
        start = time.perf_counter()
        mat_in = self._input_mat_from_rgb(rgb)
        ex = self._net.create_extractor()
        if hasattr(ex, "set_light_mode"):
            ex.set_light_mode(False)
        ret_in = ex.input(self.input_blob, mat_in)
        if ret_in != 0 and isinstance(self.input_blob, str) and self.config.get("blob_in") is not None:
            ret_in = ex.input(int(self.config["blob_in"]), mat_in)
        if ret_in != 0:
            raise RuntimeError(f"input {self.input_blob} 失败")
        blobs: dict[str, np.ndarray] = {}
        failed = []
        for name in names:
            ret, mat = ex.extract(name)
            if ret != 0:
                failed.append(f"{name}({ret})")
                continue
            blobs[name] = ncnn_mat_to_numpy(mat)
        if failed:
            raise RuntimeError("extract " + ", ".join(failed) + " 失败")
        elapsed_us = (time.perf_counter() - start) * 1_000_000
        return blobs, elapsed_us

    def invoke(self, nchw: np.ndarray) -> tuple[np.ndarray, float]:
        import time

        x = np.asarray(nchw, np.float32)
        if x.ndim == 4:
            x = x[0]
        if x.ndim == 3 and x.shape[0] not in (1, 3) and x.shape[-1] in (1, 3):
            x = np.transpose(x, (2, 0, 1))
        start = time.perf_counter()
        if self._net is not None:
            mat_in = self._py.Mat(x)
            out = self._forward(mat_in)
        else:
            out = run_graph(self._graph, self._weights, {self.input_blob: x})
            out = np.asarray(out[self.output_blob])
        elapsed_us = (time.perf_counter() - start) * 1_000_000
        return out, elapsed_us

    def _forward(self, mat_in) -> np.ndarray:
        ex = self._net.create_extractor()
        if hasattr(ex, "set_light_mode"):
            ex.set_light_mode(False)
        ret_in = ex.input(self.input_blob, mat_in)
        if ret_in != 0 and isinstance(self.input_blob, str) and self.config.get("blob_in") is not None:
            ret_in = ex.input(int(self.config["blob_in"]), mat_in)
        if ret_in != 0:
            raise RuntimeError(f"input {self.input_blob} 失败")
        ret, mat_out = ex.extract(self.output_blob)
        if ret != 0 and self.config.get("blob_out") is not None:
            ret, mat_out = ex.extract(int(self.config["blob_out"]))
        if ret != 0:
            raise RuntimeError(f"extract {self.output_blob} 失败")
        return ncnn_mat_to_numpy(mat_out)


def _mean_norm_lists(cfg: dict, channels: int) -> tuple[list[float], list[float]]:
    mean = cfg.get("mean")
    norm = cfg.get("norm")
    if mean is None or norm is None:
        if cfg.get("normalize", True):
            mean = [0.0] * channels
            norm = [1.0 / 255.0] * channels
        else:
            mean = [0.0] * channels
            norm = [1.0] * channels
    if isinstance(mean, (int, float)):
        mean = [float(mean)] * channels
        norm = [float(norm)] * channels
    mean_l = [float(x) for x in mean]
    norm_l = [float(x) for x in norm]
    while len(mean_l) < channels:
        mean_l.append(mean_l[-1] if mean_l else 0.0)
        norm_l.append(norm_l[-1] if norm_l else 1.0)
    return mean_l[:channels], norm_l[:channels]


def ncnn_mat_to_numpy(mat) -> np.ndarray:
    w, h, c = int(mat.w), int(mat.h), max(int(mat.c), 1)
    if w * h * c <= 0:
        return np.zeros((0, 6), dtype=np.float32)
    if w == 6 and c == 1 and h >= 1:
        rows = np.zeros((h, 6), dtype=np.float32)
        for i in range(h):
            raw = np.asarray(mat.row(i), np.float32).reshape(-1)
            n = min(6, int(raw.size))
            rows[i, :n] = raw[:n]
        return rows
    try:
        out = np.array(mat, dtype=np.float32)
        if out.ndim == 0:
            return np.array([float(out)], dtype=np.float32)
        return out
    except Exception:
        return np.zeros((0, 6), dtype=np.float32)


def parse_param(path: Path) -> list[dict]:
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    lines = [ln.strip() for ln in lines if ln.strip()]
    if lines[0] != "7767517":
        raise ValueError("不是 ncnn param（缺少 magic 7767517）")
    layers = []
    for ln in lines[2:]:
        parts = ln.split()
        n_in, n_out = int(parts[2]), int(parts[3])
        blobs = parts[4 : 4 + n_in + n_out]
        kv = {}
        for item in parts[4 + n_in + n_out :]:
            if "=" not in item:
                continue
            k, v = item.split("=", 1)
            if v.startswith("[") or "," in v:
                kv[int(k)] = v
            elif "." in v or "e" in v.lower():
                kv[int(k)] = float(v)
            else:
                try:
                    kv[int(k)] = int(v)
                except ValueError:
                    kv[int(k)] = v
        layers.append({
            "type": parts[0],
            "name": parts[1],
            "inputs": blobs[:n_in],
            "outputs": blobs[n_in:],
            "p": kv,
        })
    return layers


def load_bin_weights(path: Path, layers: list[dict]) -> dict:
    data = Path(path).read_bytes()
    off = 0
    store = {}

    def take_f32(n: int, tagged: bool) -> np.ndarray:
        nonlocal off
        if tagged:
            off += 4
        nbytes = n * 4
        arr = np.frombuffer(data, dtype="<f4", count=n, offset=off).copy()
        off += nbytes
        return arr

    for layer in layers:
        t, p, name = layer["type"], layer["p"], layer["name"]
        if t in ("Convolution", "ConvolutionDepthWise"):
            wsize = int(p.get(6, 0))
            bias = int(p.get(5, 0))
            num_output = int(p.get(0, 0))
            w = take_f32(wsize, True)
            b = take_f32(num_output, False) if bias else None
            store[name] = {"w": w, "b": b}
        elif t == "InnerProduct":
            wsize = int(p.get(2, 0))
            bias = int(p.get(1, 0))
            num_output = int(p.get(0, 0))
            w = take_f32(wsize, True)
            b = take_f32(num_output, False) if bias else None
            store[name] = {"w": w, "b": b}
        elif t == "BatchNorm":
            ch = int(p.get(0, 0))
            store[name] = {
                "slope": take_f32(ch, False),
                "mean": take_f32(ch, False),
                "var": take_f32(ch, False),
                "bias": take_f32(ch, False),
            }
    return store


def run_graph(layers: list[dict], weights: dict, feeds: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    blobs = {k: np.asarray(v, np.float32) for k, v in feeds.items()}
    for layer in layers:
        t = layer["type"]
        ins = [blobs[n] for n in layer["inputs"]]
        name = layer["name"]
        p = layer["p"]
        if t == "Input":
            continue
        if t == "Split":
            for out in layer["outputs"]:
                blobs[out] = ins[0]
            continue
        if t == "ReLU":
            blobs[layer["outputs"][0]] = np.maximum(ins[0], 0)
            continue
        if t == "Clip":
            lo, hi = float(p.get(0, 0)), float(p.get(1, 6))
            blobs[layer["outputs"][0]] = np.clip(ins[0], lo, hi)
            continue
        if t == "Softmax":
            x = ins[0].astype(np.float32).reshape(-1)
            x = x - np.max(x)
            e = np.exp(x)
            blobs[layer["outputs"][0]] = e / np.sum(e)
            continue
        if t == "Eltwise":
            blobs[layer["outputs"][0]] = ins[0] + ins[1]
            continue
        if t == "Concat":
            axis = int(p.get(0, 0))
            blobs[layer["outputs"][0]] = np.concatenate(ins, axis=axis)
            continue
        if t == "BatchNorm":
            w = weights[name]
            eps = float(p.get(1, 1e-5))
            x = ins[0]
            scale = w["slope"] / np.sqrt(w["var"] + eps)
            bias = w["bias"] - w["mean"] * scale
            blobs[layer["outputs"][0]] = x * scale.reshape(-1, 1, 1) + bias.reshape(-1, 1, 1)
            continue
        if t == "Pooling":
            blobs[layer["outputs"][0]] = _pool(ins[0], p)
            continue
        if t in ("Convolution", "ConvolutionDepthWise"):
            blobs[layer["outputs"][0]] = _conv(ins[0], weights[name], p, depthwise=(t == "ConvolutionDepthWise"))
            continue
        if t == "InnerProduct":
            blobs[layer["outputs"][0]] = _dense(ins[0], weights[name], p)
            continue
        raise ValueError(f"内置解释器不支持 {t}，请 pip install ncnn")
    return blobs


def _activation(x: np.ndarray, act: int, params) -> np.ndarray:
    if act == 1:
        return np.maximum(x, 0)
    if act == 3:
        return np.clip(x, 0, 6)
    if act == 4:
        return 1 / (1 + np.exp(-x))
    return x


def _conv(x, wb, p, depthwise: bool) -> np.ndarray:
    cout = int(p.get(0))
    kw = int(p.get(1))
    kh = int(p.get(11, kw))
    sw = int(p.get(3, 1))
    sh = int(p.get(13, sw))
    pad = int(p.get(4, 0))
    act = int(p.get(9, 0))
    cin, h, w = x.shape
    if pad == -233:
        oh, ow = int(np.ceil(h / sh)), int(np.ceil(w / sw))
        pad_h = max(0, (oh - 1) * sh + kh - h)
        pad_w = max(0, (ow - 1) * sw + kw - w)
        pt, pb = pad_h // 2, pad_h - pad_h // 2
        pl, pr = pad_w // 2, pad_w - pad_w // 2
    else:
        pt = pb = pl = pr = max(pad, 0)
        oh = (h + pt + pb - kh) // sh + 1
        ow = (w + pl + pr - kw) // sw + 1
    if pt or pb or pl or pr:
        x = np.pad(x, ((0, 0), (pt, pb), (pl, pr)))
    wgt = wb["w"]
    bias = wb["b"]
    y = np.zeros((cout, oh, ow), np.float32)
    if depthwise:
        k = wgt.reshape(cout, kh, kw)
        for oc in range(cout):
            ic = oc % cin
            for oy in range(oh):
                for ox in range(ow):
                    patch = x[ic, oy * sh : oy * sh + kh, ox * sw : ox * sw + kw]
                    y[oc, oy, ox] = np.sum(patch * k[oc])
                    if bias is not None:
                        y[oc, oy, ox] += bias[oc]
    else:
        k = wgt.reshape(cout, cin, kh, kw)
        for oc in range(cout):
            for oy in range(oh):
                for ox in range(ow):
                    patch = x[:, oy * sh : oy * sh + kh, ox * sw : ox * sw + kw]
                    y[oc, oy, ox] = np.sum(patch * k[oc])
                    if bias is not None:
                        y[oc, oy, ox] += bias[oc]
    return _activation(y, act, None)


def _pool(x, p) -> np.ndarray:
    pool = int(p.get(0, 0))
    global_p = int(p.get(4, 0))
    if global_p:
        if pool == 1:
            return x.mean(axis=(1, 2), keepdims=True)
        return x.max(axis=(1, 2), keepdims=True)
    kw = int(p.get(1, 2))
    kh = int(p.get(11, kw))
    sw = int(p.get(2, kw))
    sh = int(p.get(12, sw))
    cin, h, w = x.shape
    oh, ow = (h - kh) // sh + 1, (w - kw) // sw + 1
    y = np.zeros((cin, oh, ow), np.float32)
    for c in range(cin):
        for oy in range(oh):
            for ox in range(ow):
                patch = x[c, oy * sh : oy * sh + kh, ox * sw : ox * sw + kw]
                y[c, oy, ox] = patch.mean() if pool == 1 else patch.max()
    return y


def _dense(x, wb, p) -> np.ndarray:
    units = int(p.get(0))
    act = int(p.get(9, 0))
    vec = x.reshape(-1)
    w = wb["w"].reshape(units, -1)
    y = w @ vec
    if wb["b"] is not None:
        y = y + wb["b"]
    return _activation(y, act, None)
