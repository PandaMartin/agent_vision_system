# 实测输出与参考结果比对

本页汇总**上板实测输出**与参考结果的比对数据；全部数字的工程内出处见文末。
未实测项一律显式标注（技术报告中标记 `__` 的 3 处待回填项不在本页出现）。

## 1. 网络文件推送（两阶段演进，均有实测记录）

**阶段一：自定制 TCP 协议（FWPUT2）调优**

| 优化项 | 优化前 | 优化后 | 效果 |
|---|---|---|---|
| TCP 接收窗口 | 2048（出厂值） | 11680（8×MSS） | 77 KB/s 的主因消除 |
| 收包方式 | 非阻塞 + 20 ms 轮询 | 阻塞 recv + SO_RCVTIMEO(400 ms) | 数据即到即处理 |
| PHY 链速 | 强制 100M/1000M（实测握不上） | 自协商 AUTO | 千兆链路可靠建立 |

结果：推送吞吐由 **约 77 KB/s 提升至 MB/s 量级**（实测记录：`src/.../platform_network_config.md`）。

**阶段二：重构为标准 TFTP（交付形态）**

- 动因：真机暴露 FWPUT2 健壮性问题——`accept()` 永久阻塞、TIME_WAIT 的 PCB 堆积耗尽 lwIP
  连接资源（`net_server.c` 头注释实测记载）；
- 结果：重构为标准 TFTP（RFC 1350，UDP 69，WRQ 方向，512 B 锁步），吞吐回到数百 KB/s 量级
  （512 B 锁步所限），换取两点收益：UDP 无连接从根上消除挂死；PC 侧任意标准 tftp 客户端可推；
- 比对方式：`tftp -i 192.168.1.10 put <file>`（标准客户端）与 `net_push.py` 推送结果一致，
  板端落盘后文件可被推理应用正常装载。

## 2. 神经网络推理（上板实测，2026-10-22 记录）

| 模型包 | 类型 | 单帧耗时 | 帧率 | 输出正确性 |
|---|---|---|---|---|
| mnv2_ssdlite_voc | 检测（fp32） | 239 ms | ≈4.2 fps | 框/类别/坐标正确 |

- 低帧率观感优化：推理期间可关闭实时预览让出带宽，检出框由 30 fps 任务做 IoU 配对 +
  指数平滑插值渲染，消除跳变（工程内实现，非承诺值）；
- 模型自检：推送模型即触发 `nn_selftest` 生成自检位图，免重烧固件确认可用性。

## 3. RTL 仿真（组件级，一键复跑）

`sim/run_sim.tcl`（Vivado 2026.1 xsim，2026-10-30 运行）6 项自校验全部通过：

```
CHECK C1 PASS: cam_rst_n=1, cam_pwdn=0
CHECK C2 PASS: cmos_frame_clk == cam_pclk
CHECK C3 PASS: wait_done 前无输出 (de_before_wait=0)
CHECK C4 PASS: 每帧 64 像素配对, 共 192 (= 3 帧 x 64)
CHECK C5 PASS: 192 像素 RGB565->RGB888 扩展逐像素一致
CHECK C6 PASS: 3 个输出帧端口序列逐点一致
TB_PASS: all 6 checks passed (frames_in=13, pixels_checked=192)
```

## 4. 实现级指标（布线后报告，机器提取）

见 `build/impl_summary.md`（LUT 17.68%、WNS +0.683 ns、0 违例、全布通、2.064 W）；
与文献[2]（同器件 84.65% LUT 换 6.15 fps 单模型）的对比分析见设计报告 §4.3。

## 5. 功能验证判据（评委实测可复打的口径）

| 功能 | 判据 | 状态 |
|---|---|---|
| 双显示通路 | HDMI 与 LCD 同时出摄像头画面（PL 承担，像素不经 PS） | 上板通过 |
| 桌面与应用 | LVGL 桌面出图标，snake 等示例应用可运行 | 上板通过 |
| 模型即文件 | 推模型包 → 自检位图 → 推理应用四面板出框与耗时 | 上板通过 |
| 应用即文件 | sdapp 编译 → TFTP 推送 → 桌面即时出图标 | 上板通过 |
| 人脸跟踪闭环 | 检测输出人脸框，云台 20 Hz PID 跟踪（前馈+自运动补偿） | 上板演示 |
| 网络看门狗 | 传输服务卡死自动恢复（每秒自检） | 工程内实现 |

## 数据出处对照

| 本页表述 | 工程内来源 |
|---|---|
| 网络两阶段与 77 KB/s | `src/vitis_workspace/cam_lcd_hdmi_servo_net/src/docs/platform_network_config.md`、`sw/impl/net/net_server.c`、`tools/netpush/net_push.py` |
| 4.2 fps / 239 ms 实测 | `src/.../src/docs/model_platform_design.md`（2026-10-22 记录） |
| RTL 仿真结论 | `sim/sim_result.txt` + `sim/vivado_run.log` |
| 实现指标 | `build/impl_reports/*.rpt`（impl_1 布线后） |
| 固件与符号版本 | `board/firmware/manifest.json`（symbol_count 5137） |
