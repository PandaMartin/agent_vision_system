# board —— 上板工程、运行脚本与实测输出

| 文件/目录 | 内容 |
|---|---|
| `system_wrapper.xsa` | 硬件交付（PL 比特流 + BD，供 Vitis 平台导入） |
| `firmware/` | JTAG 下载组：`system_wrapper.bit` + `fsbl.elf` + `cam_lcd_hdmi_servo_net.elf` + `manifest.json`（含固件符号版本，5137 个导出符号） |
| `run/jtag_download.md` | 三条 JTAG 下载路径（Vitis xsct / Vivado xsdb / OpenOCD） |
| `run/push_examples.md` | 网口推送运行脚本与示例命令（标准 TFTP） |
| `measured_outputs.md` | 实测输出与参考结果比对（网络两阶段、推理帧率、RTL 仿真、实现指标） |

## 上板最短路径

1. 用 `run/jtag_download.md` 任一路径下载 `firmware/` 三件套（或 SD 卡烧 BOOT.BIT/FSBL 方式）；
2. 板卡进入 LVGL 桌面（启动全流程可降级，单环节失败不停机）；
3. PC 网卡按 `board_profile.json` 的 `pc_ip_hint` 配置（192.168.1.100/24）直连板卡 192.168.1.10；
4. 板端打开「文件传输」应用后，按 `run/push_examples.md` 推送应用/模型；
5. 功能验证判据见 `measured_outputs.md` 与设计报告 §7 步骤 8–9。
