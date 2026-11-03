# 上位机仓库

上位机负责把数据训练成网络，并做成板子能直接加载的文件。板子上的摄像头、屏幕、网口和 ncnn 运行时在 FPGA 工程里，见 page=fpga。

## 目录

| 目录 | 作用 |
|---|---|
| `agent_ui_v2/` | PyQt5 界面。开始页训练，已训练模型页推板，板端应用页写应用，左侧「FPGA 代码」经 JTAG 下载比特流和固件。 |
| `tools/` | 从板工程导入的 sdapp、nnmod、netpush、font，以及 fw_exports 符号表。固件迭代后整套替换。 |
| `firmware/` | 下载到空白 FPGA 的 system_wrapper.bit、fsbl.elf、应用 ELF。 |
| `train/` | 命令行。`train.py` 训练，`export_ncnn.py` 导出，`board_mod.py` 生成 `model.mod`，`board_apps.py` 编译应用，`board_push.py` 经 TFTP 下发。 |
| `datasets/` | 训练图。数据集名单以 `train/datasets.py` 的 `DATASET_SPECS` 为准。 |
| `models/<包名>/` | 一次导出的完整档案，含权重、说明和板上那四个文件的来源。 |
| `models_sd/<短名>/` | 只保留要推到 `0:/models/<短名>/` 的四个文件。 |
| `apps/<目录>/` | 本机编写的桌面应用源码和编出来的 `.bin`。 |
| `prebuilt/` | 交叉编译器和 ncnn 的三个命令。优先用这里，不必再指向另一份安装。 |
| `dist/FPGA_Deploy_Assistant/` | onedir 目录包。双击 `FPGA_Deploy_Assistant.exe`。用 `scripts/build_exe.bat` 生成。 |
| `board_profile.json` | 当前这块板的屏幕、IP、路径上限、模型区地址。换板改这份，不要把数字写死在对话里。 |
| `skills/host-agent/` | 给对话 agent 的说明。 |

`models_output/` 是训练过程目录。对话里选择模型、推板、识别，都用已经导出的 `models/` 包。

## 从训练到板上

分类：选数据集和网络，先问用户要不要图像积木，再 `start_training`。积木非空时会写进 `model.mod` 的预处理。训练后 INT8 要做量化审阅，崩了就保留 fp32。QAT 只给分类，导出标记为 QAT，不再做这次审阅。

检测：通用目标用 MobileNetV2 SSDLite，边长 300。人脸用 RetinaFace，边长 320，保持 fp32。检测不走 QAT。

模拟识别走和板子相同的像素路径：0–255 的图，加上 `model.cfg` 里的 mean 和 norm。直接把 0–1 的 CHW 浮点送进 ncnn，结果会和训练对不上。

推送只在用户确认板上「文件传输」已打开之后进行。网口只能写，不能远程删除。路径和长度见 page=board。

## 对话里谁做什么

训练、导出、识别、检测、列模型、写应用、下发应用，都调用已有本地工具，用工具返回的结果说话。

写应用的源码由对话生成，调用 `write_board_app`。编译用的 gcc 来自 `prebuilt/`，链接脚本和符号表来自 FPGA 工程的 `tools/sdapp/`。步骤见 page=apps。

左侧「FPGA 代码」把仓库 `firmware/` 里的比特流、FSBL 和应用 ELF 经 JTAG 下载到已上电的板子。固件重新编译并执行 `sdapp.py export` 之后，用该页「从板工程替换」更新 `tools/` 和 `firmware/`。模型和桌面应用仍经网口推送。

固件源码、Vivado 工程、比特流和 JTAG 不属于上位机的修改范围。要改这些，说明需要在 FPGA 工程里做，并指出对应目录，不要直接改文件。
