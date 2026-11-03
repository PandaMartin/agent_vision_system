# FPGA 神经网络部署助手 - 打包说明

打成 **PyInstaller onedir 目录包**：一个文件夹，入口是 `FPGA_Deploy_Assistant.exe`。

## 打包

在仓库根目录执行：

```bat
scripts\build_exe.bat
```

产物：`dist\FPGA_Deploy_Assistant\`

目录内会包含：

- `FPGA_Deploy_Assistant.exe` 与 `_internal/`（界面与依赖）
- `prebuilt/`（交叉编译器与 ncnn 三个程序）
- `src/`、`bsp/`、`tools/`、`firmware/`（编译应用和 model.mod、JTAG 下载）
- `train/`、`skills/`、`board_profile.json`
- `models/`、`models_sd/`、`apps/`
- 空的 `datasets/`（数据集需另拷或首次训练下载）

## 新电脑

1. 拷走整个 `FPGA_Deploy_Assistant` 文件夹，双击 exe。
2. 不必再装 Python、Vitis、Vivado 或整份板工程。编译桌面应用和 `model.mod` 用包内的 `src/`、`bsp/` 和 `tools/`。
3. 推板前配置 PC 网口（设置页或对话 `configure_board_net`），板上打开「文件传输」。
4. 训练需要数据集；默认包内是 CPU 版 PyTorch。
5. 重新综合比特流用 Vivado。JTAG 下载已编好的镜像：有 xsct 或 xsdb 就用本机的；没有则用项目内置 OpenOCD。修改并重编固件仍用 Vitis。

## 说明

- 不做单文件 `onefile`：`prebuilt` 必须与 exe 同级，交叉编译器才能找到库。
- 冻结后的训练/导出通过 `FPGA_Deploy_Assistant.exe --host-script train\xxx.py` 调用。
- 默认排除 TensorFlow；旧 Keras 权重导出请用源码环境。
