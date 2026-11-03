# 参考结果

数字全部由 `train_info.json` / `model_info.json`（训练运行产物）机器读取，出处逐条标注；
未实测项不出现。板端帧率实测见 `board/measured_outputs.md`。

## 1. 模型包测试精度（train_info.json）

| 模型包（`src/host_agent/models/`） | 任务 | 位宽 | test_accuracy | 来源 |
|---|---|---|---|---|
| mnist_simple_cnn_fp32 | MNIST 分类 | fp32 | **0.9859**（loss 0.0467） | `train_info.json`（10 epochs, bs 64, lr 1e-3） |
| hands_resnet18_fp32 | 手形分类 | fp32 | **0.8065** | `train_info.json` |
| wider_face_retinaface_fp32 | 人脸检测 | fp32 | **0.5254**（mAP 口径） | `train_info.json` |
| wider_face_ssdlite_int8 | 人脸检测 | int8 | **0.5973**（mAP 口径） | `train_info.json` |

> 注：两个人脸检测包为**不同网络结构**（RetinaFace vs SSDLite），上表不可当作
> "fp32 vs int8" 同网对比；同网 fp32/int8 对比在量化审阅环节按同批图现场执行（见 §2）。

## 2. 量化审阅规则（已验证，固化为上位机 agent 纪律）

- 分类模型 INT8 量化后，须与 fp32 在**同一批测试图**上对比；INT8 低 **15 个百分点**以上
  → 自动下发 fp32，不硬上 INT8；
- QAT 仅用于分类，导出标记 QAT 后不再重复本次审阅；
- 来历：INT8 在部分模型上实测回退超阈值，该规则避免带病上线（`src/host_agent/skills/host-agent/SKILL.md`）。

## 3. 板端参考结果（实测记录摘录）

| 项 | 数值 | 出处 |
|---|---|---|
| mnv2_ssdlite_voc 检测单帧 | 239 ms ≈ 4.2 fps（框/类别/坐标正确，2026-10-22 上板） | `model_platform_design.md` |
| 网络推送（TCP 调优后） | 77 KB/s → MB/s 量级 | `platform_network_config.md` |
| 网络推送（标准 TFTP 交付形态） | 数百 KB/s（512 B 锁步所限） | `net_push.py` 文档 |
| RTL 采集 IP 仿真 | 6/6 CHECK PASS | `sim/sim_result.txt` |

## 4. 模型包清单（`data/model_packs/`，板端就绪四文件）

| 目录 | 内容 |
|---|---|
| `mnist_cnn_fp32` / `mnist_dcnn_int8` | MNIST 分类（fp32 / int8） |
| `hands_r18_fp32` | 手形分类 |
| `wd_face_ret_fp32` / `wd_face_ssd_int8` | WIDER FACE 人脸检测 |

每个包内：`model.param.bin`（ncnn2mem 结构）+ `model.bin`（权重）+ `labels.txt`（类别名）+
`model.mod`（自描述程序模块：预处理/解码/NMS/阈值，见 `nn_pipeline.h` 冻结契约）。
推板即用：`python tools/netpush/net_push.py --dir 0:/models/<名> <四文件>`。
