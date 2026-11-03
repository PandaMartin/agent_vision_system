# 上位机 agent

这套系统把 PyTorch 训练出来的网络，导出成 ncnn，封进板子能加载的模型包，经网口推到 FPGA 板上运行。桌面小应用也在这台电脑上编写、交叉编译，再推到板子桌面。换电脑或换板时，先读仓库根目录的 `board_profile.json`。不要把某一台电脑的盘符当成唯一路径。

## 两条目录

上位机仓库就是当前这个工程。FPGA 工程是另一份目录，里面有 Vivado 工程和 Vitis 软件工程。上位机用 `tools/nnmod/nnmod.py` 是否存在来识别软件工程根。仓库里还没有这份 `tools/` 时，用环境变量 `FPGA_PROJECT`。工程怎么分目录、内存怎么划、固件和模型怎么分工，用 `read_host_note`（page=fpga）。

## 一条链路

1. `datasets/` 里的图，在 `train/` 用 PyTorch 训练。
2. 导出 ncnn。分类要做量化审阅。三个命令在 `prebuilt/ncnn/`。
3. 按模型语义生成 `model.mod`，和 `model.param.bin`、`model.bin`、`labels.txt` 一起放进 `models_sd/<短名>/`。
4. 板上「文件传输」打开后，经网口写到 `0:/models/<短名>/`。
5. 桌面应用写在 `apps/`，用 `prebuilt/gcc-arm-none-eabi/` 和 FPGA 工程里的 `sdapp.py` 编成 `.bin`，写到 `0:/apps/`。

仓库各目录做什么，用 `read_host_note`（page=project）。推板时的路径、长度和网口，用 page=board。写应用的步骤和骨架，用 page=apps。

## 本仓库目录

- `agent_ui_v2/`：界面和对话。入口 `python agent_ui_v2/app_fluent.py`。
- `train/`：训练、导出、模拟、封 `model.mod`、推送、编译应用。
- `models/<包名>/`：上位机保存的完整包。`models_sd/<短名>/`：准备推到板子的四个文件。
- `datasets/`：训练数据。`apps/`：在本机编写的桌面应用。
- `prebuilt/gcc-arm-none-eabi/`：交叉编译器。`prebuilt/ncnn/`：`ncnn2table`、`ncnn2int8`、`ncnn2mem`。优先用这两份。
- `skills/host-agent/`：本说明。`board_profile.json`：当前板的屏幕、网口、路径上限和模型区地址。
- `tools/`：板工程工具和 `fw_exports` 符号表。仓库里已有就用这份；固件迭代后从新的板工程整套替换。还没有时用 `FPGA_PROJECT`。
- `firmware/`：空白 FPGA 的比特流、FSBL 和应用 ELF。左侧「FPGA 代码」经 JTAG 下载。掉电后需再下载。

环境变量只在仓库里没有对应工具时使用：`FPGA_PROJECT`、`FPGA_TOOLCHAIN`（`arm-none-eabi-` 前缀）、`FPGA_NCNN_ROOT`。

## 新环境先做

1. 安装 `requirements.txt` 里的依赖。交叉编译器和 ncnn 命令已经在 `prebuilt/`。
2. 确认 `board_profile.json` 与当前板子一致。
3. 找到 FPGA 工程的 `tools/`。调用 `query_symbols`。固件表或字库缺失时，先在该工具目录执行 `python sdapp/sdapp.py export`，不要编造函数地址。
4. 训练、导出、识别、写应用都走已有本地工具，不要假装已经完成。报错或对不上文件时，用 `read_project_file` 和 `run_command` 看现场，说明在 page=shell。
5. 上位机经 JTAG 下载已编好的比特流、FSBL 和 ELF。本机有 Vitis 的 `xsct.bat` 或 Vivado 的 `xsdb.bat` 时用它们；都没有时用项目内置的 `prebuilt/openocd`。只安装 Vitis HLS 不能代替这两个程序。重新综合比特流用 Vivado，修改并重编固件用 Vitis。只做导出、封模型、写 SD 应用时不用它们。

## 原则

- 板子读 `model.mod`，不读 `model.cfg`。`name=` 是文件夹身份，中文显示名另存。
- 分类里，训练后 INT8 要做量化审阅。INT8 比同一批图上的 fp32 低 15 个百分点以上就下发 fp32。QAT 只用于分类，导出标记为 QAT，不再做这次审阅。
- 推送前，板上「文件传输」必须开着。只写不读。PC 网口用 `configure_board_net`（或设置页同一按钮）按 `pc_ip_hint` 追加地址。
- 不修改 FPGA 工程里的固件源码、硬件描述和 Vivado 工程。不把旧板的固件替换包下发到当前板子。
- 固件重新编译并 export 之后，用 adapt_firmware 按 status、import、rebuild、program 的顺序衔接。不要把旧地址写进源码。
- 写桌面应用之前必须 `query_symbols`（固件符号和字库）并 `read_host_note`（page=apps）。界面再读 page=ui：深色卡片，标题用 font_cn_20，主数字用 lv_font_montserrat_48。可用刻度盘、圆弧、进度条、曲线、滑杆、开关、复选框、指示灯和转圈，一屏只留一个主视觉。不要画网格和霓虹角标。用 `save_app_source` 分段落盘，再让 `write_board_app` 编译磁盘上的文件。不要在一轮里重发整份 `app_main.c`。
- 用户问仓库分工、训练到上板的流程时，先 `read_host_note`（page=project）。问板工程目录、内存、固件和模型怎么配合时，先 `read_host_note`（page=fpga）。
