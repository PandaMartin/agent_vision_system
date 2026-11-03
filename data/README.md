# data —— 测试数据与参考结果

| 目录/文件 | 内容 |
|---|---|
| `datasets/mnist/` | MNIST 数据（`mnist.npz`，训练/评测唯一数据源；展开 PNG 图因体积不随包，由 `train/datasets.py` 从 npz 加载） |
| `model_packs/` | 板端就绪模型包（5 个：mnist 分类 fp32/int8、手形分类、人脸检测 RetinaFace fp32 / SSDLite int8） |
| `reference_results.md` | 参考结果汇总：各模型包测试精度（train_info.json 机器提取）、量化审阅规则、板端实测口径 |

上位机侧完整模型库（含部署指引与训练超参快照）在 `src/host_agent/models/`；
训练运行记录（校准集、训练超参快照）在 `src/host_agent/models_output/`。
数据集获取脚本与说明：`src/host_agent/datasets/README.md`、`train/download_datasets.py`。

> 体积说明：上位机原始 `models_sd/` 目录与 `model_packs/` 内容相同（已去重）；
> `wider_face_ssdlite_int8.rar` 与 `wd_face_ssd_int8/` 目录同内容（保留目录）。

## 快速复现一条数据链（约 10 分钟，CPU 即可）

```bash
cd src/host_agent
pip install -r requirements.txt
python train/train.py --dataset mnist --arch simple_cnn   # 训练（test_accuracy 应 ≈0.986）
python train/export_ncnn.py                                # 导出 ncnn（fp32 基线）
# 量化审阅：INT8 与 fp32 在同一批测试图上对比，低 15 个百分点以上则下发 fp32
```

参考数值与判据见 `reference_results.md`。
