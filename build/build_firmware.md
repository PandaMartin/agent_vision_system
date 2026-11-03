# PS 侧固件构建（Vitis 2025.2）

固件工作区：`src/vitis_workspace/`（含 `FreeRtos_Platform` 平台、`cam_lcd_hdmi_servo_net`
固件组件、`lwip_echo_server_freertos` 参考应用）。

## 步骤

1. **硬件平台**：使用 `board/system_wrapper.xsa`（或 PL 重建后重新导出的 XSA）。
2. Vitis 打开工作区 `src/vitis_workspace/`，确认 `FreeRtos_Platform` 平台指向上述 XSA。
3. Build `cam_lcd_hdmi_servo_net` 组件 → 产出 `build/cam_lcd_hdmi_servo_net.elf`
   （CMake + ninja，Vitis 内置；产物亦可直接使用 `board/firmware/` 内已编好的 elf）。

## BSP 手改项恢复纪律（重要）

`FreeRtos_Platform` BSP 的 `libsrc` 含**两处手改**（lwIP 接收超时宏 `LWIP_SO_RCVTIMEO`、
PHY 自协商等待 5s→20s），平台执行"**重新生成**"会冲掉。固件组件根目录执行：

```bash
python tools/bsp_net_check.py check   # 核对，全 PASS 即可
python tools/bsp_net_check.py fix     # 有 FAIL 时自动恢复（原文件备份 .bak）
```

背景与实测来历经由 `src/vitis_workspace/cam_lcd_hdmi_servo_net/src/docs/platform_network_config.md`
（77 KB/s → MB/s 量级的两阶段网络优化记录），工具说明见
`skill/verification-scripts/bsp-net-check-reference.md`。

## SD 应用与模型包（不重编固件）

- SD 应用编译封包：`python tools/sdapp/sdapp.py build src/sd/apps/<名> --name "图标名"`
  （需先 `sdapp.py export` 导出固件符号表）；
- 模型封装：`tools/nnmod/nnmod.py`；推板：`tools/netpush/net_push.py`（标准 TFTP）。
- 完整工作流见 `src/vitis_workspace/cam_lcd_hdmi_servo_net/tools/README.md` 与
  `board/run/push_examples.md`。
