# RetinaFace 上板交接说明（新训 WIDER FACE）

给下游：竞赛板**第一次**跑 RetinaFace。旧固件只有 `classify` / SSD `detect`，**只拷权重不能出框**，必须先改 Vitis 再编 ELF。

上次交接的 `handoff/retinaface_mnet25_fp32`（InsightFace mnet.25）**没有上过板，不要用那套 blob 编号**。本次权重是仓库自己训的 WIDER FACE RetinaFace fp32。

**一包交接目录：**  
`F:\AWork\fpga竞赛\work\handoff\wider_face_retinaface_fp32\`

| 子目录 | 用途 |
|---|---|
| `SD卡模型/` | 拷到 `SD:/models/wider_face_retinaface_fp32/` |
| `板端固件替换/` | 覆盖 `ov5640_ncnn/src/` 后重新编译 |
| `对照资料/` | `model_id.h` / `graph.param`，不进 SD |
| `说明文档/` | 本文与验收说明 |

---

## 0. 结论（先读）

| 项 | 决定 |
|---|---|
| 网络 | 本仓库训练的 **RetinaFace**（WIDER FACE，2026-10-17 02:01），输入 **320×320** |
| 量化 | **fp32**，`model.bin` 约 1.65MB。板上 `use_fp16_storage=false` |
| 预处理 | 摄像头是 BGR，网络要 **RGB**。`from_pixels` 用 `PIXEL_BGR2RGB`。**不减均值**：mean=`0,0,0`，norm=`1,1,1`（对齐 `ncnn/examples/retinaface.cpp`，**不是** Caffe 的 104,117,123，也不是 SSD 的 127.5） |
| 缩放 | 必须 **letterbox** 到 320（等比缩放 + 填 0），禁止把摄像头帧拉扁成正方形 |
| 解码 | **必须抄官方 example**，不要用 variance 0.1/0.2 的 InsightFace PyTorch 公式（见 §2.4） |
| 输出 | 图内 **无** DetectionOutput；三个尺度各有 cls / bbox（landmark 可先不画） |
| SD 加载 | 仍用四文件包：`model.cfg` + `model.param.bin` + `model.bin` + `labels.txt` |
| 与旧模型共存 | `post=detect` 继续走 SSD；**新增** `post=retinaface` |

板上 ncnn 已有 Conv / DWConv / Concat / Interp / Softmax 等，本网用得到。  
**不能**只换权重：现固件只 `extract` 一个 `blob_out`，且按 SSD 的 `N×6` 行解码。

约束（写进代码时不要违反）：

- `NCNN_STRING=0`：只能用 **blob 整数 ID**，不能 `extract("data")`
- `NCNN_SIMPLESTL=1`：禁止 `std::vector` / `std::string`，解码用固定数组
- `NCNN_THREADS=0`：保持 `num_threads=1`
- 堆：`0x02000000`～`0x10000000`（约 224MB），320 fp32 放得下

参考工程：

- 板端：`F:\AWork\12_ov5640_ncnn\vitis\ov5640_ncnn\src\`
- 上位机/导出（第二阶段才动）：`F:\AWork\fpga竞赛\work\`
- ncnn 工具：`F:\AWork\fpga_ncnn\`（`ncnn2mem` 等）

---

## 1. 不要改的文件

这些与摄像头、显示、叠加画框无关，**保持不动**：

| 路径 | 原因 |
|---|---|
| `src/hw/**`（ov5640 / vdma / display / clk） | 视频链路已通 |
| `src/sw/sys/mem_layout.h` | 320 推理一般不需要改 DDR 布局 |
| `src/sw/capture/**` | 拍照仍用整帧 BMP |
| 现有 SD 上的 `mobilenetv2_ssdlite_voc*` | 用 `post=detect` 继续可用 |
| PL bit / Vivado | 第一阶段纯 PS ncnn |

`main.cpp` 里把 `NnDetBox` 转成 `CamOverlayRect` 的逻辑可复用，只要 RetinaFace 后处理填好像素框。

---

## 2. 板端必须改的文件（第一阶段）

工程根：`F:\AWork\12_ov5640_ncnn\vitis\ov5640_ncnn\`

改完后 **重新编译 ELF**，只拷 SD 模型不够。

### 2.1 `src/sw/nn/model_profile.h`

**现状：** `NnPostType` 只有 `CLASSIFY` / `DETECT_SSD`；检测只认单个 `blob_out`。

**改法：**

1. 枚举增加一项：

```c
typedef enum {
    NN_POST_CLASSIFY = 0,
    NN_POST_DETECT_SSD,
    NN_POST_DETECT_RETINA
} NnPostType;
```

2. 在 `NnModelProfile` 末尾增加 RetinaFace 字段（固定 3 个尺度）：

```c
    /* 仅 post=NN_POST_DETECT_RETINA */
    int blob_cls[3];    /* stride 8/16/32 的分类 blob，-1=无 */
    int blob_bbox[3];
    int blob_lmk[3];    /* 不画关键点则全填 -1 */
    u8  retina_strides[3]; /* 建议 {8,16,32} */
```

`blob_out` 可保留，RetinaFace 不用它。`score_th` / `nms_iou_th` / `max_keep` 三档检测共用。

### 2.2 `src/sw/nn/model_mgr.cpp`（`load_cfg`）

**现状：** `post=detect` 一律变成 `NN_POST_DETECT_SSD`；只解析 `blob_out`。

**改法：**

1. `post` 解析改成三段：

```c
if (strcmp(val, "retinaface") == 0)
    prof->post = NN_POST_DETECT_RETINA;
else if (strcmp(val, "detect") == 0)
    prof->post = NN_POST_DETECT_SSD;
else
    prof->post = NN_POST_CLASSIFY;
```

2. 默认 `blob_cls/bbox/lmk` 全设 `-1`，`retina_strides` = `{8,16,32}`。

3. 增加键（未知键本来就会忽略，加上即可）：

| cfg 键 | 写入 |
|---|---|
| `blob_cls8` `blob_cls16` `blob_cls32` | `blob_cls[0..2]` |
| `blob_bbox8` `blob_bbox16` `blob_bbox32` | `blob_bbox[0..2]` |
| `blob_lmk8` `blob_lmk16` `blob_lmk32` | `blob_lmk[0..2]`，缺省 -1 |

4. `post=retinaface` 时校验：三个 `blob_cls` 和三个 `blob_bbox` 均 ≥ 0，否则打印错误并 `return -2`。

`labels.txt` 仍必需。人脸包写两行（与 SSD 一样，0 号是 background，解码出的 label=1）：

```
background
face
```

`model_mgr.h` 只更新文件头注释：`post` 可为 `classify | detect | retinaface`。

### 2.3 `src/sw/nn/nn_runner.h` / `nn_runner.cpp`

**现状：**

- `from_pixels_resize(..., S, S)` 把 1280×800 **拉扁**成正方形 → 预训练 RetinaFace 会明显掉点
- 只 `extract(blob_out)`

**改法 A — 几何（所有 detect 都建议 letterbox，至少 RetinaFace 必须）：**

增加结构（可放 header）：

```c
typedef struct {
    float scale;   /* S 空间 / 原图 */
    int pad_x;     /* letterbox 左边像素 */
    int pad_y;
    int src_w, src_h;
} NnLetterbox;
```

RetinaFace 前向：

1. `scale = min(S/(float)W, S/(float)H)`
2. `nw = round(W*scale)`，`nh = round(H*scale)`
3. `pad_x = (S-nw)/2`，`pad_y = (S-nh)/2`
4. `from_pixels_resize(frame, PIXEL_BGR2RGB, W, H, nw, nh)` 得到 RGB 的 `resized`（摄像头缓冲仍是 BGR）
5. 建 `ncnn::Mat in(S, S, 3)`，**填 0**（本包不减 mean）
6. 把 `resized` 拷到 `(pad_x, pad_y)`
7. **不要** `substract_mean_normalize`（cfg 里 mean=0,norm=1；若仍调用，效果相同）
8. 解码得到的是 **320 输入像素坐标**（不是 0～1），再映回摄像头：

```
x_cam = (x_net - pad_x) / scale
y_cam = (y_net - pad_y) / scale
```

`model.cfg` 的 `pixel` 请增加取值 `bgr2rgb`。现有固件只有 `bgr|rgb`，`rgb` 会把 BGR 摄像头字节**当成** RGB 读入（不做通道交换），和 `PIXEL_BGR2RGB` 不是一回事。

分类模型可继续用旧的直接 resize，避免改 MNIST/SqueezeNet 行为：仅 `post==NN_POST_DETECT_RETINA` 走 letterbox。

**改法 B — 多 blob：**

`NCNN_STRING=0`，必须：

```cpp
ex.extract(prof->blob_cls[i], cls[i]);
ex.extract(prof->blob_bbox[i], bbox[i]);
if (prof->blob_lmk[i] >= 0)
    ex.extract(prof->blob_lmk[i], lmk[i]);
```

建议新增，避免拆分类接口：

```cpp
int nn_forward_retinaface(const u8* frame, int W, int H,
                          ncnn::Mat cls[3], ncnn::Mat bbox[3], ncnn::Mat lmk[3],
                          NnLetterbox* geom);
```

`nn_forward_frame` 保持给 classify / SSD。

### 2.4 `src/sw/nn/post_process.h` / `post_process.cpp`

**现状：** `nn_post_detect` 假定 `det` 每行 `[label,score,x0,y0,x1,y1]`（0～1 归一化）。

**改法：** 新增 `nn_post_retinaface(...)`，输出仍填 `NnDetBox[]`（像素坐标、label=1），这样 `main.cpp` 的画框几乎不用改。

禁止 STL。建议：

```c
#define NN_RETINA_CAND_MAX  256
```

候选超过 256 时按分数丢弃（先用 `score_th` 滤）。

**解码必须对齐** `F:\AWork\fpga_ncnn\third_party\ncnn\examples\retinaface.cpp` 的 `generate_anchors` / `generate_proposals`。PC 端 `train/retina_decode.py` 已按同一公式实现。**不要**用 InsightFace Python 的 `variance=[0.1,0.2]`。

每个尺度：

| stride | scales（× base_size=16） | 锚像素边长 |
|---|---|---|
| 32 | 32, 16 | 512, 256 |
| 16 | 8, 4 | 128, 64 |
| 8 | 2, 1 | 32, 16 |

- `num_anchors = 2`。`cls` blob 已是 Softmax 后的 `face_rpn_cls_prob_reshape_stride*`，channel = 4：前 2 个是 background，**face 分数在 `channel(q + num_anchors)`**，即 q=0 用 ch2，q=1 用 ch3。
- `bbox` channel_range `(q*4, 4)` → dx,dy,dw,dh。
- 锚在 **输入图像像素** 上平移（每次 `+= feat_stride`），公式：

```
cx = anchor_x + 0.5*aw;  cy = anchor_y + 0.5*ah;
pb_cx = cx + aw * dx;     pb_cy = cy + ah * dy;   // 没有 ×0.1
pb_w  = aw * exp(dw);     pb_h  = ah * exp(dh);   // 没有 ×0.2
x0,y0,x1,y1 = pb_cx±pb_w/2, pb_cy±pb_h/2          // 已是 320 像素
```

- `score >= score_th` 才进候选（官方 demo 0.8，现场可先 0.5）。
- 再按 letterbox 映回摄像头 `W×H`，clip。
- NMS 用 `nms_iou_th`（官方 0.4），最多 `max_keep`（32）。
- `NnDetBox.label = 1`（`labels.txt` 第 2 行 `face`）。

Landmark 本包已导出（`blob_lmk8/16/32`），第一阶段可不画。官方 landmark 公式见 example 第 205–214 行。

**务必在 PC 上用同一套公式对拍。** 用错 variance 或拿成 background 通道会满屏乱框或 0 框。

### 2.5 `src/main.cpp`

**现状：** `post != CLASSIFY` 全部当 SSD，把**单个** `out` 交给 `report_detect`。

**改法：**

```cpp
if (prof->post == NN_POST_CLASSIFY) {
    nn_forward_frame(...);
    report_classify(...);
} else if (prof->post == NN_POST_DETECT_SSD) {
    nn_forward_frame(...);
    report_detect(...);          /* 旧逻辑 */
} else if (prof->post == NN_POST_DETECT_RETINA) {
    ncnn::Mat cls[3], bbox[3], lmk[3];
    NnLetterbox geom;
    nn_forward_retinaface(..., cls, bbox, lmk, &geom);
    nn_post_retinaface(cls, bbox, lmk, &geom, prof, boxes, ...);
    /* 下面打印 + CamOverlayRect 与 report_detect 后半段相同 */
}
```

菜单打印里 `post == CLASSIFY ? "classify" : "detect"` 可改成三元/小函数，避免把 retinaface 显示成 classify。

叠加颜色、`OVERLAY_MS`、串口命令不用改。

### 2.6 `src/main.cpp` 依赖的头

`nn_runner.h` / `post_process.h` 增加声明后，确认 `main.cpp` include 已覆盖，无需新模块。

---

## 3. SD 卡模型包（新文件夹，不改旧包）

路径：`SD:/models/wider_face_retinaface_fp32/`（与本交接夹同名）

```
wider_face_retinaface_fp32/
  model.cfg
  model.param.bin
  model.bin
  labels.txt
```

### 3.1 `labels.txt`

```
background
face
```

### 3.2 `model.cfg` 模板

本夹 `model.cfg` **已经填好**（来自本包 `model_id.h`，**不要抄 mnet.25 的编号**）：

```
name = wider_face_retinaface_fp32
input_size = 320
pixel = bgr2rgb
blob_in = 0
blob_out = 140
blob_cls8  = 134
blob_bbox8 = 124
blob_lmk8  = 125
blob_cls16  = 137
blob_bbox16 = 127
blob_lmk16  = 128
blob_cls32  = 140
blob_bbox32 = 130
blob_lmk32  = 131
mean = 0,0,0
norm = 1,1,1
post = retinaface
score_th = 0.5
nms_iou_th = 0.4
max_keep = 32
```

对应 `model_id.h`：

| 尺度 | cls（Softmax 后 reshape） | bbox | landmark |
|---|---|---|---|
| stride 8 | `BLOB_face_rpn_cls_prob_reshape_stride8 = 134` | 124 | 125 |
| stride 16 | `BLOB_face_rpn_cls_prob_reshape_stride16 = 137` | 127 | 128 |
| stride 32 | `BLOB_face_rpn_cls_prob_reshape_stride32 = 140` | 130 | 131 |

重新跑 `ncnn2mem` 后编号会变，以新头文件为准。

现场漏检把 `score_th` 降到 `0.3`；误检再升回 `0.5`。

### 3.3 权重从哪来

本包已由上位机 `export_ncnn.py` 导出（训练目录 `models_output/wider_face_retinaface_20261017_020127`）。板端**不要**再转 caffe/onnx，也**不要**用 `handoff/retinaface_mnet25_fp32` 的 blob。

PC 对拍（下板前）：

```
python train/infer_image.py --pack models/wider_face_retinaface_fp32 --detect 某张人脸图
```

或上位机「已训练模型 → 检测目标」。板上 letterbox / BGR2RGB / mean=0 / 解码必须与 PC 一致。FPN 的 Interp 已写成 `1=2.000000e+00`（整数 `1=2` 会被板上 ncnn 读成 scale=0）。

核对 `graph.param`：

- 输入 3×320×320
- 分类头已接 Softmax（抽 `cls_prob_reshape`，不要抽 `cls_score`）
- 每尺度 2 个锚，bbox 通道 8

---

## 4. 上位机（本次已完成，板端不用改训练）

训练与导出已在 `F:\AWork\fpga竞赛\work\` 做完。板端只改固件 + 拷本夹四文件。

---

## 5. 实现顺序（建议队友按周）

1. **改固件 2.1～2.5 并重新编译 ELF**（上次没做）。分类和 `post=detect` 回归：SqueezeNet / VOC SSDLite 仍能跑。
2. **SD 只放本夹四个文件**到 `/models/wider_face_retinaface_fp32/`，不要整夹拷 `graph.param`。
3. 串口 `l` 应列出该文件夹；`s` 后打印 `N objects` 且 LCD 有框。
4. 漏检先降 `score_th`、确认 letterbox 与 blob ID；不要先改网络。
5. 人脸暂不 INT8。

---

## 6. 验收

- 串口模型列表同时有 VOC SSD 和 RetinaFace，切换互不影响。
- 分类模型行为与改前一致。
- 正对摄像头、单人、脸约占画面 15%～40%：fp32 应稳定出 1 个框，坐标贴合脸廓，置信度大约 0.6～0.99。
- 无人场景不应长期满屏框（若有，提高 `score_th` 或检查 cls 通道是否拿成 background）。
- 推理时间：7020 单线程 fp32 320，粗估百毫秒到数百毫秒级；若数秒以上，查是否误用 640 或开了 INT8 反量化异常。

常见翻车：

| 现象 | 优先查 |
|---|---|
| `load_param failed` | 板上 ncnn 缺层；或 param 不是 `ncnn2mem` 的 `.param.bin` |
| 0 个框 | blob ID 填错；cls 通道反了；mean 误用 127.5；stretch 而非 letterbox |
| 框全图/飞掉 | bbox 通道顺序、variance、stride 与 feat 尺寸不匹配 |
| 分类模型坏了 | letterbox 误用于 classify |
| 链接/STL 报错 | 解码里用了 `std::vector` |

---

## 7. 分工建议

| 角色 | 负责 |
|---|---|
| 板端 | `model_profile.h`、`model_mgr.cpp`、`nn_runner.*`、`post_process.*`、`main.cpp`，Vitis 出 ELF |
| 模型 | 本包四文件 + `model.cfg` blob 已填；PC 用上位机检测对拍 |
| 现场 | 正对摄像头、单人脸约占画面 15%～40% 再看框 |

对接时以本文第 2～3 节的字段名为准，不要再发明第二套 cfg 键名。
