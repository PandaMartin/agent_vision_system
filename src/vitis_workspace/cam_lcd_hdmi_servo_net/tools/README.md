# tools —— PC 端工具集

固件本体在 Vitis 里构建; 这里只放 PC 端工具, **按功能分三组 + 一个图形入口**,
所有命令都在固件组件根(`vitis/cam_lcd_hdmi_servo_net/`)下运行。

## 目录结构(每组 = 一件事, 与GUI三个页面一一对应)

```
tools/
├── gui.py             图形工具箱: python tools/gui.py
│                        左侧导航 = 网口推送 / SD应用 / 字库 (即下面三组)
├── netpush/           网口推送安装(装应用/模型/任意文件都走它)
│   └── net_push.py      CLI: python tools/netpush/net_push.py <文件>...
├── sdapp/             SD应用开发套件(固件Build后用)
│   ├── sdapp.py         唯一入口, 两个子命令: export / build
│   ├── app.ld           应用链接脚本(静态输入, 不变)
│   ├── fw_exports.ld    符号=地址表        ┐ export的产物
│   └── fw_exports.json  版本号             ┘ (固件每次Build后重写)
└── font/              界面字库维护(改了界面文案才用)
    ├── ui_chars.js       扫描src里的汉字+标点 → 字符集
    └── ui_symbols.txt    上次收集的字符集(字体生成脚本的输入)
```

**不想记命令就开 GUI**:`python tools/gui.py` —— 左侧导航三页对应下面三组工作流,
点按钮即可(后台跑同样的命令, 日志显示在页面里)。"网口推送"页分三种模式,
板端路径自动填好:

- **推送模型**: 填模型名 → 选该模型的全部文件 → 自动推到 `0:/models/<名>/`
  (目录板端自动建, 推理应用自动列出)
- **推送应用**: 选应用bin → 可改"板上文件名" → 自动推到 `0:/apps/<名>.bin`
  (桌面图标名在编译时封进bin头, 不在这里改)
- **推送文件**: 任意文件, 目标目录已预填 `0:/` 前缀

## 工作流与命令

### A. 开发/更新一个SD应用(最常用)

```
①【Vitis】 Build固件(动了固件代码时; 只改应用不用)
②【终端】  python tools/sdapp/sdapp.py export
            固件符号表+版本号刷新; 打印NOTE就再Build一次固件再export,
            直到无NOTE
③【终端】  python tools/sdapp/sdapp.py build src/sd/apps/<名> --name "图标名"
            -> src/sd/apps/<名>/<名>.bin
④【终端】  python tools/netpush/net_push.py src/sd/apps/<名>/<名>.bin
            板端先打开"文件传输"应用; 打印"完成"=成功, 桌面当场出图标
```

GUI等价操作: "SD应用"页 导出符号表→编译封包(勾选加入推送队列, 自动按
"推送应用"模式填好 0:/apps/ 路径)→"网口推送"页点推送。

### B. 推其他文件(模型/照片等)

```bash
python tools/netpush/net_push.py --dir 0:/models/yolo yolo.param yolo.bin
```

### C. 界面加了新汉字/中文标点(字库补字)

```bash
node tools/font/ui_chars.js > tools/font/ui_symbols.txt
# 再按 src/docs/应用开发指南 第四章用 lv_font_conv 重新生成 font_cn_*.c
# (GUI"字库"页 = 这两步各一个按钮)
```

## 产物与再生时机

| 产物 | 生成者 | 何时变 |
|---|---|---|
| `sdapp/fw_exports.ld` `sdapp/fw_exports.json` | `sdapp.py export` | 固件每次Build后必跑(旧版对不上=应用被拒载) |
| `src/sw/app/fw_version_gen.h` | `sdapp.py export` | 同上(编译进固件的版本锚点) |
| `src/sd/apps/<名>/<名>.bin` | `sdapp.py build` | 改了应用代码 |
| `src/sd/apps/<名>/build/` | `sdapp.py build` | 编译中间件, 随时可删 |
| `font/ui_symbols.txt` | `ui_chars.js` | 界面文案有增删后(汉字与，。…等标点自动收集) |
| `src/sw/impl/lvgl/fonts/font_cn_*.c` | lv_font_conv(GUI字库页) | 字符集变化后; 生成完回Vitis Build |

## 版本校验为什么这么严

SD应用是裸机器码, 直接按地址调固件函数; 固件重链后地址几乎必变。
`fw_exports.json` / `fw_version_gen.h` / 应用bin头 三方的版本号一致才放行,
不一致=拒载并toast —— 只会误拒不会误载。所以固件一变, ②③④必须整条重走。

## 历史注记

- 原 `fw_exports_gen.py` + `build_app.py` 已合并为 `sdapp/sdapp.py` 的
  `export`/`build` 子命令(逻辑未变); 原 `src/tools/`(17 布局)与串口推送
  `uart_put.py` 已退役删除, 安装一律走网口 `netpush/net_push.py`。
