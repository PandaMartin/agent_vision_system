# 竞赛板模型档案（12_ov5640_ncnn）

**拷 SD 卡请用**仓库 `models_sd/wider_face_ssdlite_int8/`（只有四个文件），整夹放到板上 `/models/wider_face_ssdlite_int8/`。

本目录是上位机完整包，不要整夹拷 SD。

板上路径示例：

```
/models/wider_face_ssdlite_int8/
    model.cfg
    model.param.bin
    model.bin
    labels.txt
```

板上启动扫描 `/models/` 下所有含 `model.cfg` 的子目录，串口 `l` 列出、`0-9`/`m` 切换。

## 四个文件

- `model.cfg`：输入边长、BGR/RGB、blob 编号、mean/norm、classify|detect
- `model.param.bin`：`ncnn2mem` 生成的二进制网络结构（不是文本 .param）
- `model.bin`：ncnn 权重
- `labels.txt`：每行一个类别名

## 预处理（必须与训练一致）

板上 `from_pixels_resize` 后执行 `(pixel - mean) * norm`，像素原值 0–255。
本包：pixel=bgr  mean=127.5,127.5,127.5  norm=0.00784314,0.00784314,0.00784314  input_size=300

摄像头是 3 通道。1 通道网络（如 MNIST 灰度）不能直接喂 OV5640 整帧，请用 3 通道数据集，或自行在板上改成灰度。

## PC 验证

```
python train/simulate_fpga.py --model wider_face
```
