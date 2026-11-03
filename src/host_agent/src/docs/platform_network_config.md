# 平台网络配置记录（BSP 手改项备案 + 重建恢复指南）

lwIP 网络栈跑在 **FreeRtos_Platform 平台的 BSP** 里，本页备案所有"平台侧"
的非默认配置。其中 2 项是 UI 参数（重建自动保留），2 处是 libsrc 手改
（平台"**重新生成**"会冲掉，必须按本文恢复）；另有 1 行重要注记：
**应用代码(src/sw/**)不受平台重建影响**，但本工程无版本管理，应用侧改动
也在文末一并备案。

> 来历：2026-10 网口推送实测仅 77KB/s，逐层排查出窗口/收包方式/自协商
> 三处问题后固化成现状。核对入口：`python tools/bsp_net_check.py`。

## 一、当前配置清单

### UI 参数（lwip220 组件，重建自动保留，重建后核对即可）

| 参数 | 值 | 作用 | 缺失/改错后果 |
|---|---|---|---|
| `lwip220_tcp_wnd` | **11680**（8×MSS） | TCP 接收窗口 | 出厂 2048：窗口过小，实测推送仅 ~77KB/s |
| `lwip220_temac_phy_link_speed` | **CONFIG_LINKSPEED_AUTODETECT** | PHY 自协商（千兆） | 见下"勿设强制速率" |

**勿设强制速率**（实测教训，2026-10-20）：
- `CONFIG_LINKSPEED100`：板端 PHY（RTL8211 系）软件强制后与 PC 千兆网卡
  并行检测握不上手，**链路起不来**
- `CONFIG_LINKSPEED1000`：千兆铜缆标准强制要求自协商，单端强制必失败
- AUTO 时千兆自协商在 20 秒补丁窗口内可靠完成，无需强制

### libsrc 手改项（平台"重新生成"会丢失，需恢复）

基目录：`vitis/FreeRtos_Platform/ps7_cortexa9_0/freertos_ps7_cortexa9_0/bsp/libsrc/lwip220/src/lwip-2.2.0/`

#### 1. LWIP_SO_RCVTIMEO（2026-10-20 加）

- 文件：`contrib/ports/xilinx/include/lwipopts.h.in`
- 改动：`#cmakedefine TCP_WND @TCP_WND@` 行**之前**加一行
  `#define LWIP_SO_RCVTIMEO 1`
- 作用：打开 socket 接收超时选项。应用收包路径"阻塞 recv + SO_RCVTIMEO
  (400ms)"依赖它——数据一到立即醒（吞吐不打折），无数据 400ms 醒一次检查
  退出/死链（退出应用 ≤0.4s 中止会话，30s 判死链）
- 降级：缺失时 setsockopt 失败，应用**自动回退**"非阻塞+20ms 轮询"，
  能用但速度明显下降
- 恢复：`python tools/bsp_net_check.py fix`

#### 2. PHY 自协商等待 5s → 20s（项目早期补丁，早于本次工作）

- 文件：`contrib/ports/xilinx/netif/xemacpsif_physpeed.c`
- 改动：自协商等待循环 `timeout_counter == 5` 改 `== 20`（共 4 处；
  另 TI SGMII 分支 1 处 `== 30` 为原样，不动）
- 作用：千兆 RGMII 双端自协商常超 5 秒，超时即 `xemac_add` 失败 →
  **开机网络彻底起不来（须重启）**
- 降级：无降级——缺了就是开机偶发无网络
- 恢复：`python tools/bsp_net_check.py fix`

## 二、平台重建后核对流程

1. Vitis 重新生成平台后，固件组件根目录运行：
   ```
   python tools/bsp_net_check.py
   ```
   全 PASS 即完成；有 FAIL 跑 `python tools/bsp_net_check.py fix`
   （自动恢复两处 libsrc 手改，原文件备份 .bak）
2. 重编 BSP 库并同步产物（fix 模式结尾会打印完整命令）：
   `gen_bsp` 下 ninja → 把 `liblwip220.a`、`lwipopts.h` 各拷贝到
   `bsp/lib|include` 与 `export/.../lib|include` 两份
3. Vitis Build 应用固件
4. 上板验证：传输应用显示"等待PC连接"（开机先"网络初始化…"属正常，
   自协商需要几秒），`ping 192.168.1.10` 通即恢复完成

## 三、各项作用与降级行为速查

| 配置 | 在 | 不在 |
|---|---|---|
| TCP_WND=11680 | 推送 MB/s 级 | ~77KB/s（窗口 2048 ÷ 26ms 周期） |
| LWIP_SO_RCVTIMEO | 阻塞收包全速 + 可中止 | 自动回退轮询，速度减半以上 |
| 自协商 20s | 开机网络可靠 | 偶发开机无网络 |
| link_speed=AUTO | 千兆自协商可靠 | 强制 100M/1000M 连不上 PC（实测） |

## 四、应用侧改动备案（2026-10，无版本管理，此处存档）

平台重建不影响以下文件，但改动无 git 记录，列此备查：

| 文件 | 改动 |
|---|---|
| `src/sw/impl/net/net_server.c` | 板 IP 192.168.2.1→192.168.1.10（与 echo 例程同网段）；监听请求"挂起-补开"机制（栈未就绪先 XFER_WAIT，就绪自动开监听）；自适应收包（SO_RCVTIMEO 阻塞优先，失败回退轮询）；实测速率统计；PC 连接/断开事件发布（XFER_CONN）；失败原因中文化 |
| `src/sw/app/bridge.h/.c` | 新增 XFER_WAIT/XFER_CONN 状态、xfer_peer/xfer_res_* 快照字段；pub_xfer 内部落结果区 |
| `src/sw/app/int_app/app_transfer.c` | 传输页 UI 重写：顶卡链路状态（状态点+对端IP+本机地址）、主区待命/进度/结果三面板（结果停留 5s 自动回待命）、底行最近结果常驻 |
| `tools/netpush/net_push.py` | 默认板 IP → 192.168.1.10，文案同步 |
| `tools/font/ui_chars.js` | 增扫 `sw/impl/net`（net 层发布界面文案）；字库按新字符集（341 字）重新生成 |
| `src/sw/app/launcher.c/.h` 等（2026-10-20 二次） | 主页重设计为"卡片仪表盘"：HERO 系统卡（发光弧环+大号运行时长+外设 LED 胶囊）、应用 tile 网格（per-app accent 渐变+按压缩放+进场动画）、SD 应用 dock、超薄状态栏（运行时钟）；壁纸通道（bridge 加 wall_pix/gen_wall + photo_load_wallpaper + main.cpp 开机加载 `0:/theme/wallpaper.bmp`）；launcher.h 加 accent 字段（6 个应用定义同步） |
| `src/main.cpp`、`src/sw/app/README.md`、`src/docs/应用开发指南.md` | IP 引用同步为 192.168.1.10 |

PC 侧约定随之固定：**PC 网卡 192.168.1.100/24，板 192.168.1.10，端口 69（TFTP/UDP）**。
