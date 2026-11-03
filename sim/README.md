# sim —— 自定义采集 IP 的 RTL 功能仿真（可一键复跑）

对工程三个自定义 IP 之一的 `ov5640_capture_data`（DVP 8bit 并口 → RGB888 视频流，
内含 8→16bit 字节配对、RGB565→RGB888 位扩展、10 帧稳定等待、ISP 四级旁路链）做
纯 RTL 功能仿真。**独立 Vivado 工程，不改动主工程任何文件。**

## 一键运行

```bash
cd sim
vivado -mode batch -source run_sim.tcl        # 任意 202x.x 版本；产物在 build/
```

运行约 10 秒；结论打印 `CHECK C1..C6` 与 `TB_PASS`，完整日志在
`build/ov5640_cap_sim.sim/sim_1/behav/xsim/simulate.log`，结论摘录见 `sim_result.txt`。

## 最近一次运行结果（Vivado 2026.1 xsim, 2026-10-30）

```
CHECK C1 PASS: cam_rst_n=1, cam_pwdn=0
CHECK C2 PASS: cmos_frame_clk == cam_pclk
CHECK C3 PASS: wait_done 前无输出 (de_before_wait=0)
CHECK C4 PASS: 每帧 64 像素配对, 共 192 (= 3 帧 x 64)
CHECK C5 PASS: 192 像素 RGB565->RGB888 扩展逐像素一致
CHECK C6 PASS: 3 个输出帧端口序列逐点一致 (每帧 56 选通, 行主序窗口)
TB_PASS: all 6 checks passed (frames_in=13, pixels_checked=192)
```

## 验证项与判据

| 检查 | 内容 | 判据 |
|---|---|---|
| C1 | 常量输出 | cam_rst_n=1、cam_pwdn=0（模块不复位/休眠摄像头） |
| C2 | 时钟传递 | cmos_frame_clk 与 cam_pclk 同相 |
| C3 | 稳定等待 | wait_done 置位前（前 10 帧）无任何数据输出 |
| C4 | 字节配对 | 每帧恰好 ROWS×PIX 次配对（4×16，共 3 输出帧） |
| C5 | 数据通路 | 192 像素逐位比对：RGB565 拼装与 RGB888 位扩展全部一致 |
| C6 | 端口端到端 | 输出使能窗口(de∩ce)序列与行主序像素序列逐点一致 |

激励：模拟 OV5640 DVP 时序（首字节与 href 同拍、行/场消隐），每像素发
{高字节=行号, 低字节=列号}，期望 RGB888 = {r[7:3],000, r[2:0],p[7:5],00, p[4:0],000}。

## 两点如实说明

1. **测试平台开发中踩过的坑**（对复用者有价值）：初版激励在 href 抬高后隔一拍才
   送首字节，字节相位整体错位一轮——真实传感器数据与 href 同拍有效，修正后配对
   即正确。该问题由仿真自查发现，也是"用仿真锁定接口时序契约"的直接例证。
2. **端口窗口特性**：isp_en=0（ISP 全旁路）时，输出使能窗口每行覆盖前 PIX-2 个
   像素、与消隐宽度无关；核心配对与数值正确性由 C4/C5 在装配点逐像素精确证明，
   端口侧 C6 验证传输序列。系统级全链路正确性另由上板实测确认（见设计报告）。

## 文件

```
sim/
├── run_sim.tcl                一键脚本（建独立工程→编译→运行→收尾）
├── tb_ov5640_capture_data.v   自校验测试平台（6 项检查）
├── sim_result.txt             最近一次运行结论摘录
├── README.md                  本文件
└── build/                     仿真工程与完整日志（可删，删后重跑即再生）
```
