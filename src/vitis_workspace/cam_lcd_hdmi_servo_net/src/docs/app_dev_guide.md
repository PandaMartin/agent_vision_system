# 应用开发指南

固件 = 常驻系统(launcher桌面 + 命令/数据管道); 应用有两种形态:
**内置应用**(随固件编译, 第三章)与 **SD应用**(独立bin装进SD卡, 第一/七/八章)。
模板: `sw/app/int_app/_template.c`(内置)、`apps/snake`(SD, 已上板验证)。

**本文命令的工作目录分两层**: 带 `python/node` 的工具命令在**固件组件根**
(`vitis/cam_lcd_hdmi_servo_net/`, 工具集中在 `tools/`, 见 tools/README.md);
其余相对路径以 `src/` 为基准。

## 〇、工具分工: Vitis 与 python 各管什么

| 工具 | 只负责 | 不负责 |
|---|---|---|
| **Vitis IDE** | ①Build 固件(产出 `build/cam_lcd_hdmi_servo_net.elf` + `build/fw.map`)②烧写/运行 elf | SD应用的编译、封包、安装**全都不经过Vitis** |
| **终端 + python** | 固件符号导出、SD应用编译封包、网口推送安装、字体生成 | 不编固件 |

终端里用到的脚本(都在 `vitis/cam_lcd_hdmi_servo_net/tools/`; 已按使用时机排序):

| 何时用 | 命令(组件根) | 作用 |
|---|---|---|
| 固件Build之后 | `python tools/sdapp/sdapp.py export` | 解析 fw.map → 重写符号表 fw_exports.ld + 版本号 |
| 发布SD应用 | `python tools/sdapp/sdapp.py build src/sd/apps/<名> --name "图标名"` | 交叉编译→链接→封64字节头 → `<目录名>.bin` |
| 安装应用 | `python tools/netpush/net_push.py src/sd/apps/<名>/<名>.bin` | 经网口写进板卡SD卡, 桌面当场出图标 |

> 不想记命令就开图形工具箱: `python tools/gui.py` —— 左侧导航三页(网口推送/SD应用/字库)
> 对应下表三个工具, 点按钮即可, 命令行用法不变。网口推送页分"推送模型/
> 推送应用/推送文件"三种模式, 板端路径(0:/models/<名>/ 等)自动填好。

两条纪律:

- **网口推送前置**: 板端打开"文件传输"应用(传输只在应用内开放),
  PC网卡(直连网线)配 192.168.1.100/24; 串口此时只出打印, 无独占问题。
- **别用 Vitis IDE 的"添加源文件"**: 它多次破坏手工维护的 `UserConfig.cmake`
  (LVGL整库被删过)。源文件增删一律手编该文件。
- **构建自定义只放安全区**: `UserConfig.cmake` 的 `END OF USER SETTINGS`
  标记线以下的尾部区, 及其 include 的独立 .cmake 文件(现 lvgl_sources.cmake/
  ncnn_port_post_project.cmake)。`CMakeLists.txt` 是 IDE retarget 的管理
  对象(2026-10起被整文件清空过多次), 除模板内容外不得放任何自定义配置——
  LVGL源注入即因此从CMakeLists.txt迁至lvgl_sources.cmake。IDE把CMakeLists
  清空后 `git restore` 恢复即可, 不丢任何自定义。

## 一、场景A: 开发SD应用(最常用, 全程不碰Vitis)

只写应用、不改固件时不需要重编固件。步骤严格按序:

```
步骤1 写代码     新建 src/sd/apps/<name>/ 目录, 写 .c(骨架见第八章)
步骤2 编译封包   python tools/sdapp/sdapp.py build src/sd/apps/<name> --name "应用名"
                 产物: src/sd/apps/<name>/<name>.bin
步骤3 推送安装   板端打开"文件传输"应用, PC网卡配192.168.1.100后:
                 python tools/netpush/net_push.py src/sd/apps/<name>/<name>.bin
步骤4 上板验证   工具打印"完成"即成功, 桌面当场出现图标(免重启免拔卡)
```

- 桌面图标名 = `--name`(最多15字节UTF-8); 板上文件名 = bin的文件名, 两者无关。
- 重复推送同名bin = 覆盖更新, 不必先删旧文件。
- 步骤2的前置条件 `tools/sdapp/fw_exports.json` 随仓库提交,
  克隆下来即可开发应用; 只有走完场景B它才会变。

常见报错速查:

| 现象 | 原因 / 处理 |
|---|---|
| 连接被拒/超时 | ①板端没开"文件传输"应用(桌面态不监听UDP 69) ②PC网卡不在192.168.1.x网段 ③ping 192.168.1.10不通(网线/固件) |
| `板端拒绝: no .. allowed` 等 | 目标路径含`..`/非ASCII/超长; 用 `--dir 0:/apps` 等SD绝对路径 |
| `ACK超时 (block N)` | 传输中链路断开, 重推即可(整文件事务, 无半文件残留) |
| 图标出现但点击后toast拒载 | 固件与应用版本不配对(bin过期) → 走一遍场景B, 再重推 |
| `app_boot @0x.. must be 0x20000000` | 代码缺 `.app_entry_first` 段的 app_boot(见第八章骨架) |

## 二、场景B: 固件变更流程(Vitis 与 python 交替, 顺序严格)

动了固件代码(内置应用、launcher/bridge/appctl、sw/、hw/、main.cpp)后必须走
完整流程; 后一步吃前一步的产物, 顺序不能乱:

```
①【Vitis】 Build 工程
            → 产出 build/cam_lcd_hdmi_servo_net.elf 和 build/fw.map
②【终端】  python tools/sdapp/sdapp.py export
            解析fw.map, 重写 tools/sdapp/fw_exports.ld(符号=地址) +
            tools/sdapp/fw_exports.json(版本号) + sw/app/fw_version_gen.h
            ├ 打印 "NOTE: version changed ..." → 版本未收敛, 去③
            └ 没打印 NOTE                     → 已收敛, 直接去⑤
③【Vitis】 再 Build 一次
            (把②生成的新版本号编进固件; 版本号变化会挪动符号地址,
             所以可能又引出一轮新版本)
④【终端】  再跑一次 python tools/sdapp/sdapp.py export 确认无 NOTE
            (fw_version_gen.h用定宽字面量, 二轮必收敛;
             若仍打印NOTE, 重复②③④直到无NOTE为止)
⑤【Vitis】 烧写【最近一次Build】的 build/cam_lcd_hdmi_servo_net.elf
⑥【终端】  重编并重推全部已发布的SD应用:
              python tools/sdapp/sdapp.py build src/sd/apps/<每个应用> --name "..."
              python tools/netpush/net_push.py src/sd/apps/<每个应用>/<...>.bin
```

- 为什么必须⑥: 版本号是"固件符号地址快照"的哈希, 固件重链几乎必变;
  旧bin头里的版本对不上, 加载器拒载并toast(**只会误拒不会误载**, 刻意的保护)。
- SD卡里过期的bin可直接用文件应用(删除按钮)清掉。
- 快速判断走哪个场景: 只动了 `apps/` 下的文件 → 场景A;
  动了其它任何源文件 → 场景B。

## 三、内置应用: 四步接入(做完必走场景B)

1. 复制 `sw/app/int_app/_template.c` → `app_<name>.c`, 实现 `app_xxx` 实例
   (UI+后端同文件: 后端=dispatch函数, 跑在app_task, 见app_files.c范式)
2. `int_app/pages.h` 加 `extern const LauncherApp app_xxx;`
3. `launcher.c` 的 `launcher_init()` 里 `launcher_register(&app_xxx);`
4. `UserConfig.cmake` 源清单加一行(**手编文件, 别用Vitis IDE添加**)

内置应用是固件的一部分, 之后必须走第二章完整流程才能上板。

## 四、UI铁律(踩过的坑, 违反即控件消失/错乱)

| 规则 | 原因 |
|---|---|
| 控件只用固定坐标/尺寸(内容区784x424) | LVGL布局惰性, 运行期读尺寸得0 |
| 不调用 lv_obj_get_width/height(除自己set过的) | 同上 |
| 不用 lv_pct() 参与像素加减 | pct是编码值, 算术即错位 |
| lv_obj_set_flex_align 第1参=主轴 | SPACE_BETWEEN放第3参不生效 |
| 纯容器 remove CLICKABLE\|SCROLLABLE | 默认可点击会吞触摸(桌面图标bug) |
| 新汉字先入字库 | 缺字画空白: 见下方字体流程 |

新汉字入字库(改了界面文案必做; 产出的font_cn_*.c是固件源码, 完事走场景B):
①②在组件根跑(ui_chars.js会自动扫描src下的全部文案,
其中"传/输/监"等文件传输应用的新字已自动覆盖)

```
①【终端】在 tools/font/ui_chars.js 末尾的 extra 字符串里补上新汉字(组件根)
②【终端】node tools/font/ui_chars.js > tools/font/ui_symbols.txt
③【终端】cd src/sw/impl/lvgl/fonts 后执行
   (两档字号各来一次; symbols路径五级上跳到组件tools/):
   npx lv_font_conv --no-compress --bpp 4 --size 16 --font C:/Windows/Fonts/simhei.ttf \
     --range 0x20-0x7E --symbols "$(cat ../../../../../tools/font/ui_symbols.txt)" \
     --format lvgl --lv-include lvgl.h -o font_cn_16.c
   (font_cn_20 同理, 只改 --size 20 和 -o)
```

省事办法: 文案尽量只用 `tools/font/ui_symbols.txt` 里已有的字。

## 五、数据流(重活不走GUI线程)

```
GUI应用            appctl命令总线        app_task(main.cpp)        bridge数据桥
  |  post(命令)  ->  |                    |                         |
  |                  |    ->  执行(SD/相机/推理, FatFs只在此线程)     |
  |                  |                    |  ->  pub_xxx发布快照 ->  |
  |  refresh()比对代数, 变了才重绘  <------------- snapshot拉取 ------|
```

- 命令: `sw/app/appctl.h` 加枚举(AppCmd可内嵌48字节路径+void*指针)
- 数据: `sw/app/bridge.h` 加快照字段+分组代数gen_xxx+pub函数
- 范式参考: `int_app/app_files.c`(FS_LIST全流程)、`int_app/app_transfer.c`
  (含net任务参与的第三生产者)

## 六、生命周期契约

- `on_open(parent)`: parent=应用屏内容区(784x424), 建界面; 可post初始化命令
- `refresh()`: 只读bridge快照, 按gen_xxx代数增量重绘(勿轮询硬件)
- `on_close()`: 只释放非LVGL资源; LVGL对象由launcher整屏删除, 回调里不得再碰
- 应用被删后static变量仍在: on_open需重置全部static状态(参考files.c的
  s_last_gen=0)

## 七、SD应用运行机制(加载/版本/推送协议)

- **安装位置**: `/apps/<名>.bin` 或 `/apps/<目录>/<名>.bin` 均可(扫描两级);
  网口推送默认目录 `0:/apps`(`--dir`可改, 模型用 `--dir 0:/models/<名>`)。
- **加载流程**(app_task): 读bin → 校验64字节AppHeader(magic "ZAPP"+版本号) →
  拷到 0x20000000 → 清bss → 刷D-Cache/失效I-Cache/dsb+isb → gui任务调entry。
- **运行时契约**(`sw/app/ext_app/loader.h`): entry在gui任务上下文、须快速返回;
  定时器用 `app_timer_create`(框架记账, 退出强制回收); 退出调 `fw_app_exit()`
  后立即返回, 不得再碰LVGL对象(框架自动载回桌面并删应用屏);
  每次启动重新拷贝+bss清零, static状态天然复位。
- **版本三方一致才放行**: 固件内嵌版本 == 应用bin头版本 == fw_exports.json(sdapp/下);
  应用链接时的符号地址来自 tools/sdapp/fw_exports.ld —— 换了固件不重编应用 = 地址错位,
  必须拦截。
- **推送协议(标准TFTP, UDP 69, 每文件一会话)**: PC发 WRQ <SD全路径> octet
  到板端(仅"文件传输"应用打开期间监听) → 板自会话临时端口回 ACK(0) →
  512字节DATA/ACK锁步 → 尾块即ACK(客户端视角传输完成), 板收满整文件交
  app_task落SD, 落盘失败补发TFTP ERROR(尽力而为, 最终结果以板面为准),
  落盘成功自动重扫(/apps出图标, /models刷清单)。文件名"0:/..."原样,
  裸名(标准tftp客户端)挂SD根"0:/"下; 只支持推送(WRQ), 下载(RRQ)回错误。

## 八、SD应用代码骨架(apps/snake 为范本)

```c
#include "lvgl.h"
#include "sw/app/ext_app/loader.h"  //AppHeader契约+app_timer/fw_app_exit

void app_main(void) { /*建screen(800x480), lv_screen_load(scr), app_timer_create*/ }

__attribute__((section(".app_entry_first")))
void app_boot(void) { app_main(); } //恒定入口, 链接脚本保证在0x20000000
```

- 可调用全部固件符号(lv_*、appctl/bridge、impl层函数);
- **无libc**: memcpy/strcmp等来自固件导出, 纯C、无构造器;
- UI铁律(第四章)同样适用;
- 编译封包与安装见第一章场景A。

## 九、验收清单(每个新应用上板前)

- [ ] 反复进出10次: 无残影/无泄漏(LVGL堆用量稳定)
- [ ] 触摸全区域可达(控件无遮挡)
- [ ] 推理/相机类: 与桌面切换互不影响
- [ ] 新文案字体完整(无空白方块)
