"""按上位机 model.cfg 生成板上 model.mod 源码，并在工具链可用时封包。

板上 18_cam_lcd_hdmi_servo_net 不读 model.cfg，也不读 image_ops.cfg。
预处理、图像积木、blob、解码都写进 model.mod。积木在进网络之前对整帧 RGB 执行，
工作区在模块区后面的空闲 DDR（0x21400000，8MB）。
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

from board_model import (
    CFG_NAME,
    MOD_NAME,
    board_folder_for_pack,
    board_folder_name,
    parse_model_cfg,
    public_board_status,
    retire_long_sd_dir,
    write_sd_four_files,
)

DEFAULT_FPGA = Path(
    r"F:\AWork\18_cam_lcd_hdmi_servo_net\18_cam_lcd_hdmi_servo_net"
    r"\vitis\cam_lcd_hdmi_servo_net"
)
_TOOLCHAIN_CANDIDATES = (
    r"D:\AMDDesignTools\2025.2\gnu\aarch32\nt\gcc-arm-none-eabi\bin\arm-none-eabi-",
    r"D:\software\AMDDesign\2025.2\Vitis\gnu\aarch32\nt\gcc-arm-none-eabi\bin\arm-none-eabi-",
)


def _repo_toolchain_prefix() -> str:
    from repo_paths import project_root

    root = project_root() / "prebuilt" / "gcc-arm-none-eabi" / "bin"
    return str(root / "arm-none-eabi-")


def resolve_toolchain(explicit: str | None = None) -> str | None:
    raws = []
    if explicit:
        raws.append(explicit)
    raws.append(_repo_toolchain_prefix())
    env = (os.environ.get("FPGA_TOOLCHAIN") or "").strip()
    if env:
        raws.append(env)
    raws.extend(_TOOLCHAIN_CANDIDATES)
    for raw in raws:
        prefix = raw
        if prefix.lower().endswith("g++.exe"):
            prefix = prefix[: -len("g++.exe")]
        elif prefix.lower().endswith("g++"):
            prefix = prefix[: -len("g++")]
        exe = prefix + "g++.exe"
        if Path(exe).is_file():
            return prefix
    return None


def resolve_fpga_root(explicit: str | Path | None = None) -> Path | None:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    env = (os.environ.get("FPGA_PROJECT") or "").strip()
    if env:
        candidates.append(Path(env))
    cfg = Path.home() / ".fpga_deploy" / "config.json"
    if cfg.is_file():
        try:
            data = json.loads(cfg.read_text(encoding="utf-8"))
            raw = str(data.get("fpga_project") or "").strip()
            if raw:
                candidates.append(Path(raw))
        except Exception:
            pass
    candidates.append(DEFAULT_FPGA)
    for path in candidates:
        if (path / "tools" / "nnmod" / "nnmod.py").is_file():
            return path
    from repo_paths import project_root

    bundled = project_root()
    if (bundled / "tools" / "nnmod" / "nnmod.py").is_file() and (bundled / "src" / "sw").is_dir():
        return bundled
    return None


def _load_steps(folder: Path, extra: dict) -> list[dict]:
    """训练时的积木链。train_info / deployment 里有 image_ops 就用它，否则读 image_ops.cfg。"""
    from image_ops import normalize_steps

    if isinstance(extra.get("image_ops"), list):
        return normalize_steps(extra["image_ops"])
    path = folder / "image_ops.cfg"
    if not path.is_file():
        return []
    steps: list[dict] = []
    current: dict | None = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        text = line.split("#", 1)[0].strip()
        if "=" not in text:
            continue
        key, val = text.split("=", 1)
        key = key.strip().replace(" ", "")
        val = val.strip()
        if key.startswith("op") and key[2:].isdigit():
            current = {"op": val, "params": {}}
            steps.append(current)
            continue
        if current is not None and "." in key:
            index, field = key.split(".", 1)
            if index == f"op{len(steps) - 1}" and field != "name":
                current["params"][field] = val
    return normalize_steps(steps)


_STAGE_PLAIN = "const NnImage* im = img;"
_STAGE_OPS = """unsigned char* scratch = 0;
    if (apply_board_ops(img->pix, img->w, img->h, img->fmt == NN_PIX_RGB, &scratch) != 0)
        return -10;
    NnImage staged = *img;
    staged.pix = scratch;
    staged.fmt = NN_PIX_RGB;
    const NnImage* im = &staged;"""


def _apply_stage(src: str, steps: list[dict]) -> str:
    from board_ops_cxx import render_ops_cxx

    if "@STAGE@" not in src:
        raise ValueError("预处理模板缺少积木插入点")
    src = src.replace("@STAGE@", _STAGE_OPS if steps else _STAGE_PLAIN)
    if not steps:
        return src
    ops = render_ops_cxx(steps)
    needle = "const char* MOD_NAME"
    at = src.find(needle)
    if at < 0:
        raise ValueError("模块头缺少 MOD_NAME")
    nl = src.find("\n", at)
    return src[: nl + 1] + "\n" + ops + "\n" + src[nl + 1 :]


def _sidecar(folder: Path) -> dict:
    extra: dict = {}
    for name in ("train_info.json", "deployment_config.json"):
        path = folder / name
        if not path.is_file():
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        if isinstance(data, dict):
            extra.update(data)
    return extra


def _family(post: str, arch: str) -> str:
    post = (post or "").lower()
    arch = (arch or "").lower()
    if post == "retinaface" or "retina" in arch:
        return "retinaface"
    if post == "detect" or "ssd" in arch:
        return "ssd"
    return "classify"


def _c_float(value) -> str:
    text = f"{float(value):.8g}"
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def _floats(values, n: int) -> str:
    seq = list(values or [])
    if len(seq) < n:
        pad = seq[-1] if seq else 0.0
        seq = (seq + [pad] * n)[:n]
    return ", ".join(_c_float(v) for v in seq[:n])


def _pixel_cxx(pixel: str, channels: int) -> str:
    if int(channels) == 1:
        return (
        "const int ptype = (im->fmt == NN_PIX_BGR) ? ncnn::Mat::PIXEL_BGR2GRAY\n"
        "                                               : ncnn::Mat::PIXEL_RGB2GRAY;"
        )
    if (pixel or "").lower() == "bgr":
        return (
        "const int ptype = (im->fmt == NN_PIX_BGR) ? ncnn::Mat::PIXEL_BGR\n"
        "                                               : ncnn::Mat::PIXEL_RGB2BGR;"
        )
    return (
        "const int ptype = (im->fmt == NN_PIX_BGR) ? ncnn::Mat::PIXEL_BGR2RGB\n"
        "                                               : ncnn::Mat::PIXEL_RGB;"
    )


def _c_string(text: str) -> str:
    raw = "".join(ch for ch in str(text or "model") if ch.isascii() and (ch.isalnum() or ch == "_"))
    return raw[:20] or "model"


def _common_head(name: str, note: str) -> str:
    return f"""// generated by train/board_mod.py — {note}
// board contract: preprocess + declared blobs + decode live in this module.
#include "sw/impl/nn/nn_pipeline.h"
#include <string.h>
#include "mat.h"
extern "C" {{
int xil_printf(const char*, ...);
float expf(float);
}}
namespace {{
const char* MOD_NAME = "{_c_string(name)}";
"""


def _boot(tag: str, blob_in: int, outs: str, roles: str, out_n: int) -> str:
    return f"""
const NnModelDef g_def = {{
    NN_PIPELINE_ABI_VER,
    MOD_NAME,
    "{tag}",
    {int(blob_in)},
    {int(out_n)}, {outs},
    {roles},
    preprocess,
    decode,
}};
}} // namespace
extern "C" __attribute__((section(".mod_entry_first")))
const NnModelDef* nn_mod_boot(void)
{{
    return &g_def;
}}
"""


def _stretch_preprocess(channels: int, pixel: str, in_size: int) -> str:
    ch = int(channels)
    return f"""
const int IN_SIZE = {int(in_size)};
const int IN_CH = {ch};
float s_in[IN_SIZE * IN_SIZE * IN_CH];
const float MEAN[IN_CH] = {{{_floats([], ch)}}};
const float NORM[IN_CH] = {{{_floats([], ch)}}};

int preprocess(const NnImage* img, NnTensor* in, NnGeom* geom)
{{
    if (img == 0 || img->pix == 0 || img->w <= 0 || img->h <= 0)
        return -1;
    @STAGE@
    {_pixel_cxx(pixel, ch)}
    ncnn::Mat m = ncnn::Mat::from_pixels_resize(im->pix, ptype, im->w, im->h, IN_SIZE, IN_SIZE);
    m.substract_mean_normalize(MEAN, NORM);
    memcpy(s_in, m.data, (size_t)IN_SIZE * IN_SIZE * IN_CH * sizeof(float));
    m.release();
    in->data = s_in;
    in->w = IN_SIZE;
    in->h = IN_SIZE;
    in->c = IN_CH;
    geom->scale = 1.f;
    geom->pad_x = 0;
    geom->pad_y = 0;
    geom->src_w = im->w;
    geom->src_h = im->h;
    return 0;
}}
"""


def render_classify(cfg: dict, name: str) -> str:
    ch = int(cfg.get("channels") or 3)
    size = int(cfg.get("image_size") or cfg.get("input_size") or 224)
    topk = max(1, min(5, int(cfg.get("topk") or 5)))
    mean = _floats(cfg.get("mean"), ch)
    norm = _floats(cfg.get("norm"), ch)
    body = _stretch_preprocess(ch, str(cfg.get("pixel") or "rgb"), size)
    body = body.replace("const float MEAN[IN_CH] = {" + _floats([], ch) + "};",
                        "const float MEAN[IN_CH] = {" + mean + "};", 1)
    body = body.replace("const float NORM[IN_CH] = {" + _floats([], ch) + "};",
                        "const float NORM[IN_CH] = {" + norm + "};", 1)
    return _common_head(name, "classify") + body + f"""
const int BLOB_IN = {int(cfg.get("blob_in") or 0)};
const int BLOB_OUT = {int(cfg.get("blob_out") or 0)};
const int TOPK = {topk};

int decode(const NnTensor* outs, int out_n, const NnGeom* geom, NnResult* res)
{{
    (void)geom;
    res->box_n = 0;
    res->topk_n = 0;
    if (out_n < 1 || outs[0].data == 0)
        return -1;
    const int n = outs[0].w * outs[0].h * outs[0].c;
    if (n <= 0)
        return -2;
    const float* p = outs[0].data;
    int idx[5];
    float sc[5];
    int k = 0;
    for (int i = 0; i < n; i++)
    {{
        const float s = p[i];
        int pos = k;
        while (pos > 0 && s > sc[pos - 1])
            pos--;
        if (pos >= TOPK)
            continue;
        const int last = (k < TOPK) ? k : (TOPK - 1);
        for (int j = last; j > pos; j--)
        {{
            sc[j] = sc[j - 1];
            idx[j] = idx[j - 1];
        }}
        sc[pos] = s;
        idx[pos] = i;
        if (k < TOPK)
            k++;
    }}
    res->topk_n = k;
    for (int i = 0; i < k; i++)
    {{
        res->topk_label[i] = idx[i];
        res->topk_score[i] = sc[i];
    }}
    return 0;
}}
""" + _boot("分类", int(cfg.get("blob_in") or 0), "{ BLOB_OUT }", "{ NN_ROLE_PROB }", 1)


def render_ssd(cfg: dict, name: str) -> str:
    size = int(cfg.get("image_size") or cfg.get("input_size") or 300)
    score = float(cfg.get("score") or 0.5)
    nms = float(cfg.get("nms_iou_th") or 0.45)
    keep = max(1, min(32, int(cfg.get("max_keep") or 32)))
    mean = _floats(cfg.get("mean") or (127.5, 127.5, 127.5), 3)
    norm = _floats(cfg.get("norm") or (1.0 / 127.5, 1.0 / 127.5, 1.0 / 127.5), 3)
    pre = _stretch_preprocess(3, str(cfg.get("pixel") or "bgr"), size)
    pre = pre.replace("const float MEAN[IN_CH] = {" + _floats([], 3) + "};",
                      "const float MEAN[IN_CH] = {" + mean + "};", 1)
    pre = pre.replace("const float NORM[IN_CH] = {" + _floats([], 3) + "};",
                      "const float NORM[IN_CH] = {" + norm + "};", 1)
    return _common_head(name, "ssd") + pre + f"""
const int BLOB_IN = {int(cfg.get("blob_in") or 0)};
const int BLOB_OUT = {int(cfg.get("blob_out") or 0)};
const float SCORE_TH = {score:.8g}f;
const float NMS_IOU = {nms:.8g}f;
const int CAND_MAX = 64;
const int KEEP_MAX = {keep};
NnDetBox s_cand[CAND_MAX];
NnDetBox s_keep[KEEP_MAX];

inline int clip_i(int v, int lo, int hi)
{{
    return v < lo ? lo : (v > hi ? hi : v);
}}
inline void cand_push(NnDetBox* arr, int* n, const NnDetBox& b)
{{
    if (*n < CAND_MAX) {{ arr[(*n)++] = b; return; }}
    int worst = 0;
    for (int i = 1; i < CAND_MAX; i++)
        if (arr[i].score < arr[worst].score) worst = i;
    if (b.score > arr[worst].score) arr[worst] = b;
}}
inline float box_iou(const NnDetBox& a, const NnDetBox& b)
{{
    const int ix0 = a.x0 > b.x0 ? a.x0 : b.x0;
    const int iy0 = a.y0 > b.y0 ? a.y0 : b.y0;
    const int ix1 = a.x1 < b.x1 ? a.x1 : b.x1;
    const int iy1 = a.y1 < b.y1 ? a.y1 : b.y1;
    const int iw = ix1 - ix0;
    const int ih = iy1 - iy0;
    if (iw <= 0 || ih <= 0) return 0.f;
    const int inter = iw * ih;
    const int area1 = (a.x1 - a.x0) * (a.y1 - a.y0);
    const int area2 = (b.x1 - b.x0) * (b.y1 - b.y0);
    return (float)inter / (float)(area1 + area2 - inter);
}}

int decode(const NnTensor* outs, int out_n, const NnGeom* geom, NnResult* res)
{{
    (void)out_n;
    res->box_n = 0;
    res->topk_n = 0;
    if (outs[0].data == 0 || outs[0].w < 6)
        return -1;
    const float* rows = outs[0].data;
    const int stride = outs[0].w;
    const int rows_n = outs[0].h;
    const float sw = (float)geom->src_w;
    const float sh = (float)geom->src_h;
    int nb = 0;
    for (int r = 0; r < rows_n; r++)
    {{
        const float* row = rows + (size_t)r * stride;
        const int label = (int)(row[0] + 0.5f);
        const float score = row[1];
        if (label <= 0 || score < SCORE_TH)
            continue;
        NnDetBox b;
        b.label = label;
        b.score = score;
        b.x0 = clip_i((int)(row[2] * sw), 0, (int)sw - 1);
        b.y0 = clip_i((int)(row[3] * sh), 0, (int)sh - 1);
        b.x1 = clip_i((int)(row[4] * sw), 0, (int)sw - 1);
        b.y1 = clip_i((int)(row[5] * sh), 0, (int)sh - 1);
        if (b.x1 - b.x0 < 4 || b.y1 - b.y0 < 4)
            continue;
        cand_push(s_cand, &nb, b);
    }}
    int nk = 0;
    while (nk < KEEP_MAX)
    {{
        int best = -1;
        for (int j = 0; j < nb; j++)
        {{
            if (s_cand[j].score < 0.f) continue;
            if (best < 0 || s_cand[j].score > s_cand[best].score) best = j;
        }}
        if (best < 0) break;
        s_keep[nk] = s_cand[best];
        s_cand[best].score = -1.f;
        const NnDetBox* k = &s_keep[nk];
        for (int j = 0; j < nb; j++)
        {{
            if (s_cand[j].score < 0.f) continue;
            if (box_iou(*k, s_cand[j]) > NMS_IOU) s_cand[j].score = -1.f;
        }}
        nk++;
    }}
    for (int i = 0; i < nk; i++)
        res->box[i] = s_keep[i];
    res->box_n = nk;
    return 0;
}}
""" + _boot("检测", int(cfg.get("blob_in") or 0), "{ BLOB_OUT }", "{ NN_ROLE_DET }", 1)


def render_retina(cfg: dict, name: str) -> str:
    pairs = (
        (int(cfg.get("blob_cls8", -1)), int(cfg.get("blob_bbox8", -1))),
        (int(cfg.get("blob_cls16", -1)), int(cfg.get("blob_bbox16", -1))),
        (int(cfg.get("blob_cls32", -1)), int(cfg.get("blob_bbox32", -1))),
    )
    if any(a < 0 or b < 0 for a, b in pairs):
        raise ValueError("RetinaFace blob 未齐（cls/bbox 三个尺度）")
    size = int(cfg.get("image_size") or cfg.get("input_size") or 320)
    score = float(cfg.get("score") or 0.5)
    nms = float(cfg.get("nms_iou_th") or 0.4)
    pre_pixel = _pixel_cxx("bgr2rgb", 3)
    return _common_head(name, "retinaface") + f"""
const int IN_SIZE = {size};
const int BLOB_IN = {int(cfg.get("blob_in") or 0)};
const float SCORE_TH = {score:.8g}f;
const float NMS_IOU = {nms:.8g}f;
const int CAND_MAX = 256;
const int KEEP_MAX = 32;
const int LABEL_FACE = 1;
const int ANCHOR_N = 2;
const int S_BLOB_CLS[3] = {{ {pairs[0][0]}, {pairs[1][0]}, {pairs[2][0]} }};
const int S_BLOB_BBOX[3] = {{ {pairs[0][1]}, {pairs[1][1]}, {pairs[2][1]} }};
const int S_STRIDE[3] = {{ 8, 16, 32 }};
const float S_EDGE[3][2] = {{ {{32.f, 16.f}}, {{128.f, 64.f}}, {{512.f, 256.f}} }};
const float ANCHOR_BASE_C = 8.f;
float s_in[IN_SIZE * IN_SIZE * 3];
NnDetBox s_cand[CAND_MAX];
NnDetBox s_keep[KEEP_MAX];

inline int px_clip(int v, int lo, int hi)
{{
    return v < lo ? lo : (v > hi ? hi : v);
}}
inline void cand_add(NnDetBox* arr, int* n, const NnDetBox& b)
{{
    if (*n < CAND_MAX) {{ arr[(*n)++] = b; return; }}
    int worst = 0;
    for (int i = 1; i < CAND_MAX; i++)
        if (arr[i].score < arr[worst].score) worst = i;
    if (b.score > arr[worst].score) arr[worst] = b;
}}
inline float box_iou(const NnDetBox& a, const NnDetBox& b)
{{
    const int ix0 = a.x0 > b.x0 ? a.x0 : b.x0;
    const int iy0 = a.y0 > b.y0 ? a.y0 : b.y0;
    const int ix1 = a.x1 < b.x1 ? a.x1 : b.x1;
    const int iy1 = a.y1 < b.y1 ? a.y1 : b.y1;
    const int iw = ix1 - ix0;
    const int ih = iy1 - iy0;
    if (iw <= 0 || ih <= 0) return 0.f;
    const int inter = iw * ih;
    const int area1 = (a.x1 - a.x0) * (a.y1 - a.y0);
    const int area2 = (b.x1 - b.x0) * (b.y1 - b.y0);
    return (float)inter / (float)(area1 + area2 - inter);
}}

int preprocess(const NnImage* img, NnTensor* in, NnGeom* geom)
{{
    if (img == 0 || img->pix == 0 || img->w <= 0 || img->h <= 0)
        return -1;
    @STAGE@
    const int sw = im->w;
    const int sh = im->h;
    float scale = (float)IN_SIZE / sw;
    if ((float)IN_SIZE / sh < scale)
        scale = (float)IN_SIZE / sh;
    int nw = (int)(sw * scale + 0.5f);
    int nh = (int)(sh * scale + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > IN_SIZE) nw = IN_SIZE;
    if (nh > IN_SIZE) nh = IN_SIZE;
    const int pad_x = (IN_SIZE - nw) / 2;
    const int pad_y = (IN_SIZE - nh) / 2;
    {pre_pixel}
    ncnn::Mat rs = ncnn::Mat::from_pixels_resize(im->pix, ptype, sw, sh, nw, nh);
    memset(s_in, 0, sizeof(s_in));
    for (int c = 0; c < 3; c++)
    {{
        const float* sp = (const float*)rs.data + (size_t)c * rs.cstep;
        float* dp = s_in + (size_t)c * IN_SIZE * IN_SIZE + (size_t)pad_y * IN_SIZE + pad_x;
        for (int y = 0; y < nh; y++, sp += nw, dp += IN_SIZE)
            memcpy(dp, sp, (size_t)nw * sizeof(float));
    }}
    rs.release();
    in->data = s_in;
    in->w = IN_SIZE;
    in->h = IN_SIZE;
    in->c = 3;
    geom->scale = scale;
    geom->pad_x = pad_x;
    geom->pad_y = pad_y;
    geom->src_w = sw;
    geom->src_h = sh;
    return 0;
}}

int decode(const NnTensor* outs, int out_n, const NnGeom* geom, NnResult* res)
{{
    res->box_n = 0;
    res->topk_n = 0;
    if (out_n < 6)
        return -1;
    int nb = 0;
    for (int s = 0; s < 3; s++)
    {{
        const NnTensor* cls = &outs[2 * s];
        const NnTensor* bbx = &outs[2 * s + 1];
        if (cls->data == 0 || bbx->data == 0 || cls->w <= 0 || cls->h <= 0)
            return -2;
        if (cls->c < 2 * ANCHOR_N || bbx->c < 4 * ANCHOR_N)
            return -3;
        const int w = cls->w;
        const int h = cls->h;
        const size_t wh = (size_t)w * h;
        const float stride = (float)S_STRIDE[s];
        for (int q = 0; q < ANCHOR_N; q++)
        {{
            const float* prob = cls->data + (size_t)(q + ANCHOR_N) * wh;
            const float* p_dx = bbx->data + (size_t)(q * 4 + 0) * wh;
            const float* p_dy = bbx->data + (size_t)(q * 4 + 1) * wh;
            const float* p_dw = bbx->data + (size_t)(q * 4 + 2) * wh;
            const float* p_dh = bbx->data + (size_t)(q * 4 + 3) * wh;
            const float edge = S_EDGE[s][q];
            for (int y = 0; y < h; y++)
            {{
                const float acy = ANCHOR_BASE_C + (float)y * stride;
                for (int x = 0; x < w; x++)
                {{
                    const int i = y * w + x;
                    const float score = prob[i];
                    if (score < SCORE_TH)
                        continue;
                    const float acx = ANCHOR_BASE_C + (float)x * stride;
                    const float pcx = acx + edge * p_dx[i];
                    const float pcy = acy + edge * p_dy[i];
                    const float pw = edge * expf(p_dw[i]);
                    const float ph = edge * expf(p_dh[i]);
                    NnDetBox b;
                    b.label = LABEL_FACE;
                    b.score = score;
                    b.x0 = px_clip((int)((pcx - pw * 0.5f - geom->pad_x) / geom->scale + 0.5f), 0, geom->src_w - 1);
                    b.y0 = px_clip((int)((pcy - ph * 0.5f - geom->pad_y) / geom->scale + 0.5f), 0, geom->src_h - 1);
                    b.x1 = px_clip((int)((pcx + pw * 0.5f - geom->pad_x) / geom->scale + 0.5f), 0, geom->src_w - 1);
                    b.y1 = px_clip((int)((pcy + ph * 0.5f - geom->pad_y) / geom->scale + 0.5f), 0, geom->src_h - 1);
                    if (b.x1 - b.x0 < 4 || b.y1 - b.y0 < 4)
                        continue;
                    cand_add(s_cand, &nb, b);
                }}
            }}
        }}
    }}
    int nk = 0;
    while (nk < KEEP_MAX)
    {{
        int best = -1;
        for (int j = 0; j < nb; j++)
        {{
            if (s_cand[j].score < 0.f) continue;
            if (best < 0 || s_cand[j].score > s_cand[best].score) best = j;
        }}
        if (best < 0) break;
        s_keep[nk] = s_cand[best];
        s_cand[best].score = -1.f;
        const NnDetBox* kept = &s_keep[nk];
        for (int j = 0; j < nb; j++)
        {{
            if (s_cand[j].score < 0.f) continue;
            if (box_iou(*kept, s_cand[j]) > NMS_IOU) s_cand[j].score = -1.f;
        }}
        nk++;
    }}
    for (int i = 0; i < nk; i++)
        res->box[i] = s_keep[i];
    res->box_n = nk;
    return 0;
}}
""" + _boot(
        "检测",
        int(cfg.get("blob_in") or 0),
        "{ S_BLOB_CLS[0], S_BLOB_BBOX[0], S_BLOB_CLS[1], S_BLOB_BBOX[1], S_BLOB_CLS[2], S_BLOB_BBOX[2] }",
        "{ NN_ROLE_CLS, NN_ROLE_BBX, NN_ROLE_CLS, NN_ROLE_BBX, NN_ROLE_CLS, NN_ROLE_BBX }",
        6,
    )


def render_module(folder: Path, cfg: dict, name: str, steps: list[dict] | None = None) -> str:
    extra = _sidecar(folder)
    family = _family(str(cfg.get("post") or extra.get("post") or ""), str(extra.get("arch") or ""))
    if family == "retinaface":
        src = render_retina(cfg, name)
    elif family == "ssd":
        src = render_ssd(cfg, name)
    else:
        src = render_classify(cfg, name)
    return _apply_stage(src, list(steps or []))


def _compiler_text(data: bytes | None) -> str:
    raw = data or b""
    if not raw:
        return ""
    text = raw.decode("utf-8", errors="replace")
    if "\ufffd" in text:
        text = raw.decode("gbk", errors="replace")
    return text.replace("\ufffd", "")


def build_model_mod(mod_dir: Path, name: str, fpga_root: Path) -> tuple[Path | None, str]:
    script = fpga_root / "tools" / "nnmod" / "nnmod.py"
    from repo_paths import script_argv

    cmd = script_argv(script, "build", str(mod_dir), "--name", name[:15])
    toolchain = resolve_toolchain()
    if toolchain:
        cmd.extend(["--toolchain", toolchain])
    else:
        return None, "未找到交叉编译器，model.mod 尚未编译"
    run_kwargs = dict(capture_output=True, timeout=180)
    if sys.platform == "win32":
        run_kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    proc = subprocess.run(cmd, **run_kwargs)
    log = (_compiler_text(proc.stdout) + "\n" + _compiler_text(proc.stderr)).strip()
    mod = mod_dir / MOD_NAME
    if proc.returncode != 0 or not mod.is_file():
        return None, public_board_status(log or "model.mod 编译失败")
    return mod, "model.mod 已按当前固件封包"


def _patch_deploy(folder: Path, fields: dict) -> None:
    path = folder / "deployment_config.json"
    data: dict = {}
    if path.is_file():
        try:
            loaded = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                data = loaded
        except Exception:
            data = {}
    data.update(fields)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def prepare_board_pack(folder: Path | str, fpga_root: str | Path | None = None) -> dict:
    """生成模块、尽量编译，并写出 models_sd/<短名>/。"""
    folder = Path(folder)
    extra = _sidecar(folder)
    dataset = str(extra.get("dataset") or "")
    arch = str(extra.get("arch") or "")
    quant = str(extra.get("quantize") or "fp32")
    name = board_folder_name(dataset, arch, quant) if dataset and arch else board_folder_for_pack(folder)
    cfg_path = folder / CFG_NAME
    status = ""
    ready = False
    mod_path: Path | None = None
    if not cfg_path.is_file():
        status = "缺少 model.cfg，不能生成 model.mod"
    else:
        cfg = parse_model_cfg(cfg_path)
        steps = _load_steps(folder, extra)
        try:
            source = render_module(folder, cfg, name, steps)
        except ValueError as exc:
            source = ""
            status = str(exc)
        board_dir = folder / "board"
        board_dir.mkdir(parents=True, exist_ok=True)
        if source:
            (board_dir / "model_mod.cpp").write_text(source, encoding="utf-8")
        if source:
            root = resolve_fpga_root(fpga_root)
            if root is None:
                status = "未找到板工程里的 tools/nnmod/nnmod.py，model.mod 尚未编译"
            else:
                try:
                    mod_path, status = build_model_mod(board_dir, name, root)
                except Exception as exc:
                    mod_path = None
                    status = f"model.mod 编译失败: {exc}"
                ready = mod_path is not None
        if ready and steps:
            status = f"{status}；已把 {len(steps)} 个图像积木写进 model.mod"
    sd_dir = write_sd_four_files(folder, name, model_mod=mod_path, include_mod=ready)
    retire_long_sd_dir(folder, name)
    result = {
        "ready": ready,
        "status": status or ("可以推送 " + name),
        "board_folder": name,
        "sd_dir": str(sd_dir),
        "host_models_dir": str(folder),
        "board": "18_cam_lcd_hdmi_servo_net",
    }
    result["status"] = public_board_status(result["status"])
    _patch_deploy(folder, {
        "board_folder": name,
        "board_ready": ready,
        "board_status": result["status"],
        "sd_models_dir": str(sd_dir),
        "host_models_dir": str(folder),
        "board": "18_cam_lcd_hdmi_servo_net",
    })
    return result
