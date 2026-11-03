# 上位机智能体（host_agent）安装与构建

源码位置：`src/host_agent/`。它把「训练 → 导出 ncnn → 量化审阅 → 封 model.mod →
TFTP 推板 → SD 应用交叉编译」整条链路收敛到一个仓库，并附 agent 技能文件族
（`skills/host-agent/`，供对话式智能体按纪律调用）。

## 源码方式运行（推荐评委复现用）

```bash
cd src/host_agent
pip install -r requirements.txt        # PyTorch 按本机 CUDA 情况自选 wheels
python train/agent_shell.py --help     # 工具调用层自检
python train/train.py --help           # 训练入口（mnist/手形/人脸检测等已内置）
```

- 各环节脚本：`train/`（训练/导出/量化审阅/封包/推送/JTAG 下载）、`tools/`
  （netpush/nnmod/sdapp/bsp_net_check/gui.py 图形工具箱）；
- 交叉编译器与 ncnn2int8 等命令默认取 `prebuilt/`（**随打包分发，不随本仓库**），缺失时：
  - ncnn 命令：从 ncnn 官方发布或源码编译，设环境变量 `FPGA_NCNN_ROOT` 指向其目录；
  - arm 交叉编译器：`FPGA_TOOLCHAIN` 设前缀（如 `arm-none-eabi-`），或用 Vitis 自带工具链；
- 板卡参数单一真值源：`board_profile.json`。

> 提交包体积说明（与仓库根 README"提交合规说明"一致）：本快照内
> `models_sd/`（板端就绪包）与 `firmware/`（JTAG 下载组）为重复副本，已去重——
> 分别使用 `data/model_packs/` 与 `board/firmware/`；agent 运行时如需原路径，
> 复制或软链回去即可（`train/` 内脚本按相对路径自动识别）。

## 打包分发方式（PyInstaller onedir）

本项目随开发机打包为免安装目录包（入口 `FPGA_Deploy_Assistant.exe`，内含 Python 运行时、
PyQt5 界面、交叉编译器与 ncnn 命令），供无 Python/Vitis 环境的电脑直接运行——该分发包
体积大（数 GB），不随本仓库提交；打包方法见 `src/host_agent/BUILD_GUIDE.md`。

> 如实说明：聊天界面（PyQt5 UI 层）的 Python 源码未包含在本快照中，仅随打包机构建；
> 仓库内包含**全部**训练/导出/封包/推送/编译工具链源码、agent 技能文件与模型/应用数据。
> UI 使用的界面预设保存在 `agent_ui_v2/models/presets/models.json`。

## 环境变量（仅在仓库内缺对应工具时使用）

| 变量 | 作用 |
|---|---|
| `FPGA_PROJECT` | FPGA 板工程根（含 `tools/` 的目录），默认自动识别 |
| `FPGA_TOOLCHAIN` | 交叉编译器前缀（默认 `arm-none-eabi-`） |
| `FPGA_NCNN_ROOT` | ncnn 命令目录（ncnn2table/ncnn2int8/ncnn2mem） |
