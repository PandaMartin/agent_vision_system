# 用命令查清问题

遇到报错、文件对不上、网口或编译结果不清楚时，先看现场，再下结论。不要用猜测代替命令输出。

## 先读文件

`read_project_file` 的 path 相对仓库根，例如 `train/board_push.py`、`apps/host_timer/app_main.c`、`board_profile.json`。一次看相关的那一段。二进制和超大文件不要整篇读。

## 再跑命令

`run_command` 的工作目录就是仓库根。用它查看目录、跑一段 Python、看工具版本。命令失败时，根据退出码和输出改下一条命令，直到原因清楚。

源码启动时，`python` 会用当前解释器。打包后的程序里，用 `python train/某脚本.py` 跑仓库脚本，不要用 `python -c`。

例子：

- `python -c "import sys; sys.path.insert(0, 'train'); import board_net; print(board_net.net_status()['message'])"`
- `dir apps`
- `python train/board_apps.py` 仅在该脚本本身支持命令行时使用

## 不要用命令代替这些工具

训练和导出用 `start_training`、`export_tflite`。写桌面应用用 `save_app_source` 分段写入，再用 `write_board_app` 编译。下发用 `deploy_board_app`。配网口用 `configure_board_net`。JTAG 下载用 `adapt_firmware`。这些步骤有界面进度和确认，命令行不会代替它们。不要用命令行重写 `app_main.c`。

不要关机、格式化磁盘、改注册表。不要修改 FPGA 工程里的固件和 Vivado 源码。
