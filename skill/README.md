# skill —— 技能包说明

本技能包提炼自"智能体驱动可配置视觉处理系统"（Zynq-7020 + Vitis/FreeRTOS + lwIP + ncnn）的真实开发过程。
每项均写明适用范围、使用方法、已验证效果与失效条件；中文仅出现在文件内容中，文件名与目录名均为英文。

## 目录

```
skill/
├── prompt-workflows/        提示词工作流（3 项，均对应本工程真实产出）
│   ├── 01-xdc-constraint-audit.md      约束文件完整性审查
│   ├── 02-citation-verification.md     引用文献逐条溯源
│   └── 03-diagram-recognition-guard.md 框图/截图识别的幻觉防护
├── case-templates/          案例模板（2 项，指向工程内已上板成例）
│   ├── sd-app-skeleton.md              SD 应用开发骨架与工具链
│   └── model-pack-structure.md         模型包结构与接入流程
├── verification-scripts/    校验脚本（2 项）
│   ├── repo_integrity_check.py         提交包完整性校验（对本包实跑通过）
│   └── bsp-net-check-reference.md      平台网络配置核对（工程自带工具的使用说明）
├── pynq-generic/            通用 PYNQ 技能（加分项，不与本队题目绑定）
│   └── pynq-model-deployment.md        PYNQ 模型部署六步工作流（换题/换板可用）
└── pitfalls/
    └── pitfalls-and-triage.md          踩坑清单（6 条，含触发条件/排查步骤/出处）
```

另：工程内 `src/host_agent/skills/host-agent/`（8 个文件）为**面向 agent 的技能文件族**
（本项目上位机智能体的工作流设计，与人机提示词协作同源），可视作"智能体技能文件"的成例模板，
说明见 `report/llm_collaboration_log.md` 末节。

## 适用范围与边界声明

- 本包主体面向技术栈：Zynq-7000（xc7z020）+ Vivado/Vitis 2025.2 + FreeRTOS + lwIP 2.2 + ncnn；
  换到其他裸机/RTOS SDK 流程时，涉及板级外设的条目（PHY 型号 RTL8211、OV5640、正点原子屏等）
  已在条目内标注，换板需替换对应环节。
- `pynq-generic/` 为**通用 PYNQ 技能**：面向 PYNQ 框架、不与本队题目绑定（换题目、换板卡仍可用），
  以申报 PYNQ 通用加分项。其主干环节（量化审阅/封包/实测口径）在本队 Zynq 目标验证，
  PYNQ 特有命令按官方文档整理并在文内逐条标注实测状态（★ 未实测环节需首用时按检查单确认）。
- 固件架构类经验（三层单向依赖、命令总线/数据桥、模型/应用双文件形态）为通用经验，换题目可直接参考。

## 已验证效果（摘要）

- `repo_integrity_check.py` 两次实跑：①对旧扁平布局提交包正确检出 1 项致命缺失（ip_repo 外链指向包外路径）与 11 处中文命名；②2026-11-03 对本仓库（官方推荐结构布局）实跑：**无致命缺失、无命名违规**（脚本已升级为同时兼容两种布局）。
- `tools/bsp_net_check.py`（工程自带，非本包新增）已用于核对 libsrc 手改项，对应网络推送两阶段演进的实测记录（TCP 调优 77KB/s→MB/s 量级，后重构为标准 TFTP），出处见 pitfalls 条目 P1。
- `extract_impl_summary.py` 对本提交包实跑：从 impl_1 四份报告一键提取资源/时序/布线/功耗为 Markdown 表（2026-11-03 实跑输出已固化为本仓库 `build/impl_summary.md`；样例见同目录 _sample_output.md）。
- `sim/`（仓库根）为自定义采集 IP 的一键 RTL 仿真骨架：run_sim.tcl 复跑约 10 秒，6 项自校验全部通过（sim_result.txt）。
- 提示词工作流三项分别产出：55 项引脚约束表（技术报告表 3）、5 篇可溯源文献（其中 2 篇在补核后才保留）、与 system.bd 完全对齐的 17 模块框图标注（设计报告图 2）。
