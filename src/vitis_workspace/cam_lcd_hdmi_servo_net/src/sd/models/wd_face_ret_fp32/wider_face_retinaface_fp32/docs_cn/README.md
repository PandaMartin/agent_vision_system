# 说明（新训 RetinaFace，第一次板测）

权重是仓库 WIDER FACE 训练结果（2026-10-17 02:01），不是 InsightFace mnet.25。

**只拷 SD 不够。** 现固件只有 classify / SSD detect。必须先用上级目录 `板端固件替换/` 覆盖源码并编译 ELF。细节见 `RETINAFACE_BOARD_HANDOFF.md`。

## SD 四文件（在 `../SD卡模型/`）

放到 `SD:/models/wider_face_retinaface_fp32/`：

| 文件 | 作用 |
|---|---|
| `model.cfg` | `post=retinaface`，blob / mean / 320 已填 |
| `model.param.bin` | ncnn 二进制结构 |
| `model.bin` | fp32 权重约 1.65MB |
| `labels.txt` | `background` / `face` |

## 本包 blob（不要用 mnet.25 的号）

| 尺度 | cls | bbox | lmk |
|---|---|---|---|
| 8 | 134 | 124 | 125 |
| 16 | 137 | 127 | 128 |
| 32 | 140 | 130 | 131 |

预处理：`PIXEL_BGR2RGB`，320 letterbox，mean=0，norm=1。  
解码抄 `ncnn/examples/retinaface.cpp`（variance=1）。
