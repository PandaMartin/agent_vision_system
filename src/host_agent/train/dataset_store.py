"""训练数据统一存放在仓库根目录 datasets/ 下。

目录约定：
  datasets/
    README.md
    mnist/            mnist.npz
    fashion_mnist/    idx gzip
    kmnist/           四个 npz
    cifar10/          cifar-10-batches-py/
    cifar100/         cifar-100-python/
    flowers/          flower_photos/
    rps/              rps/
    hands/            数字手势 0-9（类别子文件夹，本地放入）
    coco/             annotations / train2017 / val2017 / test2017
    wider_face/       WIDER FACE 人脸检测（图像 + 框标注）
"""
from __future__ import annotations

import gzip
import os
import pickle
import shutil
import subprocess
import tarfile
import threading
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import requests

from common import DATASETS_DIR, EXTERNAL_COCO_DIR, ensure_dir

COCO_COUNTS = {"train2017": 118287, "val2017": 5000, "test2017": 40670}

DOWNLOADS = {
    "mnist": [
        {
            "filename": "mnist.npz",
            "urls": ["https://storage.googleapis.com/tensorflow/tf-keras-datasets/mnist.npz"],
        }
    ],
    "fashion_mnist": [
        {
            "filename": name,
            "urls": [
                "https://raw.githubusercontent.com/zalandoresearch/fashion-mnist/master/data/fashion/" + name,
                "http://fashion-mnist.s3-website.eu-central-1.amazonaws.com/" + name,
            ],
        }
        for name in (
            "train-labels-idx1-ubyte.gz",
            "train-images-idx3-ubyte.gz",
            "t10k-labels-idx1-ubyte.gz",
            "t10k-images-idx3-ubyte.gz",
        )
    ],
    "kmnist": [
        {
            "filename": name,
            "urls": [
                "https://cdn.jsdelivr.net/gh/dzisandy/Machine-Learning@master/HW3/" + name,
                "https://github.com/dzisandy/Machine-Learning/raw/master/HW3/" + name,
                "http://codh.rois.ac.jp/kmnist/dataset/kmnist/" + name,
            ],
        }
        for name in (
            "kmnist-train-imgs.npz",
            "kmnist-train-labels.npz",
            "kmnist-test-imgs.npz",
            "kmnist-test-labels.npz",
        )
    ],
    "cifar10": [
        {
            "filename": "cifar-10-python.tar.gz",
            "urls": [
                "https://github.com/Digital-Media/cv_data/releases/download/cifar-10/cifar-10-python.tar.gz",
                "https://hf-mirror.com/datasets/MIT-OL-AI-D/cifar-10-python/resolve/main/cifar-10-python.tar.gz",
                "https://huggingface.co/datasets/MIT-OL-AI-D/cifar-10-python/resolve/main/cifar-10-python.tar.gz",
                "https://www.cs.toronto.edu/~kriz/cifar-10-python.tar.gz",
            ],
            "extract": "tar",
        }
    ],
    "cifar100": [
        {
            "filename": "cifar100.tgz",
            "urls": [
                "https://s3.amazonaws.com/fast-ai-imageclas/cifar100.tgz",
                "https://www.cs.toronto.edu/~kriz/cifar-100-python.tar.gz",
            ],
            "extract": "tar",
        }
    ],
    "flowers": [
        {
            "filename": "flower_photos.tgz",
            "urls": [
                "https://storage.googleapis.com/download.tensorflow.org/example_images/flower_photos.tgz"
            ],
            "extract": "tar",
        }
    ],
    "rps": [
        {
            "filename": "rps.zip",
            "urls": ["https://storage.googleapis.com/download.tensorflow.org/data/rps.zip"],
            "extract": "zip",
        }
    ],
}

WIDER_SPLITS = ("train", "val")
WIDER_CLASS_NAMES = ["face"]

COCO_FILES = [
    {
        "filename": "test2017.zip",
        "urls": [
            "http://images.cocodataset.org/zips/test2017.zip",
            "https://hf-mirror.com/datasets/pcuenq/coco-2017-mirror/resolve/main/test2017.zip",
        ],
        "extract": "zip",
        "needed_if": "test2017",
        "count": 40670,
    },
    {
        "filename": "image_info_test2017.zip",
        "urls": ["http://images.cocodataset.org/annotations/image_info_test2017.zip"],
        "extract": "zip",
        "needed_if_missing": "annotations/image_info_test2017.json",
    },
    {
        "filename": "stuff_annotations_trainval2017.zip",
        "urls": ["http://images.cocodataset.org/annotations/stuff_annotations_trainval2017.zip"],
        "extract": "zip",
        "needed_if_missing": "annotations/stuff_train2017.json",
    },
]


def _wider_urls(filename: str) -> list[str]:
    return [
        f"https://hf-mirror.com/datasets/CUHK-CSE/wider_face/resolve/main/data/{filename}",
        f"https://data.brainchip.com/dataset-mirror/widerface/{filename}",
        f"https://huggingface.co/datasets/CUHK-CSE/wider_face/resolve/main/data/{filename}",
        f"https://huggingface.co/datasets/wider_face/resolve/main/data/{filename}",
    ]


WIDER_FILES = [
    {
        "filename": "wider_face_split.zip",
        "urls": _wider_urls("wider_face_split.zip"),
        "extract": "zip",
        "kind": "ann",
    },
    {
        "filename": "WIDER_val.zip",
        "urls": _wider_urls("WIDER_val.zip"),
        "extract": "zip",
        "split": "val",
    },
    {
        "filename": "WIDER_train.zip",
        "urls": _wider_urls("WIDER_train.zip"),
        "extract": "zip",
        "split": "train",
    },
]


def dataset_dir(name: str) -> Path:
    return ensure_dir(DATASETS_DIR / name)


def _link_dir(dest: Path, src: Path) -> None:
    if dest.exists():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    src = src.resolve()
    if os.name == "nt":
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(dest), str(src)],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise OSError(result.stderr.strip() or result.stdout.strip() or "mklink /J 失败")
        return
    dest.symlink_to(src, target_is_directory=True)


def _count_images(folder: Path, limit: int | None = None) -> int:
    if not folder.is_dir():
        return 0
    n = 0
    with os.scandir(folder) as it:
        for ent in it:
            if not ent.is_file():
                continue
            name = ent.name.lower()
            if name.endswith(".jpg") or name.endswith(".jpeg") or name.endswith(".png"):
                n += 1
                if limit is not None and n >= limit:
                    return n
    return n


def ensure_coco_root() -> Path:
    """优先使用已有完整 COCO，避免把 20GB+ 图像再拷一份。"""
    dest = ensure_dir(DATASETS_DIR / "coco")
    src = EXTERNAL_COCO_DIR
    src_ok = src.is_dir() and _split_ready(src, "train2017") and _split_ready(src, "val2017")
    if src_ok:
        (dest / "SOURCE.txt").write_text(
            "COCO 2017 图像与标注已在下列目录（train 118287 / val 5000），"
            "不再重复拷贝约 20GB。\n"
            f"{src}\n"
            "本目录用于补全 test2017 及说明文件。\n",
            encoding="utf-8",
        )
        print(f"使用已有 COCO: {src}")
        return src
    return dest


def _split_ready(root: Path, split: str) -> bool:
    img_dir = root / split
    expect = COCO_COUNTS[split]
    if not img_dir.is_dir():
        return False
    try:
        next(os.scandir(img_dir))
    except StopIteration:
        return False
    if split == "test2017":
        return _count_images(img_dir, limit=expect) >= expect
    ann_name = "instances_val2017.json" if split == "val2017" else "instances_train2017.json"
    return (root / "annotations" / ann_name).is_file()


def coco_status(root: Path | None = None) -> dict:
    root = root or ensure_coco_root()
    status = {"root": str(root)}
    for split, expect in COCO_COUNTS.items():
        ready = _split_ready(root, split)
        status[split] = {"have": expect if ready else _count_images(root / split, limit=8), "expect": expect, "complete": ready}
    anns = root / "annotations"
    status["annotations"] = {
        "instances_train2017": (anns / "instances_train2017.json").is_file(),
        "instances_val2017": (anns / "instances_val2017.json").is_file(),
        "image_info_test2017": (anns / "image_info_test2017.json").is_file(),
        "stuff_train2017": (anns / "stuff_train2017.json").is_file(),
    }
    return status


def _remove_quietly(path: Path) -> None:
    for _ in range(8):
        try:
            if path.exists():
                path.unlink()
            return
        except OSError:
            time.sleep(0.4)


_DL_LOCK = threading.Lock()
_DL_STATE = {"total": 0, "already": 0, "session": 0, "last": 0}


def _range_resume_at(path: Path, start: int, end: int, block: int = 1024 * 1024) -> int:
    """预分配文件里未写入字节为 0。返回该分片下一个待写偏移；写满则返回 end+1。"""
    size = path.stat().st_size
    if size <= start:
        return start
    limit = min(end, size - 1)
    with open(path, "rb") as f:
        pos = limit + 1
        while pos > start:
            chunk_start = max(start, pos - block)
            f.seek(chunk_start)
            data = f.read(pos - chunk_start)
            kept = data.rstrip(b"\x00")
            if kept:
                return chunk_start + len(kept)
            pos = chunk_start
    return start


def _dl_tick(n: int) -> None:
    with _DL_LOCK:
        _DL_STATE["session"] += n
        if _DL_STATE["session"] - _DL_STATE["last"] < 32 * 1024 * 1024:
            return
        _DL_STATE["last"] = _DL_STATE["session"]
        have = _DL_STATE["already"] + _DL_STATE["session"]
        total = _DL_STATE["total"]
        print(f"  {have / 1024 / 1024:.0f} / {total / 1024 / 1024:.0f} MB")


def _download_range(url: str, dest: Path, start: int, end: int, retries: int = 6) -> int:
    """下载 dest[start, end]（含端点），返回本次新写入字节数。"""
    if start > end:
        return 0
    wrote = 0
    cursor = start
    last_err: Exception | None = None
    for attempt in range(retries):
        if cursor > end:
            return wrote
        headers = {
            "User-Agent": "Mozilla/5.0 fpga-nn-assistant/1.0",
            "Range": f"bytes={cursor}-{end}",
        }
        try:
            with requests.get(url, headers=headers, stream=True, timeout=(30, 90), allow_redirects=True) as resp:
                resp.raise_for_status()
                with open(dest, "r+b", buffering=0) as out:
                    out.seek(cursor)
                    for chunk in resp.iter_content(1024 * 1024):
                        if not chunk:
                            continue
                        remain = end + 1 - cursor
                        if remain <= 0:
                            break
                        if len(chunk) > remain:
                            chunk = chunk[:remain]
                        out.write(chunk)
                        n = len(chunk)
                        cursor += n
                        wrote += n
                        _dl_tick(n)
            if cursor > end:
                return wrote
            last_err = IOError(f"分片未写完 {cursor}-{end}")
        except Exception as exc:
            last_err = exc
        time.sleep(min(2 ** attempt, 20))
    raise RuntimeError(f"分片 {start}-{end} 失败") from last_err


def _download_parallel(url: str, dest: Path, total: int, parts: int = 8) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_suffix(dest.suffix + ".part")
    if not part.exists():
        with open(part, "wb") as out:
            out.truncate(total)
    elif part.stat().st_size != total:
        with open(part, "r+b") as out:
            out.truncate(total)

    chunk = total // parts
    jobs = []
    already = 0
    for i in range(parts):
        start = i * chunk
        end = total - 1 if i == parts - 1 else (start + chunk - 1)
        resume = _range_resume_at(part, start, end)
        have = min(end - start + 1, max(0, resume - start))
        already += have
        jobs.append((start, end, resume))
        print(f"  分片 {i + 1}/{parts}: {have / 1024 / 1024:.0f}/{ (end - start + 1) / 1024 / 1024:.0f} MB")

    _DL_STATE.update({"total": total, "already": already, "session": 0, "last": 0})
    print(
        f"分片下载 {dest.name}  {parts} 路  已有 {already / 1024 / 1024:.0f}/{total / 1024 / 1024:.0f} MB  <- {url}"
    )
    todo = [(resume, end) for _start, end, resume in jobs if resume <= end]
    if todo:
        with ThreadPoolExecutor(max_workers=parts) as pool:
            futs = [pool.submit(_download_range, url, part, a, b) for a, b in todo]
            for fut in as_completed(futs):
                fut.result()

    leftover = []
    for start, end, _resume in jobs:
        resume = _range_resume_at(part, start, end)
        if resume <= end:
            leftover.append((resume, end))
    if leftover:
        print(f"补洞 {len(leftover)} 段")
        with ThreadPoolExecutor(max_workers=min(parts, len(leftover))) as pool:
            futs = [pool.submit(_download_range, url, part, a, b) for a, b in leftover]
            for fut in as_completed(futs):
                fut.result()

    if dest.exists():
        _remove_quietly(dest)
    part.replace(dest)
    print(f"完成 {dest} ({dest.stat().st_size / 1024 / 1024:.1f} MB)")
    return dest


def _remote_size(url: str) -> int:
    headers = {"User-Agent": "Mozilla/5.0 fpga-nn-assistant/1.0"}
    try:
        head = requests.head(url, headers=headers, allow_redirects=True, timeout=15)
        total = int(head.headers.get("Content-Length") or 0)
        if head.ok and total > 0:
            return total
    except Exception:
        pass
    probe = requests.get(
        url,
        headers={**headers, "Range": "bytes=0-0"},
        timeout=15,
        allow_redirects=True,
        stream=True,
    )
    try:
        cr = probe.headers.get("Content-Range") or ""
        if "/" in cr:
            return int(cr.rsplit("/", 1)[-1])
        return int(probe.headers.get("Content-Length") or 0)
    finally:
        probe.close()


def _download_one(url: str, dest: Path) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        total = _remote_size(url)
        if total > 80 * 1024 * 1024:
            return _download_parallel(url, dest, total)
    except Exception as exc:
        print(f"探测文件大小失败，改用单连接: {exc}")
    part = dest.with_suffix(dest.suffix + ".part")
    existing = part.stat().st_size if part.exists() else 0
    headers = {"User-Agent": "Mozilla/5.0 fpga-nn-assistant/1.0"}
    if existing:
        headers["Range"] = f"bytes={existing}-"
    print(f"下载 {dest.name}  <- {url}" + (f" （续传 {existing} 字节）" if existing else ""))
    with requests.get(url, headers=headers, stream=True, timeout=(20, 60), allow_redirects=True) as resp:
        resp.raise_for_status()
        ctype = (resp.headers.get("Content-Type") or "").lower()
        if "text/html" in ctype and "octet" not in ctype:
            raise RuntimeError(f"得到的是网页而不是文件: {url}")
        total = resp.headers.get("Content-Length")
        total_n = int(total) + existing if total and total.isdigit() else None
        done = existing
        last_print = done
        with open(part, "ab" if existing else "wb") as out:
            for chunk in resp.iter_content(1024 * 1024):
                if not chunk:
                    continue
                out.write(chunk)
                done += len(chunk)
                if done - last_print >= 8 * 1024 * 1024:
                    if total_n:
                        print(f"  {dest.name}: {done / 1024 / 1024:.0f} / {total_n / 1024 / 1024:.0f} MB")
                    else:
                        print(f"  {dest.name}: {done / 1024 / 1024:.0f} MB")
                    last_print = done
    if dest.exists():
        _remove_quietly(dest)
    part.replace(dest)
    size = dest.stat().st_size
    if size < 64:
        _remove_quietly(dest)
        raise RuntimeError(f"下载结果过小 ({size} 字节): {url}")
    print(f"完成 {dest} ({size / 1024 / 1024:.1f} MB)")
    return dest


def _download_file(urls, dest: Path) -> Path:
    if isinstance(urls, str):
        urls = [urls]
    last_err: Exception | None = None
    for url in urls:
        try:
            return _download_one(url, dest)
        except Exception as exc:
            last_err = exc
            print(f"下载失败 {url}: {exc}（保留 .part 以便续传）")
    raise RuntimeError(f"无法下载 {dest.name}") from last_err


def _extract(archive: Path, dest_dir: Path, mode: str) -> None:
    print(f"解压 {archive.name} -> {dest_dir}")
    dest_dir.mkdir(parents=True, exist_ok=True)
    if mode == "zip":
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(dest_dir)
    elif mode == "tar":
        with tarfile.open(archive, "r:*") as tf:
            tf.extractall(dest_dir)
    else:
        raise ValueError(f"未知解压类型: {mode}")


def fetch_file(subdir: str, filename: str, urls, extract: str | None = None) -> Path:
    folder = dataset_dir(subdir)
    dest = folder / filename
    if not dest.is_file() or dest.stat().st_size < 64:
        _download_file(urls, dest)
    if extract:
        marker = folder / f".extracted_{filename}"
        if not marker.is_file():
            _extract(dest, folder, extract)
            marker.write_text("ok", encoding="utf-8")
    return dest


def prepare_builtin(name: str) -> Path:
    folder = dataset_dir(name)
    for item in DOWNLOADS[name]:
        fetch_file(name, item["filename"], item.get("urls") or item.get("url"), item.get("extract"))
    return folder


def prepare_coco(download_missing: bool = True) -> Path:
    root = ensure_coco_root()
    status = coco_status(root)
    for split, info in ((k, status[k]) for k in COCO_COUNTS):
        flag = "齐全" if info["complete"] else "不完整"
        print(f"COCO {split}: {info['have']}/{info['expect']}  {flag}")

    if not download_missing:
        return root

    if not status["train2017"]["complete"]:
        zip_path = root / "train2017.zip"
        src_zip = EXTERNAL_COCO_DIR / "train2017.zip"
        if src_zip.is_file() and not zip_path.is_file():
            shutil.copy2(src_zip, zip_path)
        if not zip_path.is_file():
            _download_file(
                [
                    "http://images.cocodataset.org/zips/train2017.zip",
                    "https://hf-mirror.com/datasets/pcuenq/coco-2017-mirror/resolve/main/train2017.zip",
                ],
                zip_path,
            )
        _extract(zip_path, root, "zip")

    if not status["val2017"]["complete"]:
        zip_path = root / "val2017.zip"
        src_zip = EXTERNAL_COCO_DIR / "val2017.zip"
        if src_zip.is_file() and not zip_path.is_file():
            shutil.copy2(src_zip, zip_path)
        if not zip_path.is_file():
            _download_file(
                [
                    "http://images.cocodataset.org/zips/val2017.zip",
                    "https://hf-mirror.com/datasets/pcuenq/coco-2017-mirror/resolve/main/val2017.zip",
                ],
                zip_path,
            )
        _extract(zip_path, root, "zip")

    anns = status["annotations"]
    for item in COCO_FILES:
        split = item.get("needed_if")
        missing = item.get("needed_if_missing")
        if split and status.get(split, {}).get("complete"):
            continue
        if missing and (root / missing).is_file():
            continue
        dest = root / item["filename"]
        if not dest.is_file():
            _download_file(item.get("urls") or item.get("url"), dest)
        _extract(dest, root, item["extract"])

    print("COCO 补全后状态:")
    for split, info in coco_status(root).items():
        if split in COCO_COUNTS:
            print(f"  {split}: {info['have']}/{info['expect']}")
    return root


def wider_ann_file(root: Path, split: str) -> Path:
    name = f"wider_face_{split}_bbx_gt.txt"
    direct = root / "wider_face_split" / name
    if direct.is_file():
        return direct
    nested = root / "wider_face_split" / "wider_face_split" / name
    if nested.is_file():
        return nested
    matches = list(root.rglob(name))
    if matches:
        return matches[0]
    raise FileNotFoundError(f"未找到 WIDER FACE 标注 {name}")


def wider_image_root(root: Path, split: str) -> Path:
    cand = root / f"WIDER_{split}" / "images"
    if cand.is_dir():
        return cand
    matches = [p for p in root.rglob("images") if p.is_dir() and f"WIDER_{split}" in str(p)]
    if matches:
        return matches[0]
    raise FileNotFoundError(f"未找到 WIDER FACE 图像目录 WIDER_{split}/images")


def _wider_has_images(folder: Path) -> bool:
    if not folder.is_dir():
        return False
    try:
        for event in folder.iterdir():
            if not event.is_dir():
                continue
            for item in event.iterdir():
                if item.is_file() and item.suffix.lower() in {".jpg", ".jpeg", ".png"}:
                    return True
    except OSError:
        return False
    return False


def _wider_split_ready(root: Path, split: str) -> bool:
    try:
        if not wider_ann_file(root, split).is_file():
            return False
        return _wider_has_images(wider_image_root(root, split))
    except FileNotFoundError:
        return False


def wider_status(root: Path | None = None) -> dict:
    root = root or dataset_dir("wider_face")
    status = {"root": str(root)}
    for split in WIDER_SPLITS:
        status[split] = {"complete": _wider_split_ready(root, split)}
    try:
        status["annotations"] = wider_ann_file(root, "val").is_file() or wider_ann_file(root, "train").is_file()
    except FileNotFoundError:
        status["annotations"] = False
    return status


def prepare_wider_face(download_missing: bool = True) -> Path:
    root = dataset_dir("wider_face")
    status = wider_status(root)
    for split in WIDER_SPLITS:
        flag = "齐全" if status[split]["complete"] else "不完整"
        print(f"WIDER FACE {split}: {flag}")
    if not download_missing:
        return root
    for item in WIDER_FILES:
        split = item.get("split")
        if split and status.get(split, {}).get("complete"):
            continue
        if item.get("kind") == "ann":
            try:
                wider_ann_file(root, "val")
                continue
            except FileNotFoundError:
                try:
                    wider_ann_file(root, "train")
                    continue
                except FileNotFoundError:
                    pass
        dest = root / item["filename"]
        if not dest.is_file() or dest.stat().st_size < 64:
            _download_file(item["urls"], dest)
        marker = root / f".extracted_{item['filename']}"
        if not marker.is_file():
            _extract(dest, root, item["extract"])
            marker.write_text("ok", encoding="utf-8")
        status = wider_status(root)
    print("WIDER FACE 补全后状态:")
    for split in WIDER_SPLITS:
        print(f"  {split}: {'齐全' if wider_status(root)[split]['complete'] else '不完整'}")
    return root


def iter_wider_records(root: Path, split: str, max_samples: int | None = None, min_size: int = 8):
    """解析 WIDER FACE gt：像素框 + 图像路径。invalid 或过小的框丢掉。"""
    ann_path = wider_ann_file(root, split)
    image_root = wider_image_root(root, split)
    lines = ann_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    i = 0
    yielded = 0
    while i < len(lines):
        rel = lines[i].strip().replace("\\", "/")
        i += 1
        if not rel:
            continue
        if i >= len(lines):
            break
        try:
            n_box = int(lines[i].strip())
        except ValueError:
            continue
        i += 1
        boxes: list[list[int]] = []
        for _ in range(max(n_box, 0)):
            if i >= len(lines):
                break
            parts = lines[i].split()
            i += 1
            if len(parts) < 4:
                continue
            x, y, w, h = (int(float(parts[0])), int(float(parts[1])), int(float(parts[2])), int(float(parts[3])))
            invalid = int(parts[7]) if len(parts) > 7 else 0
            if invalid or w < min_size or h < min_size:
                continue
            boxes.append([x, y, w, h])
        if n_box == 0 and i < len(lines):
            peek = lines[i].strip()
            dummy = peek.split()
            if dummy and dummy[0].lstrip("-").isdigit() and not peek.lower().endswith((".jpg", ".jpeg", ".png")):
                i += 1
        if not boxes:
            continue
        path = image_root / rel
        if not path.is_file():
            path = image_root / Path(rel).name
            if not path.is_file():
                continue
        yield path, boxes
        yielded += 1
        if max_samples and yielded >= max_samples:
            return


def prepare_all() -> None:
    ensure_dir(DATASETS_DIR)
    errors: list[str] = []
    for name in DOWNLOADS:
        print("=" * 60)
        print(f"准备 {name}")
        try:
            prepare_builtin(name)
        except Exception as exc:
            errors.append(f"{name}: {exc}")
            print(f"失败 {name}: {exc}")
    print("=" * 60)
    print("准备 coco")
    try:
        prepare_coco(download_missing=True)
    except Exception as exc:
        errors.append(f"coco: {exc}")
        print(f"失败 coco: {exc}")
    print("=" * 60)
    print("准备 wider_face")
    try:
        prepare_wider_face(download_missing=True)
    except Exception as exc:
        errors.append(f"wider_face: {exc}")
        print(f"失败 wider_face: {exc}")
    write_readme()
    print(f"全部数据集目录: {DATASETS_DIR}")
    if errors:
        print("未完成:")
        for item in errors:
            print(f"  - {item}")
        raise SystemExit(1)


def write_readme() -> None:
    text = """# datasets — 训练数据

所有训练用数据都放在这个文件夹，不要再写到 Keras 默认缓存或其它工程目录。

| 子目录 | 内容 |
|---|---|
| `mnist/` | MNIST `mnist.npz` |
| `fashion_mnist/` | Fashion-MNIST idx gzip |
| `kmnist/` | KMNIST 四个 npz |
| `cifar10/` | CIFAR-10 官方 python 包 |
| `cifar100/` | CIFAR-100 官方 python 包 |
| `flowers/` | TensorFlow flower_photos |
| `rps/` | 石头剪刀布 |
| `hands/` | 手势识别（数字 0–9，本地类别文件夹） |
| `coco/` | COCO 2017：`annotations/`、`train2017/`、`val2017/`、`test2017/` |
| `wider_face/` | WIDER FACE 人脸检测：`WIDER_train/`、`WIDER_val/`、`wider_face_split/` |

COCO 的 train/val 图像和实例标注来自 `tflm_pl_accel/data/coco`（目录联接，不重复拷贝）。
test2017 与 stuff / test 标注在本目录补全。

重新下载：

```
python train/download_datasets.py
```
"""
    (DATASETS_DIR / "README.md").write_text(text, encoding="utf-8")


def load_mnist_npz(folder: Path) -> tuple:
    path = folder / "mnist.npz"
    import numpy as np

    with np.load(path, allow_pickle=False) as f:
        return f["x_train"], f["y_train"], f["x_test"], f["y_test"]


def load_fashion_idx(folder: Path) -> tuple:
    import numpy as np

    def _labels(name: str):
        with gzip.open(folder / name, "rb") as fh:
            return np.frombuffer(fh.read(), dtype=np.uint8, offset=8)

    def _images(name: str, n: int):
        with gzip.open(folder / name, "rb") as fh:
            return np.frombuffer(fh.read(), dtype=np.uint8, offset=16).reshape(n, 28, 28)

    y_train = _labels("train-labels-idx1-ubyte.gz")
    y_test = _labels("t10k-labels-idx1-ubyte.gz")
    x_train = _images("train-images-idx3-ubyte.gz", len(y_train))
    x_test = _images("t10k-images-idx3-ubyte.gz", len(y_test))
    return x_train, y_train, x_test, y_test


def load_kmnist_npz(folder: Path) -> tuple:
    import numpy as np

    def _arr(name: str):
        return np.load(folder / name)["arr_0"]

    return (
        _arr("kmnist-train-imgs.npz"),
        _arr("kmnist-train-labels.npz"),
        _arr("kmnist-test-imgs.npz"),
        _arr("kmnist-test-labels.npz"),
    )


def _unpickle(path: Path):
    with open(path, "rb") as fh:
        return pickle.load(fh, encoding="bytes")


def load_cifar10_python(folder: Path) -> tuple:
    import numpy as np

    root = folder / "cifar-10-batches-py"
    if not root.is_dir():
        matches = list(folder.rglob("data_batch_1"))
        if not matches:
            raise FileNotFoundError(f"未找到 CIFAR-10 data_batch_1: {folder}")
        root = matches[0].parent
    xs, ys = [], []
    for i in range(1, 6):
        batch = _unpickle(root / f"data_batch_{i}")
        xs.append(batch[b"data"])
        ys.extend(batch[b"labels"])
    x_train = np.concatenate(xs, axis=0).reshape(-1, 3, 32, 32).transpose(0, 2, 3, 1)
    y_train = np.asarray(ys, dtype=np.int32)
    test = _unpickle(root / "test_batch")
    x_test = test[b"data"].reshape(-1, 3, 32, 32).transpose(0, 2, 3, 1)
    y_test = np.asarray(test[b"labels"], dtype=np.int32)
    return x_train, y_train, x_test, y_test


def load_cifar100_python(folder: Path) -> tuple:
    import numpy as np

    pickle_root = folder / "cifar-100-python"
    if pickle_root.is_dir() and (pickle_root / "train").is_file():
        root = pickle_root
        train = _unpickle(root / "train")
        test = _unpickle(root / "test")
        x_train = train[b"data"].reshape(-1, 3, 32, 32).transpose(0, 2, 3, 1)
        y_train = np.asarray(train[b"fine_labels"], dtype=np.int32)
        x_test = test[b"data"].reshape(-1, 3, 32, 32).transpose(0, 2, 3, 1)
        y_test = np.asarray(test[b"fine_labels"], dtype=np.int32)
        return x_train, y_train, x_test, y_test

    train_dir = find_class_folder(folder, "train")
    test_dir = find_class_folder(folder, "test")
    if _has_class_images(train_dir) and _has_class_images(test_dir):
        x_train, y_train, names = load_class_folder_arrays(train_dir, 32)
        x_test, y_test, test_names = load_class_folder_arrays(test_dir, 32)
        if names != test_names:
            print("警告: CIFAR-100 train/test 类别文件夹名称不一致，按训练集顺序对齐")
        return x_train, y_train, x_test, y_test
    raise FileNotFoundError(f"未找到 CIFAR-100 数据: {folder}")


def _has_class_images(folder: Path) -> bool:
    if not folder.is_dir():
        return False
    return any(p.is_dir() for p in folder.iterdir())


def load_class_folder_arrays(root: Path, image_size: int) -> tuple:
    from PIL import Image
    import numpy as np

    class_dirs = sorted([d for d in root.iterdir() if d.is_dir() and not d.name.startswith(".")])
    if not class_dirs:
        raise FileNotFoundError(f"没有类别子文件夹: {root}")
    names = [d.name for d in class_dirs]
    images = []
    labels = []
    for idx, class_dir in enumerate(class_dirs):
        files = [p for p in class_dir.iterdir() if p.suffix.lower() in {".jpg", ".jpeg", ".png", ".bmp"}]
        for path in files:
            with Image.open(path) as img:
                arr = np.asarray(img.convert("RGB").resize((image_size, image_size)), dtype=np.uint8)
            images.append(arr)
            labels.append(idx)
    if not images:
        raise ValueError(f"没有图像: {root}")
    return np.stack(images, axis=0), np.asarray(labels, dtype=np.int32), names


def find_class_folder(folder: Path, inner: str) -> Path:
    named = folder / inner
    if named.is_dir():
        return named
    matches = [p for p in folder.rglob(inner) if p.is_dir()]
    if matches:
        return matches[0]
    return folder
