# 踩坑清单（含触发条件与排查步骤）

每条格式：现象 → 触发条件 → 排查步骤 → 根因/处置 → 出处（工程内可复核）。

## P1 网口推送吞吐与协议演进（两阶段，全部有实测记录）
- 阶段一（自定制 TCP 协议 FWPUT2）：推送实测仅 ~77 KB/s。
  - 触发条件：lwIP 出厂 TCP 窗口（2048）+ 轮询收包 + 强制链速。
  - 排查步骤：确认单连接吞吐量级 → 查 `lwip220_tcp_wnd` → 查收包路径 → 查 PHY 协商。
  - 处置：窗口 11680（8×MSS）；阻塞 recv + SO_RCVTIMEO(400ms)（宏缺失自动回退轮询）；链速一律 AUTO。优化后 MB/s 量级。
- 阶段二（重构为标准 TFTP，现行方案）：
  - 触发条件：TCP 方案真机暴露 accept() 永久阻塞、TIME_WAIT PCB 堆积耗尽连接资源。
  - 处置：改为标准 TFTP（RFC 1350，UDP 69，仅 WRQ，512B 锁步）；吞吐降至数百 KB/s 量级，换取无连接挂死 + 任意标准客户端可用（net_server.c 头注释、net_push.py 文档）。
- 出处：`src/docs/平台网络配置记录.md`、`src/sw/impl/net/net_server.c` 头注释、`tools/netpush/net_push.py`；核对脚本 `tools/bsp_net_check.py`。

## P2 Vitis IDE 多次清空 CMakeLists.txt
- 触发条件：IDE 内 retarget / 添加源文件等操作后。
- 排查步骤：Build 突发大面积编译错误或 LVGL 缺失时，先 diff `CMakeLists.txt`。
- 根因/处置：自定义配置只放 `UserConfig.cmake` 尾部"END OF USER SETTINGS"以下及独立 .cmake（如 lvgl_sources.cmake）；被清空后 `git restore` 恢复；源文件增删一律手编配置文件。
- 出处：`src/docs/应用开发指南.md`（记载 LVGL 整库被删事件）。

## P3 强制 PHY 链速导致链路起不来
- 触发条件：`CONFIG_LINKSPEED100`/`1000` 配合千兆 PC 网卡（RTL8211 系 PHY）。
- 根因/处置：千兆铜缆标准要求自协商，单端强制必失败；一律 AUTO（20 秒补丁窗口内可靠完成）。
- 出处：`src/docs/平台网络配置记录.md`（2026-10-20 实测教训）。

## P4 全系统偶发挂死（堆被写坏，未结案）
- 触发条件：非确定复现（同一固件时好时坏）；遍历类堆操作（大 malloc/free 合并/全堆 walk）必触发冻结。
- 排查步骤：`mallinfo` / `_sbrk(0)` / `__malloc_lock` 直调可把问题三分（锁/遍历/分配）；`nnmod` 打探针模块 + 推送即自检，免重烧固件。
- 根因/处置：已定位"堆本身被写坏"，嫌疑开机 DMA / 网络收包路径，写坏者截至文档记录待查；期间模型预处理改纯 C 双线性绕开 ncnn 原语。
- 出处：`src/docs/模型平台化设计.md` 遗留问题节。本条为如实保留的未解决问题，非已修复经验。

## P5 FatFs 并发崩溃风险
- 触发条件：多任务直接调用 FatFs。
- 根因/处置：文件操作收敛到应用任务单线程（"FatFs 单线程纪律"），其他任务经命令总线/数据桥间接访问。
- 出处：技术报告 2.3.1 / src/sw 架构。

## P6 提交工程引用包外 ip_repo，换机不可重建
- 触发条件：Vivado 工程 ip_repo 路径为工程目录之外的相对路径，打包时未随包携带。
- 排查步骤：`repo_integrity_check.py` 直接报告该缺失。
- 根因/处置：把 ip_repo 随包提交，打开工程后在 Settings→IP→Repository 核对路径。本工程实际踩中：包内 .xpr 引用 `../17_cam_lcd_hdmi_servo/ip_repo`（包外路径）。
- 出处：`18_cam_lcd_hdmi_servo_net.xpr`；本技能包 repo_integrity_check.py 实跑输出。
