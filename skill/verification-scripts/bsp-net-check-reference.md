# 校验脚本说明：平台网络配置核对（工程自带 bsp_net_check.py）

## 适用范围
Vitis 平台 BSP 含 libsrc 手改项（平台"重新生成"会丢失）的工程。本项不是新增脚本，是工程内已有工具的使用说明，作为技能包条目备案。

## 使用方法（固件组件根目录）
```
python tools/bsp_net_check.py check   # 核对两处 libsrc 手改是否在位
python tools/bsp_net_check.py fix     # 自动恢复（LWIP_SO_RCVTIMEO 宏等）
```

## 核对内容（出处：src/docs/平台网络配置记录.md）
1. `lwipopts.h.in`：`LWIP_SO_RCVTIMEO 1`（TCP_WND 定义之前）——阻塞收包路径依赖；
2. `xemacpsif_physpeed.c`：PHY 自协商等待 5s → 20s（项目早期补丁）。

## 已验证效果
该脚本对应 TCP 推送时代（FWPUT2）的调优记录：从 ~77 KB/s 优化到 MB/s 量级，是重建后的核对入口（出处文档"核对入口"原话）。协议重构为 TFTP 后，这些 BSP 配置仍保留在平台中，脚本继续作为平台手改项的核对工具。

## 失效条件
lwIP 版本/目录结构变化后文件路径需更新；仅覆盖网络相关手改，其他 libsrc 手改需自行追加条目。
