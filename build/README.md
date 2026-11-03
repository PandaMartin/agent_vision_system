# build —— 可复现的构建脚本 + 综合与实现报告

| 文件/目录 | 内容 |
|---|---|
| `rebuild_fpga.tcl` | PL 侧一键批量重建（指向 ip_repo → 升级 IP → 综合 → 实现 → 比特流） |
| `build_firmware.md` | PS 侧固件（Vitis）构建步骤，含 BSP 手改项恢复纪律 |
| `build_host_agent.md` | 上位机智能体（训练/封包/推送工具链）安装与构建 |
| `impl_reports/` | 从 `impl_1` 摘出的关键报告（布线后资源/时序/功耗/布线状态/DRC/IO/时钟 + 综合资源） |
| `impl_summary.md` | 由 `skill/verification-scripts/extract_impl_summary.py` 对本包**实跑**机器提取的指标汇总 |

## 实现结果摘要（Vivado 2025.2，布线后）

| 指标 | 数值 |
|---|---|
| LUT | 9407 / 53200（17.68%） |
| FF | 15874 / 106400（14.92%） |
| BRAM | 15 / 140（10.71%） |
| DSP | 0 / 220（0%） |
| 建立时间 WNS / TNS | +0.683 ns / 0.000 ns（0 违例） |
| 保持时间 WHS / THS | +0.050 ns / 0.000 ns（0 违例） |
| 布线 | 21562 网络全部布通，0 错误 |
| 片上总功耗 | 2.064 W（动态 1.909 + 静态 0.155） |

时钟频率（`system.bd` 中 clk_wiz 实配）：LCD 像素 33.333 MHz、HDMI 像素 75 MHz、
TMDS 串行 375 MHz。关键性能指标（推理帧率等）见 `board/measured_outputs.md` 与设计报告 §4。

## 重建入口一览

```bash
# 1) PL：批量重建（约 20–40 分钟，视机器）
vivado -mode batch -source build/rebuild_fpga.tcl -tclargs <仓库根目录>

# 2) （可选）RTL 仿真：约 10 秒，6 项自校验
cd sim && vivado -mode batch -source run_sim.tcl

# 3) PS 固件：见 build_firmware.md（Vitis 2025.2）
# 4) 上位机工具链：见 build_host_agent.md（Python 3）
```
