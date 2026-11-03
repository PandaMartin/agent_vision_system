# 仓库分层架构（18_net 迁移后）

```
src/
  main.cpp          装配点: 核心初始化(LCD链/触摸/SD/桌面) + 业务任务分发 (允许触碰hw/port_board)
  hw/               板级支持包, 依赖严格单向向下:
    mem_layout.h      板级内存图(固定地址)
    bus/              EMIO GPIO唯一实例 + 参数化GPIO模拟I2C引擎
    ip/               Xilinx IP寄存器驱动: vdma / vtc_api / clk_wiz / lcd_modes / axi_gpio
    dev/              器件驱动(挂bus): lcd_panel / ov5640 / touch(gt9147/gt9271/ft5206) / servo(云台TTC PWM)
    chain/            链路编排: lcd_chain(核心) / cam_chain(可选子系统, start/stop)
  sw/
    impl/            实现层(纯逻辑, PC可编):
      port/            平台边界: port.h(~10函数) + port_board.c(唯一允许碰Xilinx/hw的sw文件)
      sys/ platform    堆(_sbrk+malloc多任务锁)/计时/延时
      sd/  sd+photo    FatFs读写(含sd_mkdirs逐级建目录) + 拍照BMP (板卡专属)
      lcd/ lcd+lvgl_port+vidwin   帧缓冲绘制原语 / LVGL移植层 / 摄像头LCD窗口
      cam/ cam         帧边界抓取/快照/检测框叠加 (板卡专属)
      nn/  ncnn推理    runner/model_mgr/post_process + ncnn库
      net/ net_server  lwIP220网口文件接收(标准TFTP/UDP 69, 受"文件传输"应用门控)
      lvgl/            LVGL 9.4 + lv_conf.h + fonts/
    app/             固件应用系统:
      appctl           命令总线(gui/net任务->app_task): 各应用命令+APP_CMD_NET_FILE
      bridge           数据桥(多任务发布->gui拉取, 快照+分组代数, 含net/传输组)
      launcher         桌面启动器: 状态栏(SD/相机/网口)+图标网格+应用生命周期
      int_app/         内置应用(一应用一.c/.h, UI+后端同文件):
                       camera(预览/拍照/相册) gimbal(云台) infer(推理, 后端
                       app_infer_be.cpp) files(文件) transfer(文件传输) test
      ext_app/         SD卡加载应用框架: loader(AppHeader/运行时) + sdapp(扫描/搬运启动)
```

## 应用体系（int_app 内置 / ext_app SD加载）

- 系统核心 = 串口打印+LCD链+触摸+SD, 开机初始化常驻；摄像头+HDMI链是可选
  子系统, 开机完全不碰 —— 传感器不在位/无SD/无模型都正常进桌面
- 每个应用独占一个screen: 打开=框架建屏(顶栏返回键+内容区)+on_open填内容
  +滑入动画; 返回=滑回桌面+整屏删除(对象清理框架强制, 应用零负担)
- 后端纪律: 所有应用的业务处理(appctl命令消费)都在app_task上下文执行
  (FatFs唯一使用者), UI回调只投命令不碰SD; main.cpp只做初始化+命令分发
- 新内置应用: 复制 int_app/_template.c 起手, pages.h声明 + launcher注册 +
  UserConfig.cmake登记(见模板文件头)
- SD应用: /apps/*.bin(AppHeader带固件版本), ext_app/sdapp扫描校验后拷入
  0x20000000应用区跳转; 构建/契约见 ext_app/loader.h

## 文件传输（替代17的串口FWPUT）

- 网口lwIP220 SOCKET_API, 板静态IP 192.168.1.10/24, 标准TFTP(UDP 69)只在
  "文件传输"应用打开期间监听(桌面态不监听); 栈常驻, ping随时可用
- 协议: 标准TFTP/RFC1350, 只支持WRQ推送(octet, 512字节锁步, 会话串行);
  文件名"0:/..."原样、裸名挂SD根; PC端 tools/netpush/net_push.py(组件
  tools/目录)或任意标准tftp客户端(tftp -i 192.168.1.10 put <文件>)
- 收满整文件经APP_CMD_NET_FILE交app_task落SD(建父目录+写文件),
  /apps成功后桌面自动出新图标, /models自动刷新模型清单
- 串口只保留打印(接收侧console已删除)

## 依赖铁律（grep 可审计）

1. `impl/` 除 `port_board.c` 外禁止 include 任何 Xilinx/hw 头
   （cam/vidwin/net 经板卡专属路径, 不进PC构建）
2. `hw/**` 禁止 include `sw/**`
3. GUI 路径（lcd绘制/lvgl_port/vidwin）平台调用只经 port.h
4. `sw/app/**` 禁止 include `hw/**`（硬件状态一律经bridge快照;
   int_app后端例外已收敛在各app_xxx.c的dispatch段, 唯一FatFs出口）

## 已知技术债（后续里程碑处理）

- 部分头文件仍含 `xil_types.h`（纯类型定义, 无硬件访问, 待换 stdint）
- vidwin(impl)引用launcher(app)的launcher_active_app判断"应用在场",
  方向倒挂; 后续loader接管应用调度时一并理顺
- lwip TCP_WND为2048(BSP默认), 大文件吞吐受限时可提至16384
  (Vitis平台BSP设置lwip220_tcp_wnd后重建平台)

## 构建说明

UserConfig.cmake 源清单已按 int_app/ext_app 布局登记；构建走 Vitis 工程
（platform = FreeRtos_Platform, domain freertos_ps7_cortexa9_0）。
FreeRTOS堆=128KB(BSP已重建), lwIP MEM_SIZE=128KB, 串口仅打印。
验收: 无摄像头开机进桌面、相机/推理/文件/传输各应用反复进出、
网口推送应用bin后桌面即出图标。
