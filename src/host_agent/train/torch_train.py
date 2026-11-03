"""PyTorch 分类 / SSD 训练循环，打印格式与上位机进度条兼容。"""
from __future__ import annotations

import math
import time

import numpy as np
import torch
from torch import nn
from torch.utils.data import DataLoader, Dataset


class History:
    def __init__(self):
        self.history = {
            "loss": [],
            "accuracy": [],
            "val_loss": [],
            "val_accuracy": [],
        }


class _Progress:
    def __init__(self, total_epochs: int):
        self.total_epochs = total_epochs
        self._t = 0.0
        self.epoch = 0

    def emit(self, logs: dict, batch=None, steps=None):
        parts = ["[TRAIN]", f"epoch={self.epoch + 1}/{self.total_epochs}"]
        if batch is not None and steps:
            parts.append(f"step={int(batch) + 1}/{steps}")

        def _fmt(key, tag):
            raw = logs.get(key)
            if raw is None:
                return
            try:
                value = float(raw)
            except (TypeError, ValueError):
                return
            if not math.isfinite(value):
                return
            parts.append(f"{tag}={value:.4f}")

        _fmt("loss", "loss")
        _fmt("accuracy", "acc")
        _fmt("val_loss", "val_loss")
        _fmt("val_accuracy", "val_acc")
        print(" ".join(parts), flush=True)

    def maybe_step(self, logs, batch, steps):
        last = bool(steps) and (int(batch) + 1) >= steps
        if last:
            return
        every = max(1, (steps or 20) // 20)
        now = time.time()
        if (int(batch) + 1) % every != 0 and (now - self._t) < 1.5:
            return
        self._t = now
        self.emit(logs, batch=batch, steps=steps)


class NHWCClassifySet(Dataset):
    def __init__(self, x: np.ndarray, y: np.ndarray, augment: bool = False, seed: int = 42):
        self.x = np.asarray(x, np.float32)
        self.y = np.asarray(y).reshape(-1).astype(np.int64)
        self.augment = augment
        self.seed = seed
        self.channels = int(self.x.shape[-1])

    def __len__(self):
        return len(self.x)

    def __getitem__(self, index):
        img = self.x[index]
        if self.augment:
            rng = np.random.default_rng((self.seed * 1000003 + int(index)) & 0xFFFFFFFF)
            img = _augment_nhwc(img, rng, self.channels)
        t = torch.from_numpy(np.ascontiguousarray(img)).permute(2, 0, 1)
        return t, int(self.y[index])


class DetectSet(Dataset):
    def __init__(self, samples, priors, image_size, augment, seed, pipeline=None, kind="ssd"):
        self.samples = samples
        self.priors = priors
        self.image_size = image_size
        self.augment = augment
        self.seed = seed
        self.pipeline = pipeline
        self.kind = str(kind or "ssd")
        self.epoch = 0

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, index):
        # 每个 epoch 换种子，否则翻转/裁剪每轮完全相同，50 轮等于背同一批增强图。
        rng = np.random.default_rng(
            (int(self.seed) * 1000003 + int(index) * 9176 + int(self.epoch) * 104729) & 0xFFFFFFFF
        )
        if self.kind == "retinaface":
            from retina_priors import encode_retina_sample

            image, loc_t, conf_t = encode_retina_sample(
                self.samples[index], self.priors, self.image_size, self.augment, rng, pipeline=self.pipeline,
            )
        else:
            from detect_data import encode_sample

            image, loc_t, conf_t = encode_sample(
                self.samples[index], self.priors, self.image_size, self.augment, rng, pipeline=self.pipeline,
            )
        x = torch.from_numpy(np.ascontiguousarray(image)).permute(2, 0, 1)
        return x, torch.from_numpy(loc_t.astype(np.float32)), torch.from_numpy(conf_t.astype(np.int64))


def _augment_nhwc(img: np.ndarray, rng: np.random.Generator, channels: int) -> np.ndarray:
    out = img
    if channels == 3 and rng.random() < 0.5:
        out = out[:, ::-1, :].copy()
    if rng.random() < 0.8:
        dy = int(rng.integers(-2, 3))
        dx = int(rng.integers(-2, 3))
        if dy or dx:
            out = np.roll(out, (dy, dx), axis=(0, 1))
    return out


def _loader(dataset: Dataset, batch_size: int, shuffle: bool, device) -> DataLoader:
    pin = device.type == "cuda"
    return DataLoader(
        dataset,
        batch_size=batch_size,
        shuffle=shuffle,
        num_workers=0,
        pin_memory=pin,
        drop_last=False,
    )


def decode_ssd(outs, num_classes: int):
    if not isinstance(outs, (list, tuple)):
        outs = [outs]
    n = len(outs) // 2
    locs, confs = [], []
    for loc, conf in zip(outs[:n], outs[n:]):
        b = loc.shape[0]
        locs.append(loc.permute(0, 2, 3, 1).reshape(b, -1, 4))
        confs.append(conf.permute(0, 2, 3, 1).reshape(b, -1, num_classes + 1))
    return torch.cat(locs, dim=1), torch.cat(confs, dim=1)


def decode_retina(outs, num_classes: int = 1):
    """9 头 (cls,bbox,lmk)×3 → loc(N,P,4) + logits(N,P,2)。cls 布局 [bg0,bg1,face0,face1]。"""
    if not isinstance(outs, (list, tuple)):
        outs = [outs]
    locs, confs = [], []
    for i in range(0, len(outs), 3):
        cls, box = outs[i], outs[i + 1]
        b, _, h, w = box.shape
        box = box.permute(0, 2, 3, 1).reshape(b, h * w, 2, 4).reshape(b, -1, 4)
        cls = cls.permute(0, 2, 3, 1).reshape(b, h * w, 4)
        a0 = torch.stack([cls[..., 0], cls[..., 2]], dim=-1)
        a1 = torch.stack([cls[..., 1], cls[..., 3]], dim=-1)
        conf = torch.stack([a0, a1], dim=2).reshape(b, -1, 2)
        locs.append(box)
        confs.append(conf)
    return torch.cat(locs, dim=1), torch.cat(confs, dim=1)


def ssd_losses(loc_p, conf_p, loc_t, conf_t, loc_weight=1.0, neg_pos_ratio=3.0):
    conf_t = conf_t.long()
    pos = conf_t > 0
    pos_f = pos.float()
    num_pos = pos_f.sum(dim=1)
    loc_err = loc_p - loc_t
    abs_err = loc_err.abs()
    smooth = torch.where(abs_err < 1.0, 0.5 * loc_err * loc_err, abs_err - 0.5)
    loc_loss = (smooth.sum(dim=-1) * pos_f).sum(dim=1)
    loc_loss = (loc_loss / num_pos.clamp(min=1.0)).mean()

    b, a, c = conf_p.shape
    ce = nn.functional.cross_entropy(
        conf_p.reshape(-1, c), conf_t.reshape(-1), reduction="none",
    ).view(b, a)
    pos_ce = ce * pos_f
    neg = conf_t == 0
    neg_ce = torch.where(neg, ce, torch.zeros_like(ce))
    num_neg = torch.minimum(neg.float().sum(dim=1), (num_pos * neg_pos_ratio).clamp(min=3.0))
    k = int(num_neg.max().item()) if num_neg.numel() else 1
    k = max(k, 1)
    topk, _ = torch.topk(neg_ce, k, dim=1)
    idx = torch.arange(k, device=neg_ce.device, dtype=torch.float32)[None, :]
    mask = idx < num_neg[:, None]
    neg_loss = (topk * mask.float()).sum(dim=1)
    conf_loss = ((pos_ce.sum(dim=1) + neg_loss) / num_pos.clamp(min=1.0)).mean()
    loss = conf_loss + loc_weight * loc_loss
    pred = conf_p.argmax(dim=-1)
    acc = ((pred == conf_t) & pos).float().sum() / pos_f.sum().clamp(min=1.0)
    acc = torch.nan_to_num(acc, nan=0.0, posinf=0.0, neginf=0.0)
    loss = torch.nan_to_num(loss, nan=0.0)
    return loss, loc_loss.detach(), conf_loss.detach(), acc.detach()


def fit_classify(
    model,
    x_fit, y_fit, x_val, y_val,
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    device,
    augment: bool,
    seed: int,
):
    train_loader = _loader(NHWCClassifySet(x_fit, y_fit, augment=augment, seed=seed), batch_size, True, device)
    val_loader = _loader(NHWCClassifySet(x_val, y_val, augment=False), batch_size, False, device)
    model.to(device)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    sched = torch.optim.lr_scheduler.ReduceLROnPlateau(opt, factor=0.5, patience=3)
    crit = nn.CrossEntropyLoss()
    prog = _Progress(epochs)
    hist = History()
    best_state = None
    best_val = float("inf")
    steps = max(1, len(train_loader))

    for epoch in range(epochs):
        prog.epoch = epoch
        prog._t = 0.0
        model.train()
        run_loss = run_acc = n = 0.0
        for batch, (xb, yb) in enumerate(train_loader):
            xb = xb.to(device, non_blocking=True)
            yb = yb.to(device, non_blocking=True)
            opt.zero_grad(set_to_none=True)
            logits = model(xb)
            loss = crit(logits, yb)
            loss.backward()
            opt.step()
            pred = logits.argmax(dim=1)
            acc = (pred == yb).float().mean()
            bs = float(yb.size(0))
            run_loss += float(loss.item()) * bs
            run_acc += float(acc.item()) * bs
            n += bs
            logs = {"loss": run_loss / max(n, 1.0), "accuracy": run_acc / max(n, 1.0)}
            prog.maybe_step(logs, batch, steps)
        val_loss, val_acc = _eval_classify(model, val_loader, crit, device)
        logs = {
            "loss": run_loss / max(n, 1.0),
            "accuracy": run_acc / max(n, 1.0),
            "val_loss": val_loss,
            "val_accuracy": val_acc,
        }
        prog.emit(logs, batch=steps - 1, steps=steps)
        hist.history["loss"].append(logs["loss"])
        hist.history["accuracy"].append(logs["accuracy"])
        hist.history["val_loss"].append(val_loss)
        hist.history["val_accuracy"].append(val_acc)
        sched.step(val_loss)
        if val_loss + 1e-6 < best_val:
            best_val = val_loss
            best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
    if best_state is not None:
        model.load_state_dict(best_state)
        print(f"已恢复验证损失最低的权重（val_loss={best_val:.4f}）", flush=True)
    return hist


def evaluate_classify(model, x, y, batch_size, device):
    loader = _loader(NHWCClassifySet(x, y, augment=False), batch_size, False, device)
    crit = nn.CrossEntropyLoss()
    return _eval_classify(model, loader, crit, device)


@torch.no_grad()
def _eval_classify(model, loader, crit, device):
    model.eval()
    loss_sum = acc_sum = n = 0.0
    for xb, yb in loader:
        xb = xb.to(device, non_blocking=True)
        yb = yb.to(device, non_blocking=True)
        logits = model(xb)
        loss = crit(logits, yb)
        acc = (logits.argmax(dim=1) == yb).float().mean()
        bs = float(yb.size(0))
        loss_sum += float(loss.item()) * bs
        acc_sum += float(acc.item()) * bs
        n += bs
    return loss_sum / max(n, 1.0), acc_sum / max(n, 1.0)


def _cosine_with_warmup(epochs: int, warmup: int):
    warmup = max(0, min(int(warmup), max(int(epochs) - 1, 0)))

    def lr_lambda(epoch: int):
        if warmup and epoch < warmup:
            return float(epoch + 1) / float(warmup)
        progress = (epoch - warmup) / max(1, int(epochs) - warmup)
        return 0.05 + 0.95 * 0.5 * (1.0 + math.cos(math.pi * progress))

    return lr_lambda


def fit_detect(
    model,
    train_samples, val_samples, priors, image_size,
    *,
    num_classes: int,
    epochs: int,
    batch_size: int,
    lr: float,
    device,
    augment: bool,
    seed: int,
    pipeline=None,
    kind: str = "ssd",
):
    kind = str(kind or "ssd")
    decode = decode_retina if kind == "retinaface" else decode_ssd
    train_set = DetectSet(train_samples, priors, image_size, augment, seed, pipeline, kind=kind)
    train_loader = _loader(train_set, batch_size, True, device)
    val_loader = _loader(
        DetectSet(val_samples, priors, image_size, False, seed + 1, pipeline, kind=kind),
        batch_size, False, device,
    )
    model.to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-4)
    warmup = max(1, min(5, int(epochs) // 10))
    sched = torch.optim.lr_scheduler.LambdaLR(opt, _cosine_with_warmup(epochs, warmup))
    prog = _Progress(epochs)
    hist = History()
    best_state = None
    best_val = float("inf")
    best_epoch = 0
    best_acc = 0.0
    stale = 0
    steps = max(1, len(train_loader))

    for epoch in range(epochs):
        train_set.epoch = epoch
        prog.epoch = epoch
        prog._t = 0.0
        model.train()
        run_loss = run_acc = n = 0.0
        for batch, (xb, loc_t, conf_t) in enumerate(train_loader):
            xb = xb.to(device, non_blocking=True)
            loc_t = loc_t.to(device, non_blocking=True)
            conf_t = conf_t.to(device, non_blocking=True)
            opt.zero_grad(set_to_none=True)
            loc_p, conf_p = decode(model(xb), num_classes)
            loss, _, _, acc = ssd_losses(loc_p, conf_p, loc_t, conf_t)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 10.0)
            opt.step()
            bs = float(xb.size(0))
            run_loss += float(loss.item()) * bs
            run_acc += float(acc.item()) * bs
            n += bs
            logs = {"loss": run_loss / max(n, 1.0), "accuracy": run_acc / max(n, 1.0)}
            prog.maybe_step(logs, batch, steps)
        val_loss, val_acc = _eval_detect(model, val_loader, num_classes, device, kind=kind)
        logs = {
            "loss": run_loss / max(n, 1.0),
            "accuracy": run_acc / max(n, 1.0),
            "val_loss": val_loss,
            "val_accuracy": val_acc,
        }
        prog.emit(logs, batch=steps - 1, steps=steps)
        hist.history["loss"].append(logs["loss"])
        hist.history["accuracy"].append(logs["accuracy"])
        hist.history["val_loss"].append(val_loss)
        hist.history["val_accuracy"].append(val_acc)
        sched.step()
        if val_loss + 1e-6 < best_val:
            best_val = val_loss
            best_acc = val_acc
            best_epoch = epoch + 1
            stale = 0
            best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
        else:
            stale += 1
            if stale == 5:
                print(
                    f"验证损失已连续回升（当前 {val_loss:.4f} / 最佳 {best_val:.4f} @ epoch {best_epoch}）。"
                    "将跑完指定轮数，结束时恢复最佳权重。",
                    flush=True,
                )
    if best_state is not None:
        model.load_state_dict(best_state)
        print(
            f"已恢复验证损失最低的权重（epoch={best_epoch}, val_loss={best_val:.4f}, val_acc={best_acc:.4f}）",
            flush=True,
        )
    return hist


@torch.no_grad()
def _eval_detect(model, loader, num_classes, device, kind: str = "ssd"):
    model.eval()
    decode = decode_retina if str(kind or "ssd") == "retinaface" else decode_ssd
    loss_sum = acc_sum = n = 0.0
    for xb, loc_t, conf_t in loader:
        xb = xb.to(device, non_blocking=True)
        loc_t = loc_t.to(device, non_blocking=True)
        conf_t = conf_t.to(device, non_blocking=True)
        loc_p, conf_p = decode(model(xb), num_classes)
        loss, _, _, acc = ssd_losses(loc_p, conf_p, loc_t, conf_t)
        bs = float(xb.size(0))
        loss_sum += float(loss.item()) * bs
        acc_sum += float(acc.item()) * bs
        n += bs
    return loss_sum / max(n, 1.0), acc_sum / max(n, 1.0)
