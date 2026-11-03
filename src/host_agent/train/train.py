"""训练神经网络并保存 PyTorch 权重，再由 export_ncnn.py 导出竞赛板四文件档案。

示例:
  python train.py --model mnist --epochs 10
  python train.py --dataset cifar10 --arch deep_cnn --epochs 20 --image-size 32
  python train.py --dataset custom --custom-path D:\\data\\gestures --arch mobilenet --image-size 96
"""
from __future__ import annotations

import stdio_utf8  # noqa: F401

import argparse
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))

from common import (  # noqa: E402
    MODELS_OUTPUT,
    ensure_dir,
    format_class_names,
    model_stem,
    now_stamp,
    normalize_arch_name,
    normalize_dataset_name,
    update_model_registry,
    write_json,
)
from datasets import default_image_size, load_dataset, parse_keep_classes  # noqa: E402
from models import create_model, resolve_train_arch  # noqa: E402
from torch_runtime import count_params, print_model, seed_all, select_device  # noqa: E402
from torch_train import evaluate_classify, fit_classify, fit_detect  # noqa: E402


def _save_test_samples(x_test: np.ndarray, y_test: np.ndarray, output_dir: Path, num_samples: int = 10) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    n = min(num_samples, len(x_test))
    for i in range(n):
        np.save(output_dir / f"test_image_{i}.npy", x_test[i])
        (output_dir / f"test_label_{i}.txt").write_text(str(int(y_test[i])), encoding="utf-8")
    print(f"已保存 {n} 个测试样本到 {output_dir}")


def parse_args():
    parser = argparse.ArgumentParser(description="训练 FPGA 部署用神经网络（PyTorch）")
    parser.add_argument("--model", default=None, help="快捷参数：数据集名（mnist/fashion_mnist/kmnist/cifar10/cifar100/flowers/rps/hands/coco/wider_face），兼容旧文档")
    parser.add_argument("--dataset", default=None, help="数据集: mnist / fashion_mnist / kmnist / cifar10 / cifar100 / flowers / rps / hands / coco / wider_face / custom")
    parser.add_argument("--custom-path", default=None, help="自定义数据集根目录（类别子文件夹）")
    parser.add_argument("--classes", default=None, help="训练使用的类别，逗号分隔名称或索引，默认全部")
    parser.add_argument("--post", default="classify", choices=("classify", "detect"), help="classify 至少 2 类，detect 可以 1 类（SSDLite 或 RetinaFace）")
    parser.add_argument("--arch", default="simple_cnn", help="架构: squeezenet / mobilenet / ssdlite / retinaface / simple_cnn / deep_cnn / resnet18")
    parser.add_argument("--arch-file", default=None, help="自定义架构 Python 文件（需提供 create_model）")
    parser.add_argument("--epochs", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--lr", type=float, default=0.001, dest="learning_rate")
    parser.add_argument("--image-size", type=int, default=None, help="输入边长，默认随数据集")
    parser.add_argument("--normalize", dest="normalize", action="store_true", default=True)
    parser.add_argument("--no-normalize", dest="normalize", action="store_false")
    parser.add_argument("--augmentation", action="store_true", default=False)
    parser.add_argument("--validation-split", type=float, default=0.1)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output-dir", default=None, help="输出根目录，默认 models_output/")
    parser.add_argument("--calib-samples", type=int, default=200, help="保存的量化校准样本数")
    parser.add_argument("--test-samples", type=int, default=10, help="保存到 run 目录的测试样本数")
    parser.add_argument("--max-samples", type=int, default=0, help="检测训练最多读取的样本数，0 表示不限制（用满 WIDER FACE / COCO）")
    parser.add_argument("--image-ops", default=None, help="图像处理积木 JSON 字符串或文件路径")
    parser.add_argument("--qat", action="store_true", help="分类模型使用量化感知训练，导出时再做成 INT8")
    return parser.parse_args()


def resolve_dataset_and_arch(args) -> tuple[str, str]:
    dataset = args.dataset
    arch = args.arch
    if args.model:
        token = args.model.strip().lower()
        try:
            dataset = dataset or normalize_dataset_name(token)
        except ValueError:
            try:
                arch = normalize_arch_name(token)
            except ValueError as exc:
                raise ValueError(
                    f"--model {args.model} 既不是数据集也不是架构。"
                    "数据集: mnist/fashion_mnist/kmnist/cifar10/cifar100/flowers/rps/hands/coco/wider_face；架构: squeezenet/mobilenet/simple_cnn/deep_cnn/resnet18"
                ) from exc
    dataset = normalize_dataset_name(dataset or "mnist")
    if args.arch_file:
        file_stem = Path(args.arch_file).stem
        if not arch or arch in ("simple_cnn", "custom"):
            arch = file_stem
    else:
        arch = normalize_arch_name(arch or "simple_cnn")
    return dataset, arch


def save_checkpoint(path: Path, model, meta: dict) -> None:
    payload = dict(meta)
    payload["state_dict"] = {k: v.detach().cpu() for k, v in model.state_dict().items()}
    torch.save(payload, path)


def main():
    args = parse_args()
    dataset, arch = resolve_dataset_and_arch(args)
    seed_all(args.seed)
    device = select_device()

    post = str(args.post or "classify").lower()
    if post == "detect":
        if args.arch_file:
            raise ValueError("检测请使用 MobileNetV2 SSDLite 或 RetinaFace 预设，自定义层列表不含检测头")
        arch = resolve_train_arch(arch, post)
        if args.image_size is None:
            image_size = 320 if arch == "retinaface" else 300
        else:
            image_size = args.image_size
        if arch == "retinaface" and int(image_size) % 32 != 0:
            print(f"RetinaFace 输入必须被 32 整除，已将 {image_size} 改为 320")
            image_size = 320
        if device.type != "cuda" and args.batch_size > 16:
            print(f"检测训练在 CPU 上将 batch={args.batch_size} 改为 8")
            args.batch_size = 8
        if not args.augmentation:
            args.augmentation = True
            print("检测训练已开启光度抖动/随机垫边/人脸裁剪/翻转，且每轮重新抽样")
        print("检测优化器: AdamW(weight_decay=1e-4) + 余弦退火，结束时恢复最佳 val_loss 权重")
        if args.qat:
            print("检测模型不使用 QAT，本次按普通训练进行")
    else:
        image_size = args.image_size or default_image_size(dataset)

    print("=" * 60)
    print("FPGA 神经网络训练（PyTorch）")
    print("=" * 60)
    print(f"数据集: {dataset}")
    print(f"架构: {arch if not args.arch_file else args.arch_file}")
    print(f"图像尺寸: {image_size}x{image_size}")
    print(f"Epochs: {args.epochs}, Batch: {args.batch_size}, LR: {args.learning_rate}")
    print(f"归一化: {args.normalize}, 数据增强: {args.augmentation}")
    from image_ops import parse_image_ops, apply_pipeline_batch

    image_ops = parse_image_ops(args.image_ops)
    if image_ops:
        print("图像积木: " + " → ".join(step["op"] for step in image_ops))

    keep_classes = parse_keep_classes(args.classes)
    if keep_classes:
        print(f"选用类别: {keep_classes}")
    min_classes = 1 if post == "detect" else 2
    print(f"任务: {'检测' if post == 'detect' else '分类'}（至少 {min_classes} 类）")

    ssd_cfg = None
    calib_x = calib_y = None
    if post == "detect":
        from detect_data import load_detection_pack
        from ssd_train import calib_images_from_samples

        cap = None if int(args.max_samples or 0) <= 0 else int(args.max_samples)
        pack = load_detection_pack(
            dataset, image_size, custom_path=args.custom_path, keep_classes=keep_classes, max_samples=cap,
        )
        input_shape = (image_size, image_size, 3)
        model = create_model(arch, input_shape, pack["num_classes"], post=post)
        print_model(model, device)
        detect_kind = "retinaface" if arch == "retinaface" else "ssd"
        ssd_cfg = dict(getattr(model, "ssd_cfg") or {}) if detect_kind == "ssd" else None
        retina_cfg = dict(getattr(model, "retina_cfg") or {}) if detect_kind == "retinaface" else None
        priors = np.asarray(model.priors, np.float32)
        if ssd_cfg is not None:
            ssd_cfg["num_priors"] = int(priors.shape[0])
            print(f"SSD 锚框数: {priors.shape[0]}")
        else:
            print(f"RetinaFace 锚框数: {priors.shape[0]}  输入 {image_size} RGB letterbox")
        history = fit_detect(
            model,
            pack["train_samples"],
            pack["val_samples"],
            priors,
            image_size,
            num_classes=pack["num_classes"],
            epochs=args.epochs,
            batch_size=args.batch_size,
            lr=args.learning_rate,
            device=device,
            augment=args.augmentation,
            seed=args.seed,
            pipeline=image_ops,
            kind=detect_kind,
        )
        from torch_train import DetectSet, _eval_detect, _loader

        val_loader = _loader(
            DetectSet(pack["val_samples"], priors, image_size, False, args.seed + 1, image_ops, kind=detect_kind),
            args.batch_size, False, device,
        )
        test_loss, test_acc = _eval_detect(model, val_loader, pack["num_classes"], device, kind=detect_kind)
        print(f"\n验证 loc/conf 损失: {test_loss:.4f}  正样本分类准确率: {test_acc:.4f}")
        data = {
            "class_names": pack["class_names"],
            "num_classes": pack["num_classes"],
            "image_size": image_size,
            "channels": 3,
            "custom_path": args.custom_path,
            "kept_classes": pack["class_names"],
        }
        if detect_kind == "retinaface":
            from retina_decode import calib_letterbox_images

            calib_x = calib_letterbox_images(
                pack["val_samples"] or pack["train_samples"], image_size, min(args.calib_samples, 64), pipeline=image_ops,
            )
        else:
            calib_x = calib_images_from_samples(
                pack["val_samples"] or pack["train_samples"], image_size, min(args.calib_samples, 64), pipeline=image_ops,
            )
        calib_y = np.zeros((len(calib_x),), np.int32)
    else:
        data = load_dataset(
            dataset=dataset,
            image_size=image_size,
            normalize=args.normalize,
            custom_path=args.custom_path,
            keep_classes=keep_classes,
            min_classes=min_classes,
        )
        if image_ops:
            data["x_train"] = apply_pipeline_batch(data["x_train"], image_ops)
            data["x_test"] = apply_pipeline_batch(data["x_test"], image_ops)
        input_shape = (data["image_size"], data["image_size"], data["channels"])
        if arch == "simple_cnn" and data["image_size"] > 64:
            print("警告: simple_cnn 在大尺寸输入上参数量会显著增加，建议改用 mobilenet 或 deep_cnn")
        model = create_model(arch, input_shape, data["num_classes"], arch_file=args.arch_file)
        print_model(model, device)
        x_train, y_train = data["x_train"], data["y_train"]
        val_n = int(len(x_train) * args.validation_split)
        if val_n < 1:
            raise ValueError("验证集为空，请减少 --validation-split 或增加样本")
        x_val, y_val = x_train[-val_n:], y_train[-val_n:]
        x_fit, y_fit = x_train[:-val_n], y_train[:-val_n]
        if args.augmentation:
            print("训练时启用数据增强（不会写入导出模型）")
        use_qat = bool(args.qat)
        qat_handles = []
        if use_qat:
            from qat import disable_qat, enable_qat

            qat_handles = enable_qat(model)
            print("测试准确率按量化感知前向计算；保存的权重仍是浮点，导出时再量化并审阅")
        try:
            history = fit_classify(
                model, x_fit, y_fit, x_val, y_val,
                epochs=args.epochs,
                batch_size=args.batch_size,
                lr=args.learning_rate,
                device=device,
                augment=args.augmentation,
                seed=args.seed,
            )
            test_loss, test_acc = evaluate_classify(model, data["x_test"], data["y_test"], args.batch_size, device)
        finally:
            if qat_handles:
                from qat import disable_qat

                disable_qat(qat_handles)
        print(f"\n测试集准确率: {test_acc:.4f}")
        print(f"测试集损失: {test_loss:.4f}")
        calib_x, calib_y = data["x_test"], data["y_test"]

    output_root = Path(args.output_dir) if args.output_dir else MODELS_OUTPUT
    stem = model_stem(dataset, arch)
    run_name = f"{stem}_{now_stamp()}"
    run_dir = ensure_dir(output_root / run_name)

    model_path = run_dir / "model.pt"
    ckpt_meta = {
        "arch": arch,
        "arch_file": args.arch_file,
        "input_shape": list(input_shape),
        "num_classes": int(data["num_classes"]),
        "post": post,
        "framework": "pytorch",
    }
    save_checkpoint(model_path, model, ckpt_meta)
    print(f"PyTorch 模型已保存: {model_path}")

    calib_n = min(args.calib_samples, len(calib_x))
    np.save(run_dir / "calib_images.npy", calib_x[:calib_n])
    np.save(run_dir / "calib_labels.npy", np.asarray(calib_y[:calib_n]))

    if post != "detect":
        _save_test_samples(data["x_test"], data["y_test"], run_dir / "test_samples", args.test_samples)
    if ssd_cfg:
        write_json(run_dir / "ssd_cfg.json", ssd_cfg)
    if locals().get("retina_cfg"):
        write_json(run_dir / "retina_cfg.json", retina_cfg)

    hist = {k: [float(v) for v in vals] for k, vals in history.history.items()}
    class_names = format_class_names(data["class_names"])
    train_info = {
        "name": run_name,
        "pack_stem": stem,
        "dataset": dataset,
        "arch": arch,
        "arch_file": args.arch_file,
        "framework": "pytorch",
        "image_size": data["image_size"],
        "channels": data["channels"],
        "input_shape": list(input_shape),
        "num_classes": data["num_classes"],
        "class_names": class_names,
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "learning_rate": args.learning_rate,
        "normalize": args.normalize,
        "augmentation": args.augmentation,
        "qat": bool(args.qat) and post != "detect",
        "image_ops": image_ops,
        "validation_split": args.validation_split,
        "test_accuracy": float(test_acc),
        "test_loss": float(test_loss),
        "keras_path": str(model_path),
        "model_path": str(model_path),
        "run_dir": str(run_dir),
        "custom_path": data.get("custom_path"),
        "post": post,
        "pixel": (
            "bgr2rgb" if locals().get("detect_kind") == "retinaface"
            else ("bgr" if post == "detect" else ("rgb" if int(data["channels"]) == 3 else "bgr"))
        ),
        "ssd_cfg": ssd_cfg,
        "retina_cfg": locals().get("retina_cfg"),
        "kept_classes": data.get("kept_classes") or class_names,
        "history": hist,
        "params": count_params(model),
    }
    write_json(run_dir / "train_info.json", train_info)
    write_json(run_dir / "class_names.json", {"class_names": class_names})

    update_model_registry({
        "name": run_name,
        "pack_stem": stem,
        "dataset": dataset,
        "arch": arch,
        "test_accuracy": float(test_acc),
        "run_dir": str(run_dir),
        "keras_path": str(model_path),
        "model_path": str(model_path),
        "post": post,
        "created_at": now_stamp(),
    })

    print("\n" + "=" * 60)
    print("训练完成")
    print(f"输出目录: {run_dir}")
    next_quant = "qat" if (args.qat and post != "detect") else "int8"
    print("下一步: python export_ncnn.py --model {} --quantize {}".format(dataset, next_quant))
    print("=" * 60)
    print(
        f"[TRAIN_DONE] acc={float(test_acc):.4f} loss={float(test_loss):.4f} run_dir={run_dir}",
        flush=True,
    )


if __name__ == "__main__":
    main()
