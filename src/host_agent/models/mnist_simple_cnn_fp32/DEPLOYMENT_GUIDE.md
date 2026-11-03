# 板上模型包（18_cam_lcd_hdmi_servo_net）

上位机完整包在 `models/mnist_simple_cnn_fp32/`（含 model.cfg、graph.param）。不要整夹拷 SD。

推到板子的目录是 `models_sd/mnist_cnn_fp32/`，对应 `0:/models/mnist_cnn_fp32/`：

```
model.param.bin
model.bin
labels.txt
model.mod
```

板端不读 model.cfg 和 image_ops.cfg。model.mod 里写死了输入尺寸、通道、图像积木、blob 和解码。
文件夹名不超过 20 个 ASCII 字符。labels.txt 只用 ASCII。

推送：板上先打开「文件传输」。PC 网卡 192.168.1.100/24，板 192.168.1.10，UDP 69。
已训练模型页的「推送到 FPGA」走同一条 TFTP。

本包：pixel=bgr  mean=0,0,0  norm=0.00392157,0.00392157,0.00392157  input_size=28
板上状态：model.mod 已按当前固件封包

## PC 验证

```
python train/simulate_fpga.py --model mnist
```
