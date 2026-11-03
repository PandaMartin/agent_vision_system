# data —— 测试数据与参考结果

| 目录/文件 | 内容 |
|---|---|
| `datasets/mnist/` | MNIST 测试数据（`mnist.npz` + 按类别组织的图片 `images/train/<类别>/`），复现训练与量化审阅用 |
| `model_packs/` | 板端就绪模型包（5 个：mnist 分类 fp32/int8、手形分类、人脸检测 RetinaFace fp32 / SSDLite int8） |
| `reference_results.md` | 参考结果汇总：各模型包测试精度（train_info.json 机器提取）、量化审阅规则、板端实测口径 |

上位机侧完整模型库（含训练中间产物与部署指引）在 `src/host_agent/models/`；
训练运行记录（校准集、训练超参快照）在 `src/host_agent/models_output/`。
数据集获取脚本与说明：`src/host_agent/datasets/README.md`、`train/download_datasets.py`。

## 快速复现一条数据链（约 10 分钟，CPU 即可）

```bash
cd src/host_agent
pip install -r requirements.txt
python train/train.py --dataset mnist --arch simple_cnn   # 训练（test_accuracy 应 ≈0.986）
python train/export_ncnn.py                                # 导出 ncnn（fp32 基线）
# 量化审阅：INT8 与 fp32 在同一批测试图上对比，低 15 个百分点以上则下发 fp32
```

参考数值与判据见 `reference_results.md`。
