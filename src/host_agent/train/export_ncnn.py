"""将训练好的 PyTorch 模型导出为板上可加载的 ncnn 包。

对齐 18_cam_lcd_hdmi_servo_net：
  PyTorch → ncnn2mem → models/<长名>/ 完整包
  → models_sd/<短名>/{model.param.bin, model.bin, labels.txt, model.mod}
"""
from __future__ import annotations

import stdio_utf8  # noqa: F401

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from board_model import (  # noqa: E402
    BOARD_DETECT_MEAN,
    BOARD_DETECT_NORM,
    BOARD_DETECT_PIXEL,
    BOARD_MODELS,
    IMAGE_OPS_NAME,
    auto_pack_display_name,
    mean_norm_from_train,
    pack_folder_name,
    parse_blob_ids,
    parse_named_blobs,
    purge_export_duplicates,
    set_pack_display_name,
    write_four_files,
    write_labels,
    write_model_cfg,
    _read_display_name,
)
from image_ops import write_image_ops_cfg  # noqa: E402
from common import (  # noqa: E402
    MODELS_OUTPUT,
    ensure_dir,
    find_latest_run,
    format_class_names,
    model_stem,
    read_json,
    write_json,
)
from ncnn_paths import find_ncnn_root, ncnn2int8_exe, ncnn2mem_exe, ncnn2table_exe  # noqa: E402
from torch2ncnn import convert_torch_to_ncnn  # noqa: E402


DEPLOYMENT_GUIDE = """# 板上模型包（18_cam_lcd_hdmi_servo_net）

上位机完整包在 `models/{folder_name}/`（含 model.cfg、graph.param）。不要整夹拷 SD。

推到板子的目录是 `models_sd/{board_folder}/`，对应 `0:/models/{board_folder}/`：

```
model.param.bin
model.bin
labels.txt
model.mod
```

板端不读 model.cfg 和 image_ops.cfg。model.mod 里写死了输入尺寸、通道、图像积木、blob 和解码。
文件夹名不超过 20 个 ASCII 字符。labels.txt 只用 ASCII。

推送：板上先打开「文件传输」。PC 网卡 192.168.1.100/24，板 192.168.1.10，UDP 69。
已训练模型页的「推送到 FPGA」走同一条 TFTP。

本包：pixel={pixel}  mean={mean}  norm={norm}  input_size={image_size}
板上状态：{board_status}

## PC 验证

```
python train/simulate_fpga.py --model {dataset}
```
"""


def parse_args():
    parser = argparse.ArgumentParser(description="导出 ncnn 并生成板上四文件模型档案")
    parser.add_argument("--model", default=None, help="数据集名，查找最近一次训练")
    parser.add_argument("--keras", default=None, help="权重路径（兼容旧参数名，现为 model.pt）")
    parser.add_argument("--weights", default=None, help="PyTorch .pt / .pth 路径")
    parser.add_argument("--run-dir", default=None)
    parser.add_argument("--quantize", default="int8", choices=("int8", "none", "float16", "dynamic", "qat"))
    parser.add_argument("--calib-samples", type=int, default=100)
    parser.add_argument("--no-deploy", action="store_true")
    parser.add_argument("--output-dir", default=None)
    parser.add_argument("--post", default=None, choices=("classify", "detect", "retinaface"))
    return parser.parse_args()


def _resolve_weight(run_dir: Path, info: dict) -> Path:
    for key in ("model_path", "keras_path"):
        raw = info.get(key)
        if raw:
            path = Path(raw)
            if path.exists():
                return path
    for name in ("model.pt", "model.pth"):
        path = run_dir / name
        if path.exists():
            return path
    raise FileNotFoundError(f"{run_dir} 中没有 model.pt")


def _locate_run_dir(raw: str) -> Path:
    """路径里的中文被控制台打坏时，按末级目录名回到 models_output。"""
    path = Path(raw)
    if (path / "train_info.json").is_file():
        return path
    name = path.name
    if name and "\ufffd" not in name:
        alt = MODELS_OUTPUT / name
        if (alt / "train_info.json").is_file():
            print(f"运行目录路径已损坏，改用 {alt}", flush=True)
            return alt
    return path


def resolve_run(args) -> tuple[Path, dict, Path]:
    explicit = getattr(args, "weights", None) or args.keras
    if explicit:
        weight_path = Path(explicit)
        run_dir = weight_path.parent
        info_path = run_dir / "train_info.json"
        info = read_json(info_path) if info_path.exists() else {}
        return run_dir, info, weight_path
    if args.run_dir:
        run_dir = _locate_run_dir(args.run_dir)
        info = read_json(run_dir / "train_info.json")
        return run_dir, info, _resolve_weight(run_dir, info)
    dataset = args.model or "mnist"
    run_dir, info = find_latest_run(dataset)
    return run_dir, info, _resolve_weight(run_dir, info)


def load_calib(run_dir: Path, info: dict, calib_samples: int):
    calib_images = run_dir / "calib_images.npy"
    calib_labels = run_dir / "calib_labels.npy"
    if calib_images.exists():
        images = np.load(calib_images)
        labels = np.load(calib_labels) if calib_labels.exists() else None
        return images, labels
    dataset = info.get("dataset")
    if not dataset:
        return None, None
    post = str(info.get("post") or info.get("task") or "").lower()
    if post in ("detect", "retinaface") or str(dataset).lower() in {"wider_face", "widerface"}:
        print("检测包缺少 calib_images.npy，跳过从分类 loader 回退取样", flush=True)
        return None, None
    from datasets import load_dataset

    data = load_dataset(
        dataset=dataset,
        image_size=info.get("image_size"),
        normalize=info.get("normalize", True),
        custom_path=info.get("custom_path"),
        keep_classes=info.get("kept_classes") or info.get("class_names"),
        min_classes=1 if post == "detect" else 2,
    )
    n = min(calib_samples, len(data["x_test"]))
    return data["x_test"][:n], data["y_test"][:n]


def _spread_indices(labels, limit: int) -> list[int]:
    """按类别轮流取样，避免测试集按类排好时只抽到前几类。"""
    groups: dict[int, list[int]] = {}
    for i, raw in enumerate(np.asarray(labels).reshape(-1)):
        groups.setdefault(int(raw), []).append(i)
    picked: list[int] = []
    while len(picked) < limit and groups:
        for key in list(groups):
            bucket = groups[key]
            if not bucket:
                groups.pop(key, None)
                continue
            picked.append(bucket.pop(0))
            if len(picked) >= limit:
                break
    return picked


def _as_uint8_hwc(image: np.ndarray) -> np.ndarray:
    arr = np.asarray(image)
    if arr.ndim == 3 and arr.shape[0] in (1, 3) and arr.shape[-1] not in (1, 3):
        arr = np.transpose(arr, (1, 2, 0))
    if np.issubdtype(arr.dtype, np.floating):
        peak = float(np.nanmax(arr)) if arr.size else 0.0
        if peak <= 1.5:
            arr = arr * 255.0
    return np.clip(np.round(arr), 0, 255).astype(np.uint8)


# 相对 fp32 下跌超过该值，视为量化把分类打崩，不再采用 INT8。
QUANT_COLLAPSE_DROP = 0.15


def _spot_classify_accuracy(
    fp_param: Path,
    fp_bin: Path,
    q_param: Path,
    q_bin: Path,
    images: np.ndarray,
    labels,
    *,
    image_size: int,
    pixel: str,
    mean,
    norm,
    output_blob: str,
) -> tuple[float, float, int]:
    """用同一批 0–255 图比较 fp32 与 INT8。返回 (fp32准确率, int8准确率, 张数)。失败时返回 -1。"""
    from ncnn_infer import NcnnSimulator

    idx = _spread_indices(labels, 24)
    if len(idx) < 8:
        return -1.0, -1.0, len(idx)
    y = np.asarray(labels).reshape(-1)
    cfg = {
        "image_size": int(image_size),
        "input_size": int(image_size),
        "pixel": pixel,
        "mean": list(mean),
        "norm": list(norm),
        "normalize": True,
        "input_blob": "data",
        "output_blob": output_blob,
    }
    fp = NcnnSimulator(fp_param, fp_bin, input_blob="data", output_blob=output_blob, config=cfg)
    q = NcnnSimulator(q_param, q_bin, input_blob="data", output_blob=output_blob, config=cfg)
    if fp.backend != "ncnn" or q.backend != "ncnn":
        return -1.0, -1.0, 0
    fp_ok = q_ok = 0
    for i in idx:
        img = _as_uint8_hwc(images[i])
        truth = int(y[i])
        fp_out, _ = fp.invoke_rgb(img)
        q_out, _ = q.invoke_rgb(img)
        fp_ok += int(int(np.argmax(fp_out)) == truth)
        q_ok += int(int(np.argmax(q_out)) == truth)
    n = len(idx)
    return fp_ok / n, q_ok / n, n


def review_int8_accuracy(
    fp_param: Path,
    fp_bin: Path,
    q_param: Path,
    q_bin: Path,
    images: np.ndarray | None,
    labels,
    *,
    image_size: int,
    pixel: str,
    mean,
    norm,
    output_blob: str,
) -> dict:
    """审阅量化。识别率崩溃时 decision=fp32，否则 decision=int8。"""
    print("=" * 60, flush=True)
    print("步骤：审阅量化", flush=True)
    if images is None or labels is None:
        print("没有带标签的抽查图，无法审阅，仍采用 INT8", flush=True)
        print("[QUANT_REVIEW] decision=int8 fp32=0 int8=0 samples=0 reason=unavailable", flush=True)
        print("=" * 60, flush=True)
        return {"decision": "int8", "reason": "unavailable", "fp32": 0.0, "int8": 0.0, "samples": 0}
    fp_acc, q_acc, n_check = _spot_classify_accuracy(
        fp_param, fp_bin, q_param, q_bin, images, labels,
        image_size=image_size, pixel=pixel, mean=mean, norm=norm,
        output_blob=output_blob,
    )
    if n_check < 8 or fp_acc < 0:
        print(f"抽查样本不足（{n_check} 张），无法判断是否崩溃，仍采用 INT8", flush=True)
        print(
            f"[QUANT_REVIEW] decision=int8 fp32={fp_acc:.4f} int8={q_acc:.4f} "
            f"samples={n_check} reason=insufficient",
            flush=True,
        )
        print("=" * 60, flush=True)
        return {"decision": "int8", "reason": "insufficient", "fp32": fp_acc, "int8": q_acc, "samples": n_check}
    drop = fp_acc - q_acc
    print(f"同一批图  fp32 {fp_acc:.1%}    INT8 {q_acc:.1%}    抽查 {n_check} 张", flush=True)
    collapsed = fp_acc >= 0.5 and drop >= QUANT_COLLAPSE_DROP
    if collapsed:
        print("审阅结论：量化会导致识别率崩溃，不采用 INT8，下发 fp32", flush=True)
        decision, reason = "fp32", "collapse"
    else:
        print("审阅结论：INT8 识别率保持，采用量化", flush=True)
        decision, reason = "int8", "ok"
    print(
        f"[QUANT_REVIEW] decision={decision} fp32={fp_acc:.4f} int8={q_acc:.4f} "
        f"samples={n_check} reason={reason}",
        flush=True,
    )
    print("=" * 60, flush=True)
    return {"decision": decision, "reason": reason, "fp32": fp_acc, "int8": q_acc, "samples": n_check}


def write_calib_npy(images: np.ndarray, out_dir: Path, pixel: str = "rgb") -> Path:
    """写成 0–255 CHW，与板上 (pixel-mean)*norm 一致。"""
    out_dir = ensure_dir(out_dir)
    paths = []
    pixel = str(pixel or "rgb").lower()
    for i, img in enumerate(images):
        arr = np.asarray(img, np.float32)
        if arr.max() <= 1.5:
            arr = arr * 255.0
        if arr.ndim == 2:
            chw = arr[None, ...]
        else:
            if pixel == "bgr" and arr.shape[-1] == 3:
                arr = arr[..., ::-1]
            chw = np.transpose(arr, (2, 0, 1))
        path = out_dir / f"img_{i:04d}.npy"
        np.save(path, chw.astype(np.float32))
        paths.append(path)
    list_path = out_dir / "list.txt"
    list_path.write_text("\n".join(str(p.resolve()) for p in paths) + "\n", encoding="utf-8")
    return list_path


def _run(cmd: list[str], cwd: Path | None = None) -> None:
    print("$ " + " ".join(cmd), flush=True)
    proc = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if proc.stdout:
        print(proc.stdout.rstrip(), flush=True)
    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip()
        raise RuntimeError(f"命令失败，退出码 {proc.returncode}: {cmd[0]}\n{err}")


def _stage_quantize_workdir(fp_param: Path, fp_bin: Path, list_txt: Path) -> Path:
    """把 fp32 模型和校准 npy 拷到纯英文临时目录，避免 ncnn2table fopen 中文路径失败。"""
    src_paths = [
        Path(ln.strip())
        for ln in Path(list_txt).read_text(encoding="utf-8").splitlines()
        if ln.strip()
    ]
    if not src_paths:
        raise FileNotFoundError(f"校准列表为空: {list_txt}")
    tmp = Path(tempfile.mkdtemp(prefix="fpga_ncnn_calib_"))
    shutil.copy2(fp_param, tmp / "model.param")
    shutil.copy2(fp_bin, tmp / "model.bin")
    npy_dir = tmp / "npy"
    npy_dir.mkdir()
    lines = []
    for i, src in enumerate(src_paths):
        if not src.exists():
            raise FileNotFoundError(f"校准样本不存在: {src}")
        dest = npy_dir / f"img_{i:04d}.npy"
        shutil.copy2(src, dest)
        lines.append(str(dest))
    (tmp / "list.txt").write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"量化临时目录: {tmp}  （校准 {len(lines)} 张）", flush=True)
    return tmp


def _remove_quantize_workdir(work: Path | None) -> None:
    if work is None or not work.exists():
        return
    shutil.rmtree(work, ignore_errors=True)
    if work.exists():
        print(f"警告: 未能删除量化临时目录 {work}", flush=True)
    else:
        print(f"已删除量化临时目录: {work}", flush=True)


def _restore_priorbox_params(param: Path) -> int:
    """ncnn2int8 会丢掉 PriorBox 里等于默认值的 7/8/11，补回与预设相同的 variance 和 auto-step。"""
    path = Path(param)
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    out, n = [], 0
    for raw in lines:
        parts = raw.split()
        if parts and parts[0] == "PriorBox":
            if "7=" not in raw:
                raw = raw.rstrip() + " 7=0.200000 8=0.200000"
                n += 1
            if "11=" not in raw:
                raw = raw.rstrip() + " 11=-233"
                n += 1
        out.append(raw)
    if n:
        path.write_text("\n".join(out) + "\n", encoding="utf-8")
    return n


def _strip_loc_scales(table: Path) -> int:
    """loc/conf 的 1×1 投影头保持 fp32。INT8 会把人脸 softmax 压到 <1%，框回归也会出 NaN。"""
    import re

    text = Path(table).read_text(encoding="utf-8", errors="replace")
    kept, dropped = [], 0
    head_re = re.compile(r"^ssd_(?:loc|conf)_\d+(?:_|$)")
    for raw in text.splitlines():
        token = raw.split()[0] if raw.split() else ""
        if head_re.match(token) and "_dw" not in token and "_bn" not in token and "_relu" not in token:
            dropped += 1
            continue
        kept.append(raw)
    Path(table).write_text("\n".join(kept) + ("\n" if kept else ""), encoding="utf-8")
    return dropped


def quantize_int8(
    fp_param: Path,
    fp_bin: Path,
    list_txt: Path,
    shape_chw: tuple[int, int, int],
    out_prefix: Path,
    *,
    mean=(0.0, 0.0, 0.0),
    norm=(1.0, 1.0, 1.0),
    pixel: str = "bgr",
    keep_loc_fp32: bool = False,
) -> tuple[Path, Path]:
    table_exe = ncnn2table_exe()
    int8_exe = ncnn2int8_exe()
    if table_exe is None or int8_exe is None:
        raise FileNotFoundError(
            "未找到 ncnn2table/ncnn2int8。仓库 prebuilt/ncnn/ 里应有这两个程序；"
            "也可设置 FPGA_NCNN_ROOT 指向已编译的 fpga_ncnn。"
        )
    c, h, w = shape_chw
    mean_t = tuple(float(x) for x in mean) + (0.0, 0.0, 0.0)
    norm_t = tuple(float(x) for x in norm) + (1.0, 1.0, 1.0)
    work = None
    try:
        work = _stage_quantize_workdir(fp_param, fp_bin, list_txt)
        table = work / "model.table"
        q_param_tmp = work / "model_int8.param"
        q_bin_tmp = work / "model_int8.bin"
        _run(
            [
                str(table_exe),
                str(work / "model.param"),
                str(work / "model.bin"),
                str(work / "list.txt"),
                str(table),
                f"shape=[{w},{h},{c}]",
                f"mean=[{mean_t[0]:g},{mean_t[1]:g},{mean_t[2]:g}]",
                f"norm=[{norm_t[0]:g},{norm_t[1]:g},{norm_t[2]:g}]",
                f"pixel={pixel}",
                "method=kl",
                "type=1",
                "thread=1",
            ]
        )
        if keep_loc_fp32:
            n = _strip_loc_scales(table)
            if n:
                print(f"检测头 loc/conf {n} 条 scale 不量化，保持 fp32", flush=True)
        _run(
            [
                str(int8_exe),
                str(work / "model.param"),
                str(work / "model.bin"),
                str(q_param_tmp),
                str(q_bin_tmp),
                str(table),
            ]
        )
        q_param = Path(str(out_prefix) + "_int8.param")
        q_bin = Path(str(out_prefix) + "_int8.bin")
        shutil.copy2(q_param_tmp, q_param)
        shutil.copy2(q_bin_tmp, q_bin)
        shutil.copy2(table, out_prefix.with_suffix(".table"))
        restored = _restore_priorbox_params(q_param)
        if restored:
            print(f"已补回 PriorBox 参数 {restored} 处（variance/step），与板上 VOC 预设一致", flush=True)
        return q_param, q_bin
    finally:
        _remove_quantize_workdir(work)


def make_param_bin(param: Path, bin_path: Path, out_dir: Path) -> tuple[Path, Path]:
    """调用 ncnn2mem，得到板上 load_param 所需的 model.param.bin 和 blob 编号头文件。"""
    mem = ncnn2mem_exe()
    if mem is None:
        raise FileNotFoundError(
            "未找到 ncnn2mem.exe。板上必须用二进制 param.bin。"
            "仓库 prebuilt/ncnn/ 里应有该程序，也可设置 FPGA_NCNN_ROOT。"
        )
    id_h = out_dir / "model_id.h"
    mem_cpp = out_dir / "model_mem.cpp"
    _run([str(mem), str(param), str(bin_path), str(id_h), str(mem_cpp)])
    produced = Path(str(param) + ".bin")
    dest = out_dir / "model.param.bin"
    if not produced.exists():
        raise FileNotFoundError(f"ncnn2mem 未生成 {produced}")
    shutil.copy2(produced, dest)
    return dest, id_h


def _convert_weights(weight_path: Path, info: dict, fp_param: Path, fp_bin: Path) -> dict:
    suffix = weight_path.suffix.lower()
    if suffix in (".pt", ".pth"):
        from models import load_checkpoint, rebuild_from_checkpoint

        ckpt = load_checkpoint(weight_path, map_location="cpu")
        model = rebuild_from_checkpoint(ckpt, arch_file=info.get("arch_file") or ckpt.get("arch_file"))
        shape = ckpt.get("input_shape") or info.get("input_shape")
        return convert_torch_to_ncnn(model, shape, fp_param, fp_bin)
    raise ValueError(f"只接受 PyTorch 权重（.pt / .pth），当前是 {weight_path}")


def main():
    args = parse_args()

    run_dir, info, weight_path = resolve_run(args)
    keras_path = weight_path
    dataset = info.get("dataset", args.model or "model")
    arch = info.get("arch", "cnn")
    quantize = args.quantize.lower()
    requested_qat = quantize == "qat"
    post = str(args.post or info.get("post") or "classify").lower()
    if requested_qat:
        quantize = "int8"
    if quantize in ("float16", "dynamic"):
        print(f"提示: ncnn 部署将 {quantize} 按 fp32 导出；板上 INT8 请用 --quantize int8")
        quantize = "none"

    print("=" * 60)
    print("ncnn 导出（18_cam_lcd_hdmi_servo_net）")
    print("=" * 60)
    print(f"权重: {weight_path}")
    print(f"量化: {quantize}")
    root = find_ncnn_root()
    print(f"ncnn 工具: {root or '未找到（INT8 量化工具不可用）'}")

    ssd_cfg = info.get("ssd_cfg")
    if not ssd_cfg:
        cfg_path = run_dir / "ssd_cfg.json"
        if cfg_path.exists():
            ssd_cfg = read_json(cfg_path)
    retina_cfg = info.get("retina_cfg")
    if not retina_cfg:
        rpath = run_dir / "retina_cfg.json"
        if rpath.exists():
            retina_cfg = read_json(rpath)
    arch_l = str(info.get("arch") or arch or "").lower()
    is_retina = bool(retina_cfg) or "retina" in arch_l
    if post == "detect" and not ssd_cfg and not is_retina:
        raise ValueError("检测导出需要 ssd_cfg.json（SSDLite）或 retina_cfg.json（RetinaFace）")

    fp_param = run_dir / f"{dataset}_fp32.param"
    fp_bin = run_dir / f"{dataset}_fp32.bin"
    meta = _convert_weights(weight_path, info, fp_param, fp_bin)
    print(f"fp32 param: {fp_param}  layers={meta['layers']} blobs={meta['blobs']}")

    c, h, w = meta["input"]
    image_size = int(info.get("image_size") or h)
    channels = int(info.get("channels") or c)
    if is_retina:
        pixel = "bgr2rgb"
        mean, norm = (0.0, 0.0, 0.0), (1.0, 1.0, 1.0)
        print("RetinaFace 预处理：PIXEL_BGR2RGB，mean=0，norm=1", flush=True)
    elif post == "detect":
        pixel = BOARD_DETECT_PIXEL
        mean, norm = BOARD_DETECT_MEAN, BOARD_DETECT_NORM
        print("检测预处理对齐板上 VOC SSDLite：pixel=bgr mean=127.5 norm=1/127.5", flush=True)
    else:
        pixel = "rgb" if channels == 3 else "bgr"
        mean, norm = mean_norm_from_train(bool(info.get("normalize", True)))

    if is_retina:
        from retina_export import attach_retina_softmax

        r_meta = attach_retina_softmax(fp_param, retina_cfg or {})
        meta["output_blob"] = r_meta["output_blob"]
        meta["layers"] = r_meta["layers"]
        meta["blobs"] = r_meta["blobs"]
        print(f"已接 RetinaFace cls Softmax，输出 blob={meta['output_blob']}")
        post = "retinaface"
    elif post == "detect":
        from ssd_export import attach_ssd_detection

        ssd_meta = attach_ssd_detection(fp_param, ssd_cfg)
        meta["output_blob"] = ssd_meta["output_blob"]
        meta["layers"] = ssd_meta["layers"]
        meta["blobs"] = ssd_meta["blobs"]
        print(f"已接 PriorBox/DetectionOutput，输出 blob={meta['output_blob']}")

    images, _labels = load_calib(run_dir, info, args.calib_samples)
    calib_dir = run_dir / "calib_npy"
    list_txt = None
    if images is not None:
        list_txt = write_calib_npy(images, calib_dir, pixel=pixel)

    used_param, used_bin = fp_param, fp_bin
    quant_note = "fp32"
    if info.get("qat") and quantize == "int8" and not is_retina and post != "detect":
        requested_qat = True
    if requested_qat and (is_retina or post == "detect"):
        print("检测模型不使用 QAT", flush=True)
        requested_qat = False
    elif requested_qat and not info.get("qat"):
        print("权重不是量化感知训练得到的，改为训练后 INT8", flush=True)
        requested_qat = False
    elif requested_qat:
        print("按量化感知训练的权重导出 INT8，标记为 QAT，不做训练后量化审阅", flush=True)
    if is_retina and quantize == "int8":
        print("RetinaFace 建议 fp32，已跳过 INT8（人脸 INT8 易把分数压没）", flush=True)
        quantize = "none"
    if quantize == "int8":
        if images is None or list_txt is None:
            raise FileNotFoundError("INT8 量化需要校准样本（calib_images.npy）")
        try:
            used_param, used_bin = quantize_int8(
                fp_param, fp_bin, list_txt, meta["input"], run_dir / dataset,
                mean=mean, norm=norm, pixel=pixel, keep_loc_fp32=(post == "detect"),
            )
            quant_note = "int8"
            print(f"int8 param: {used_param}")
            if requested_qat and post not in ("detect", "retinaface"):
                quant_note = "qat"
                print("量化感知训练的权重按 INT8 下发，标记为 QAT，不做训练后量化审阅", flush=True)
            elif post not in ("detect", "retinaface"):
                try:
                    review = review_int8_accuracy(
                        fp_param, fp_bin, used_param, used_bin, images, _labels,
                        image_size=image_size, pixel=pixel, mean=mean, norm=norm,
                        output_blob=meta.get("output_blob") or "prob",
                    )
                except Exception as exc:
                    print(f"量化审阅未完成，仍使用 int8: {exc}", flush=True)
                    review = {"decision": "int8"}
                if review.get("decision") == "fp32":
                    used_param, used_bin = fp_param, fp_bin
                    quant_note = "fp32"
        except Exception as exc:
            print(f"INT8 量化失败，保留 fp32: {exc}")
            quant_note = "fp32"

    param_bin, id_h = make_param_bin(used_param, used_bin, run_dir)
    blob_in, blob_out = parse_blob_ids(id_h, meta["input_blob"], meta["output_blob"])
    print(f"blob_in={blob_in} ({meta['input_blob']})  blob_out={blob_out} ({meta['output_blob']})")

    class_names = format_class_names(
        info.get("class_names") or [str(i) for i in range(int(info.get("num_classes") or 10))]
    )
    stem = str(info.get("pack_stem") or model_stem(str(dataset), str(arch)))
    folder_name = pack_folder_name(dataset, arch, quant_note)
    info["pack_stem"] = stem
    info["class_names"] = class_names
    info["quantize"] = quant_note
    info["post"] = post
    write_json(run_dir / "train_info.json", info)
    extra = ""
    if channels == 1:
        extra = "网络输入 1 通道；OV5640 摄像头为 3 通道，整帧推理需 3 通道模型"
    ops = info.get("image_ops") or []
    if ops:
        chain = " → ".join(str(s.get("op") or "") for s in ops if s)
        note = f"训练图像积木: {chain}"
        extra = f"{extra}；{note}" if extra else note

    det_cfg = retina_cfg if is_retina else ssd_cfg
    extra_fields = None
    if is_retina:
        names = []
        for key in ("cls_blobs", "bbox_blobs", "lmk_blobs"):
            names.extend(list((retina_cfg or {}).get(key) or []))
        found = parse_named_blobs(id_h, names)
        extra_fields = {
            "blob_cls8": found.get("face_rpn_cls_prob_reshape_stride8", -1),
            "blob_bbox8": found.get("face_rpn_bbox_pred_stride8", -1),
            "blob_lmk8": found.get("face_rpn_landmark_pred_stride8", -1),
            "blob_cls16": found.get("face_rpn_cls_prob_reshape_stride16", -1),
            "blob_bbox16": found.get("face_rpn_bbox_pred_stride16", -1),
            "blob_lmk16": found.get("face_rpn_landmark_pred_stride16", -1),
            "blob_cls32": found.get("face_rpn_cls_prob_reshape_stride32", -1),
            "blob_bbox32": found.get("face_rpn_bbox_pred_stride32", -1),
            "blob_lmk32": found.get("face_rpn_landmark_pred_stride32", -1),
        }
        print("RetinaFace blob: " + " ".join(f"{k}={v}" for k, v in extra_fields.items()), flush=True)
    cfg_kwargs = dict(
        name=folder_name,
        input_size=image_size,
        pixel=pixel,
        blob_in=blob_in,
        blob_out=blob_out,
        mean=mean,
        norm=norm,
        post=post,
        topk=3,
        channels=channels,
        extra_comment=extra,
        score_th=float((det_cfg or {}).get("confidence_threshold") or 0.5) if post in ("detect", "retinaface") else None,
        nms_iou_th=float((det_cfg or {}).get("nms_threshold") or 0.4) if post in ("detect", "retinaface") else None,
        max_keep=int((det_cfg or {}).get("keep_top_k") or 32) if post in ("detect", "retinaface") else None,
        extra_fields=extra_fields,
    )
    write_model_cfg(run_dir / "model.cfg", **cfg_kwargs)
    write_labels(run_dir / "labels.txt", class_names, post=post)
    write_image_ops_cfg(run_dir / IMAGE_OPS_NAME, info.get("image_ops") or [])
    shutil.copy2(used_bin, run_dir / "model.bin")
    shutil.copy2(used_param, run_dir / "graph.param")

    if args.no_deploy:
        print(f"\n已导出: {param_bin}")
        return

    board_dir = BOARD_MODELS / folder_name
    saved_name = _read_display_name(board_dir) if board_dir.exists() else ""
    old_run = ""
    old_info = board_dir / "train_info.json"
    if old_info.exists():
        try:
            old_run = str(read_json(old_info).get("run_dir") or "")
        except Exception:
            old_run = ""
    board_dir = write_four_files(
        board_dir,
        param_bin=param_bin,
        weight_bin=used_bin,
        cfg_text_path=run_dir / "model.cfg",
        labels_path=run_dir / "labels.txt",
    )
    shutil.copy2(used_param, board_dir / "graph.param")
    shutil.copy2(fp_param, board_dir / fp_param.name)
    shutil.copy2(fp_bin, board_dir / fp_bin.name)
    if id_h.exists():
        shutil.copy2(id_h, board_dir / id_h.name)
    if (run_dir / "train_info.json").exists():
        shutil.copy2(run_dir / "train_info.json", board_dir / "train_info.json")
    sample_src = run_dir / "test_samples"
    if sample_src.is_dir() and (sample_src / "test_image_0.npy").is_file():
        shutil.copytree(sample_src, board_dir / "test_samples", dirs_exist_ok=True)
    ops_cfg = run_dir / IMAGE_OPS_NAME
    if ops_cfg.exists():
        shutil.copy2(ops_cfg, board_dir / IMAGE_OPS_NAME)
    purge_export_duplicates(folder_name, keep=board_dir)

    config = {
        "backend": "ncnn",
        "dataset": dataset,
        "arch": arch,
        "pack_stem": stem,
        "quantize": quant_note,
        "folder_name": folder_name,
        "param_file": "graph.param",
        "bin_file": "model.bin",
        "board_param": "model.param.bin",
        "fp32_param": fp_param.name,
        "fp32_bin": fp_bin.name,
        "keras_path": str(keras_path),
        "image_size": image_size,
        "channels": channels,
        "num_classes": len(class_names),
        "class_names": class_names,
        "normalize": info.get("normalize", True),
        "float_test_accuracy": info.get("test_accuracy"),
        "input_blob": meta["input_blob"],
        "output_blob": meta["output_blob"],
        "blob_in": blob_in,
        "blob_out": blob_out,
        "pixel": pixel,
        "mean": list(mean),
        "norm": list(norm),
        "post": post,
        "input_shape_nchw": list(meta["input"]),
        "ncnn_layers": meta["layers"],
        "host_models_dir": str(board_dir),
        "fpga_ncnn_root": str(root) if root else None,
        "board": "18_cam_lcd_hdmi_servo_net",
    }
    write_json(board_dir / "deployment_config.json", config)
    same_training = False
    if saved_name and old_run:
        try:
            same_training = Path(old_run).resolve() == Path(run_dir).resolve()
        except Exception:
            same_training = False
    try:
        if saved_name and same_training:
            set_pack_display_name(board_dir, saved_name)
        else:
            set_pack_display_name(
                board_dir,
                auto_pack_display_name(
                    dataset=str(dataset),
                    arch=str(arch),
                    quant=quant_note,
                    run_dir=run_dir,
                ),
            )
    except Exception:
        pass

    from board_mod import prepare_board_pack

    packed = prepare_board_pack(board_dir)
    sd_dir = Path(packed["sd_dir"])
    guide = DEPLOYMENT_GUIDE.format(
        folder_name=folder_name,
        board_folder=packed["board_folder"],
        pixel=pixel,
        mean=",".join(f"{x:g}" for x in mean),
        norm=",".join(f"{x:g}" for x in norm),
        image_size=image_size,
        dataset=dataset,
        board_status=packed["status"],
    )
    (board_dir / "DEPLOYMENT_GUIDE.md").write_text(guide, encoding="utf-8")

    print("\n" + "=" * 60)
    print("导出完成 —— 上位机完整包：")
    print(f"  {board_dir}")
    print(f"板上目录 0:/models/{packed['board_folder']}/ ：")
    print(f"  {sd_dir}")
    print(f"  {packed['status']}")
    for name in ("model.param.bin", "model.bin", "labels.txt", "model.mod"):
        item = sd_dir / name
        if item.exists():
            print(f"    {name}  ({item.stat().st_size} bytes)")
    print("=" * 60)
    print(f"[EXPORT_DONE] ncnn={board_dir / 'model.bin'} deploy_dir={board_dir}", flush=True)
    print(f"[EXPORT_SD] dir={sd_dir}", flush=True)


if __name__ == "__main__":
    main()
