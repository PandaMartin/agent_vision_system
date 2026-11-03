"""PC 端模拟竞赛板 ncnn 推理（Zynq 裸机同款 param/bin）。"""
from __future__ import annotations

import stdio_utf8  # noqa: F401

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from board_model import has_ncnn_weights, list_board_packs, pack_post  # noqa: E402
from common import FPGA_DEPLOYMENT, find_latest_run  # noqa: E402
from ncnn_infer import NcnnSimulator, load_pack_param_bin  # noqa: E402


def _has_model(folder: Path) -> bool:
    return has_ncnn_weights(folder)


def find_ncnn_pack(
    dataset: str | None = None,
    pack_dir: str | None = None,
    prefer: str | None = None,
) -> Path:
    if pack_dir:
        path = Path(pack_dir)
        if not path.exists():
            raise FileNotFoundError(f"部署包不存在: {path}")
        return path

    packs: list[Path] = []
    if FPGA_DEPLOYMENT.exists():
        packs.extend(p for p in FPGA_DEPLOYMENT.iterdir() if p.is_dir())
    packs.extend(list_board_packs())
    key = (dataset or "").strip().lower()
    if key:
        try:
            run_dir, _ = find_latest_run(key)
            packs.append(run_dir)
        except FileNotFoundError:
            pass

    seen = set()
    unique = []
    for pack in packs:
        resolved = str(pack.resolve()) if pack.exists() else str(pack)
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(pack)
    packs = [p for p in unique if p.exists() and _has_model(p)]

    if key:
        named = [p for p in packs if key in p.name.lower()]
        if named:
            packs = named

    if prefer:
        preferred = [p for p in packs if pack_post(p) == prefer]
        if preferred:
            packs = preferred

    packs.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    if packs:
        return packs[0]
    raise FileNotFoundError(
        "未找到 ncnn 部署包。请先训练并导出，或把竞赛板 "
        "12_ov5640_ncnn/models 下的四文件文件夹拷到本仓库 models/。"
    )


def to_nchw(image: np.ndarray) -> np.ndarray:
    arr = np.asarray(image, np.float32)
    if arr.ndim == 2:
        return arr[None, ...]
    if arr.ndim == 3 and arr.shape[-1] in (1, 3):
        return np.transpose(arr, (2, 0, 1))
    return arr


def as_board_image(image: np.ndarray) -> np.ndarray:
    """模拟走与板上相同的 0–255 像素入口。训练集里保存的是 0–1 浮点。"""
    arr = np.asarray(image)
    if arr.ndim == 3 and arr.shape[0] in (1, 3) and arr.shape[-1] not in (1, 3):
        arr = np.transpose(arr, (1, 2, 0))
    if np.issubdtype(arr.dtype, np.floating):
        peak = float(np.nanmax(arr)) if arr.size else 0.0
        if peak <= 1.5:
            arr = arr * 255.0
    return np.ascontiguousarray(np.clip(np.round(arr), 0, 255), np.uint8)


def load_samples(sample_dir: Path, num_tests: int):
    images, labels = [], []
    idx = 0
    while (sample_dir / f"test_image_{idx}.npy").exists() and idx < num_tests:
        images.append(np.load(sample_dir / f"test_image_{idx}.npy"))
        label_file = sample_dir / f"test_label_{idx}.txt"
        labels.append(int(label_file.read_text(encoding="utf-8").strip()) if label_file.exists() else -1)
        idx += 1
    if not images:
        raise FileNotFoundError(f"没有测试样本: {sample_dir}")
    return images, labels


def _read_train_info(pack: Path) -> dict:
    path = pack / "train_info.json"
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def resolve_sample_dir(pack: Path, explicit: str | None) -> Path | None:
    """优先用包内样本，没有则用训练目录里保存的 test_samples。"""
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.append(pack / "test_samples")
    run_dir = str(_read_train_info(pack).get("run_dir") or "").strip()
    if run_dir:
        candidates.append(Path(run_dir) / "test_samples")
    for folder in candidates:
        if (folder / "test_image_0.npy").is_file():
            return folder
    return None


def samples_from_dataset(pack: Path, num_tests: int):
    """训练目录也没有样本时，按 train_info 从原数据集取一小批。"""
    info = _read_train_info(pack)
    dataset = str(info.get("dataset") or "").strip()
    post = str(info.get("post") or "classify").lower()
    if not dataset or post in ("detect", "retinaface"):
        return None
    from datasets import load_dataset

    data = load_dataset(
        dataset=dataset,
        image_size=info.get("image_size"),
        normalize=bool(info.get("normalize", True)),
        custom_path=info.get("custom_path"),
        keep_classes=info.get("kept_classes") or info.get("class_names"),
    )
    images = data.get("x_test")
    labels = data.get("y_test")
    if images is None or len(images) == 0:
        return None
    n = min(num_tests, len(images))
    return list(images[:n]), [int(v) for v in labels[:n]]


def parse_args():
    parser = argparse.ArgumentParser(description="模拟 FPGA / ncnn 推理")
    parser.add_argument("--model", default=None, help="数据集名")
    parser.add_argument("--pack", default=None, help="fpga_deployment 下的包目录")
    parser.add_argument("--sample-dir", default=None)
    parser.add_argument("--num-tests", type=int, default=10)
    return parser.parse_args()


def main():
    args = parse_args()
    pack = find_ncnn_pack(args.model, args.pack)
    param, bin_path, cfg = load_pack_param_bin(pack)
    sample_dir = resolve_sample_dir(pack, args.sample_dir)
    if sample_dir is not None:
        images, labels = load_samples(sample_dir, args.num_tests)
        print(f"测试样本: {sample_dir}")
    else:
        loaded = samples_from_dataset(pack, args.num_tests)
        if loaded is None:
            raise FileNotFoundError(
                f"没有测试样本: {pack / 'test_samples'}，训练目录里也没有，且无法从数据集补取"
            )
        images, labels = loaded
        print(f"测试样本: 从数据集现取 {len(images)} 张")

    print("=" * 70)
    print("ncnn 模拟推理（对齐 Zynq 裸机：单线程 / 关 winograd / packing）")
    print("=" * 70)
    print(f"部署包: {pack}")
    print(f"模型: {param.name} + {bin_path.name}")

    sim = NcnnSimulator(
        param,
        bin_path,
        input_blob=cfg.get("input_blob", "data"),
        output_blob=cfg.get("output_blob"),
        config=cfg,
    )
    print(f"后端: {sim.backend}  输入blob={sim.input_blob}  输出blob={sim.output_blob}")

    correct = 0
    total_us = 0.0
    preds = []
    n = min(args.num_tests, len(images))
    for i in range(n):
        if sim.backend == "ncnn":
            output, elapsed_us = sim.invoke_rgb(as_board_image(images[i]))
        else:
            output, elapsed_us = sim.invoke(to_nchw(images[i]))
        predicted = int(np.argmax(output))
        preds.append(predicted)
        total_us += elapsed_us
        ok = predicted == labels[i]
        if ok:
            correct += 1
        status = "PASS" if ok else "FAIL"
        print(
            f"  样本 {i}: 真实={labels[i]}, 预测={predicted}, {status}, "
            f"推理时间={elapsed_us:.0f}us"
        )

    accuracy = correct / n if n else 0.0
    avg_us = total_us / n if n else 0.0
    fps = (1_000_000 / avg_us) if avg_us else 0.0
    print(f"\n准确率: {accuracy:.2%} ({correct}/{n})")
    print(f"平均推理时间: {avg_us:.0f}us  预估吞吐: ~{fps:.1f} FPS")

    valid = [(int(t), int(p)) for t, p in zip(labels[:n], preds) if int(t) >= 0]
    if valid:
        n_classes = max(max(t, p) for t, p in valid) + 1
        cm = np.zeros((n_classes, n_classes), dtype=int)
        for truth, pred in valid:
            cm[truth, pred] += 1
        packed = ";".join(",".join(str(int(v)) for v in row) for row in cm)
        print(f"[SIM_CM] {packed}", flush=True)

    print("=" * 70)
    print(f"[SIM_DONE] acc={accuracy:.4f} samples={n}", flush=True)


if __name__ == "__main__":
    main()
