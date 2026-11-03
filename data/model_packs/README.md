# 板上模型包

每个子目录对应 SD 卡 `0:/models/<短名>/`。短名不超过 20 个 ASCII 字符。

板上扫描时四件要齐：

- `model.param.bin`
- `model.bin`
- `labels.txt`（ASCII）
- `model.mod`（预处理、图像积木、blob、解码；由板工程 `nnmod.py` 编译）

不放 `model.cfg` 和 `image_ops.cfg`。板子不读这两个文件。训练时的积木链已经编译进 `model.mod`。

上位机训练、推理、已训练列表用仓库 `models/` 下的完整包。已训练模型页可以把本目录经网口推到板子：先在板上打开「文件传输」，PC 网卡设为 192.168.1.100/24。
