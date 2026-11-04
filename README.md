# 智能体驱动可配置视觉处理系统（Zynq-7020）

> Agent-Driven Configurable Vision Processing System on Zynq-7020

一句话：把"换检测模型 / 上线一个新应用"从**改固件—重编译—重烧写**收敛为**推一个文件**——
固件退化为与模型无关的通用推理运行时，模型知识（model.mod + 权重 + labels）与应用软件
（SD 应用二进制）都是 SD 卡上可替换的文件；一个对话式上位机智能体把
「训练 → 导出 ncnn → 量化审阅 → 封包 → 网口推板 → 应用交叉编译」整条链路自动化。

## 系统组成（两份工程，一个系统）

| 组成 | 位置 | 说明 |
|---|---|---|
| **PL 硬件平台** | `src/vivado_project/` | Vivado 2025.2 工程：OV5640 采集 → VDMA 三帧环 → HDMI（75 MHz 像素 / 375 MHz TMDS）+ LCD（33.333 MHz）双路显示；17 个 IP，含 3 个自定义 IP（ip_repo 随包） |
| **PS 固件** | `src/vitis_workspace/` | Vitis 2025.2 + FreeRTOS 五任务 + lwIP + LVGL 桌面 + ncnn 推理运行时；驱动—实现—应用三层单向依赖 |
| **上位机智能体** | `src/host_agent/` | 训练/导出/量化审阅/封包/推送/SD 应用编译全链路 Python 源码 + agent 技能文件族（`skills/host-agent/`）+ 图形工具箱 |

**核心创新**（详见 `report/design_report.md` §1.3）：模型即文件（加模型不改固件）、
应用即文件（64 字节头校验后零重定位执行）、软硬协同视频主干（像素流不经 PS）、
检测—控制异步闭环（检测低频 / 云台控制 20 Hz，数据桥解耦 + 前馈补偿）。

## 仓库结构（官方推荐结构逐目录落实）

```
agent_vision_system/
├── README.md          # 本文件：项目简介 + 复现步骤 + 目录对照
├── src/               # 设计源码
│   ├── vivado_project/18_cam_lcd_hdmi_servo_net/   # PL：Vivado 工程（xpr/BD/XDC/ip_repo/runs 产物）
│   ├── vitis_workspace/                            # PS：固件组件 + FreeRtos_Platform 平台 + 参考应用
│   └── host_agent/                                 # 上位机智能体（train/tools/skills/apps/models 等）
├── sim/               # 采集 IP RTL 自校验仿真（一键复跑约 10 秒，6 项 CHECK PASS）
├── build/             # 可复现构建脚本（rebuild_fpga.tcl + 固件/上位机构建说明）+ 综合与实现报告
├── board/             # 上板工程（XSA + bit/fsbl/elf 下载组）、运行脚本（JTAG/TFTP）与实测输出
├── data/              # 测试数据（MNIST）与参考结果（模型包/精度/审阅规则/实测口径）
├── skill/             # 技能包：提示词工作流/案例模板/校验脚本/踩坑清单 + 通用 PYNQ 技能
└── report/            # 设计报告 + 大模型协作记录（README 内含七项要求对照）
```

### 官方要求 → 本仓库对照

| 官方提交要求 | 本仓库位置 |
|---|---|
| RTL 源码 | `src/vivado_project/.../18_cam_lcd_hdmi_servo_net.srcs/`（BD + 约束）+ `ip_repo/`（自定义 IP） |
| PS 侧软件 | `src/vitis_workspace/cam_lcd_hdmi_servo_net/src/`（hw/sw 分层） |
| 上位机软件 | `src/host_agent/` |
| 仿真或验证结果 | `sim/`（RTL 仿真）+ `board/measured_outputs.md`（上板实测） |
| 综合与实现报告 | `build/impl_reports/` + `build/impl_summary.md`（LUT 17.68%、WNS +0.683 ns、2.064 W） |
| 上板工程与实测比对 | `board/`（XSA + 固件下载组 + 运行脚本 + 实测记录） |
| 可复现构建脚本 | `build/rebuild_fpga.tcl`（PL）、`build/build_firmware.md`（PS）、`build/build_host_agent.md`（上位机） |
| 测试数据与参考结果 | `data/` |
| 技能包 | `skill/`（README 含每项适用范围/使用方法/失效条件） |
| 设计报告 + 大模型协作记录 | `report/` |

## 复现步骤（从零开始）

环境：Vivado/Vitis **2025.2**（实现报告所载版本）、Python 3.x、FAT32 SD 卡、直连网线。
逐步判据与细节见 `report/design_report.md` §7（九步完整版）；主干如下：

```bash
# ① PL 重建（约 20–40 分钟；也可直接用 board/firmware/system_wrapper.bit）
vivado -mode batch -source build/rebuild_fpga.tcl -tclargs <仓库根>

# ②（可选，约 10 秒）采集 IP RTL 仿真：预期 6 项 CHECK PASS + TB_PASS
cd sim && vivado -mode batch -source run_sim.tcl

# ③ PS 固件：Vitis 打开 src/vitis_workspace/，Build cam_lcd_hdmi_servo_net 组件
#    ⚠ BSP 含两处 libsrc 手改（lwIP 超时宏 / PHY 自协商等待），平台"重新生成"后必须：
python tools/bsp_net_check.py fix && python tools/bsp_net_check.py check

# ④ 上板：board/run/jtag_download.md 三条路径任选（bit + fsbl.elf + 应用 elf）

# ⑤ 网络连通：PC 配 192.168.1.100/24 直连板卡 192.168.1.10，板端打开「文件传输」

# ⑥ 推模型/应用（标准 TFTP，任意标准客户端等价）：
python tools/netpush/net_push.py --dir 0:/models/mnist_cnn_fp32 \
       data/model_packs/mnist_cnn_fp32/{model.param.bin,model.bin,labels.txt,model.mod}

# ⑦ 验证：桌面出应用图标；模型自检位图；推理应用四面板出框与耗时；
#    参考结果比对：board/measured_outputs.md + data/reference_results.md
```

SD 应用从零开发（不打开 IDE）：`python tools/sdapp/sdapp.py export` →
`build src/sd/apps/<名> --name "图标名"` → 推送（见 `board/run/push_examples.md`）。

## 提交合规说明

- 全部目录/文件名为纯英文小写+数字+下划线/连字符（脚本核验通过），中文仅在正文内容中；
- 开发期中文文件名已重命名并保留对照：`平台网络配置记录.md→platform_network_config.md`、
  `应用开发指南.md→app_dev_guide.md`、`推理应用设计方案.md→inference_app_design.md`、
  `模型平台化设计.md→model_platform_design.md`（固件侧与上位机侧同名文档内容一致）；
- **体积治理（提交包 ≤200MB，如实声明全部裁剪项，均为可再生或重复内容）**：
  1. 上位机 PyInstaller 运行时分发（`_internal/` 数 GB）、`prebuilt/` 工具链（387MB）、两个 exe 不随包
     （获取方式见 `build/build_host_agent.md`）；
  2. Vivado 可再生中间检查点不随包：`synth_1/system_wrapper.dcp`、`impl_1/{opt,placed,physopt}.dcp`、
     `utils_1/imports/`（从他机导入的同内容 dcp）——重新跑实现即再生；**保留**布线后
     `system_wrapper_routed.dcp`（10.5MB，最终网表）；
  3. 重复副本去重：上位机 `models_sd/`（= `data/model_packs/`）、上位机 `firmware/`
     （= `board/firmware/`）、模型库内与 `model.bin` 逐字节相同的原始权重
     （`hands_fp32.bin`、`wider_face_fp32.bin`×2，MD5 已核验相同）、
     `wider_face_retinaface_fp32/model_mem.cpp`（由权重经 `train/bin2c.py` 生成的 C 数组，可再生）；
  4. `data/datasets/mnist/images/`（3 万张 PNG 展开图）不随包——训练/评测以 `mnist.npz` 为准，
     `train/datasets.py` 从 npz 加载；`data/model_packs/wider_face_ssdlite_int8.rar`（与
     `wd_face_ssd_int8/` 目录同内容）不随包；
  5. 人脸检测 INT8 包的 fp32 审阅基线权重（11MB）不随包——重跑 `train/train.py` 即可再生
     （审阅规则见 `data/reference_results.md` §2）。
  重新生成被裁剪内容的入口：PL 产物 `build/rebuild_fpga.tcl`；模型权重 `train/train.py` +
  `train/export_ncnn.py`；数据集图片 `train/datasets.py`。

## 已知事项（如实声明）

1. **rgb2lcd IP 已随包补全**（2026-11-04）：RTL 源取自工程生成缓存（与设计实际编译的源一致）；
   IP 包装（component.xml 与 lcd 总线定义）按 IP-XACT 依据 BD 记录的端口/接口/极性重建，
   VLNV `www.alientek.com:user:rgb2lcd:1.0` 与工程引用一致；`ip_repo/ov5640_capture_data`
   同步替换为与 BD 视频接口匹配的版本（原备份为旧 cmos_* 并口接口，与 BD 不符）。更新后的
   ip_repo 已在 Vivado 中实例化验证：三个自定义 IP（DVI_Transmitter / ov5640_capture_data /
   rgb2lcd）全部创建成功，引脚与接口数和模块定义逐一吻合。
2. 上位机聊天界面（PyQt5 UI 层）源码未包含在本快照（仅随打包机构建）；
   训练/导出/封包/推送/编译全链路源码与 agent 技能文件齐备，见 `build/build_host_agent.md`。
3. 固件 BSP 两处 libsrc 手改会被平台"重新生成"冲掉，恢复纪律见 `build/build_firmware.md`。
4. 全系统偶发堆写坏事件仍在排查（嫌疑开机 DMA/网络收包路径，工具已固化，未结案）——
   记录见 `report/llm_collaboration_log.md` 第 8 条与 `model_platform_design.md` 头部。

## 更多入口

- 设计报告（七项要求逐项对应，配图 14 幅）：`report/design_report.md`（Markdown）/ `report/design_report.docx`（Word 版，22 页）
- 大模型协作与智能体工作流：`report/llm_collaboration_log.md`
- 技能包（含通用 PYNQ 技能）：`skill/README.md`
- 完整提交自检：`python skill/verification-scripts/repo_integrity_check.py <仓库根>`
