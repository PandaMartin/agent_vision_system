"""把全部训练数据下载到仓库根目录 datasets/。

  python train/download_datasets.py
  python train/download_datasets.py --only coco
  python train/download_datasets.py --only wider_face
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from dataset_store import (
    DATASETS_DIR,
    DOWNLOADS,
    coco_status,
    find_class_folder,
    prepare_all,
    prepare_builtin,
    prepare_coco,
    prepare_wider_face,
    wider_status,
    write_readme,
)


def main():
    parser = argparse.ArgumentParser(description="下载训练数据集到 datasets/")
    parser.add_argument(
        "--only",
        default=None,
        help="只准备某一个：mnist / fashion_mnist / kmnist / cifar10 / cifar100 / flowers / rps / hands / coco / wider_face",
    )
    args = parser.parse_args()
    if not args.only:
        prepare_all()
        return
    name = args.only.strip().lower()
    if name == "coco":
        prepare_coco(download_missing=True)
        print(coco_status())
    elif name in {"wider_face", "widerface", "wider", "face", "faces"}:
        prepare_wider_face(download_missing=True)
        print(wider_status())
    elif name in {"hands", "hand", "gesture", "gestures"}:
        root = find_class_folder(DATASETS_DIR / "hands", "hands")
        if not root.is_dir():
            raise SystemExit("未找到 datasets/hands/，请把数字手势类别文件夹放到该目录")
        print(f"手势识别是本地数据集: {root}")
    elif name in DOWNLOADS:
        prepare_builtin(name)
    else:
        raise SystemExit(f"未知数据集: {name}")
    write_readme()


if __name__ == "__main__":
    main()
