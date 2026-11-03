# 网口推送运行脚本（标准 TFTP，RFC 1350）

## 前置（缺一不可）

1. PC 网卡（直连网线）配 `192.168.1.100 / 255.255.255.0`（`board_profile.json` 的 `pc_ip_hint`；
   上位机设置页/`configure_board_net` 可自动追加该地址，不改动既有网络）；
2. 板端打开「**文件传输**」应用（传输只在应用内开放，桌面态不监听）；
3. 板端地址默认 `192.168.1.10:69`（UDP，标准 TFTP 端口）。

## 推送命令（固件组件根 `src/vitis_workspace/cam_lcd_hdmi_servo_net/` 下）

```bash
# 推 SD 应用（桌面即时出图标）
python tools/netpush/net_push.py src/sd/apps/snake/snake.bin

# 推模型包四文件（目录板端自动建）
python tools/netpush/net_push.py --dir 0:/models/mnv2_ssdlite_voc \
       model.param.bin model.bin labels.txt model.mod

# 推任意文件
python tools/netpush/net_push.py --dir 0:/photos photo_001.bin
```

数据来源示例：`data/model_packs/`（板端就绪模型包，见 `data/README.md`）、
`src/host_agent/apps/`（示例应用）。

## 任意标准 TFTP 客户端等价命令

```bash
tftp -i 192.168.1.10 put snake.bin        # Windows 自带客户端；裸名落 SD 根 0:/
```

板端在尾块 ACK 后异步落 SD；落盘失败反向补发 TFTP ERROR（PC 侧可见失败原因）。

## 推送后验证

- 应用：桌面出现图标 → 点开运行；
- 模型：推理应用四面板显示实时检测框与耗时；或等模型自检位图（`nn_selftest`，推送即触发）
  确认模型可用；
- 压测：`tools/netpush/stress_models.py` 可批量连续推送（开发期吞吐验证用）。
