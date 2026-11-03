# datasets — 训练数据

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
