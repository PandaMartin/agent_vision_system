# JTAG 下载（三条路径，按本机已有工具任选）

下载对象：`board/firmware/` 下 `system_wrapper.bit`（PL 比特流）、`fsbl.elf`（一级引导）、
`cam_lcd_hdmi_servo_net.elf`（FreeRTOS 固件，含 LVGL 桌面与推理运行时）。

> 提示：JTAG 下载为易失加载，掉电后需重新下载；量产/演示建议 SD 卡启动。

## 路径 A：Vitis xsct（装了 Vitis 时首选）

```tcl
# xsct 命令行（Windows: xsct.bat）
connect
targets -set -filter {name =~ "APU"}
fpga board/firmware/system_wrapper.bit
dow  board/firmware/fsbl.elf
con
# 等待 FSBL 初始化 DDR/时钟后停止并加载应用
targets -set -filter {name =~ "APU"}
stop
dow  board/firmware/cam_lcd_hdmi_servo_net.elf
con
```

## 路径 B：Vivado xsdb（只装了 Vivado）

同路径 A 的命令（xsdb 与 xsct 同源），硬件管理器先 Open New Hardware Target 连接 JTAG。

## 路径 C：OpenOCD（无 Xilinx 工具的电脑）

上位机分发包内置 `prebuilt/openocd/`（本仓库不含，见 `build/build_host_agent.md` 获取方式）。
本项目上位机的「FPGA 代码」页面与 `train/fpga_code.py` 即按"xsct/xsdb 优先、OpenOCD 兜底"
的次序自动选择。Zynq-7020 常用配置：`interface jtag_axi`/`xc7z020` 目标文件随发行版
`board/` 脚本目录提供（scripts/xc7z020.cfg）。

## 下载后自检

- 串口（115200-8N1）应打印启动装配日志（各环节失败会降级并说明原因，不停机）；
- 屏幕进入 LVGL 桌面，出现内置应用图标；
- `python tools/bsp_net_check.py check`（固件组件根）核对 BSP 手改项状态。
