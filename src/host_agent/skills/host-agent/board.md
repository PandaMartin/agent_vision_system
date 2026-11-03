# 推到板上的约定

数字以仓库根目录 `board_profile.json` 为准。DDR 分区的含义在 page=fpga，来源是 FPGA 工程的 `src/hw/mem_layout.h`。

## 模型包

上位机完整包在 `models/<名称>/`。推到板子的是 `models_sd/<短名>/` 里四个文件：

- `model.param.bin`：`prebuilt/ncnn/ncnn2mem.exe` 生成的二进制结构。
- `model.bin`：权重。放在固件堆上，不受模型代码区 4MB 的限制。
- `labels.txt`：一行一个类别名。板上界面用它显示名字，优先使用英文或 ASCII。
- `model.mod`：预处理、图像积木、输出 blob 和解码。

短名长度不超过 `board_folder_max`，并且是 ASCII。远程路径 `0:/models/<短名>/model.param.bin` 必须短于 `path_limit`。板子只索引 `/models` 的直接子目录，个数不超过 `model_index_max`。文件夹名就是身份，和上位机里的中文显示名分开存放。

分类结果是 Top-K。SSD 的检测解码在 `model.mod` 里。RetinaFace 的锚框解码也在 `model.mod` 里，并保持 fp32。固件对所有模型使用同一次调用顺序，见 page=fpga。

## 传输

板上先打开「文件传输」。板地址是 `board_ip`，端口 `board_port`。PC 地址以档案 `pc_ip_hint` 为准。对话里调用 `configure_board_net`（先 `action=status`，未就绪再经用户同意后 `action=configure`）；设置页也有同一功能。向有线网卡追加该地址，不删除原有 IP；需要管理员确认。协议只写不读，不能靠它删除板上的旧文件。旧的 int8 包若已经在板上，需要在板子上另行处理，再推新的 fp32 包。

图片发到 `0:/photos/`。格式是 24 位未压缩 BMP，宽度是 4 的倍数，发送前把边长限制在 `photo_max_w` × `photo_max_h`。

## 桌面应用

源码在上位机 `apps/<目录>/`。编译调用 FPGA 工程 `tools/sdapp/sdapp.py build`，编译器使用 `prebuilt/gcc-arm-none-eabi/`。产物推到 `0:/apps/<目录>.bin`。桌面图标从 bin 头里读，最多 `gui_app_max` 个。不要覆盖 `snake` 和 `face_track`。

写应用的步骤、限制和骨架见 page=apps。函数地址用 `query_symbols` 查 `sdapp/fw_exports.ld`，汉字查 `font/ui_symbols.txt`。
