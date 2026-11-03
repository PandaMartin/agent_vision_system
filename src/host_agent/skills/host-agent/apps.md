# 编写桌面应用

应用跑在固件外面，加载到 `mem_layout.h` 给出的 SD 应用区，按符号表里的地址调用固件里的 LVGL 和退出函数。固件重编后地址会变，旧的 `.bin` 会被板子拒绝。表缺失或刚重编过固件时，先在工具目录执行 `python sdapp/sdapp.py export`，不要编造地址。

## 步骤

1. `query_symbols`，kind 用 both。固件表里要能查到 `app_timer_create`、`fw_app_exit`、`lv_screen_load`。再用 name 检查图标和界面里的每一个汉字。
2. 读 `board_profile.json`，把屏幕宽高写成 `screen_width` 和 `screen_height`。写界面前再读 page=ui，按那一页的字号、颜色和卡片排，不要沿用旧的计数器骨架。
3. 用选项确认目录名、图标名和功能。
4. 用 `save_app_source` 把 `app_main.c` 分段写入 `apps/<目录>/`。第一次 `mode=overwrite`，之后 `mode=append`，每次大约 80 到 120 行。不要把整份源码塞进一次调用，也不要写在对话正文里。编译器由上位机从 `prebuilt/gcc-arm-none-eabi/` 传入，不用在源码里写编译器路径。
5. 文件里出现 `app_boot` 后，调用 `write_board_app`，不要传 `source`。上位机编译磁盘上的文件。
6. 编译失败就用 `save_app_source`，`mode=replace`，只换报错附近的几行，再编译。不要整份重写。工具结果说缺少 source 或被截断时，禁止回答「把完整 app_main.c 写进去」。
7. 用户明确说「文件传输」已打开后，再 `deploy_board_app`。

修改本机 `apps/` 里已有应用时，用户消息里已经给出目录名和当前 `app_main.c`。保持这个 `folder`，先用选项问要改什么，再用 `save_app_source` 的 `replace` 把改动写进文件。不要另建目录，不要整份重写。

板工程自带的 `snake` 和 `face_track` 不要改原文件，也不要沿用这两个目录名。基于给出的源码，在上位机 `apps/<新目录>/` 里另写一份。

## 必须遵守

- 一个 C 文件。头文件只有 `lvgl.h` 和 `sw/app/ext_app/loader.h`。没有 libc，不能用 printf、malloc、free、stdio、stdlib，也不能用 C++。
- 使用 `font_cn_16` 时声明 `extern const lv_font_t font_cn_16`。字库里没有的汉字不要写进图标和界面。
- 入口是 `__attribute__((section(".app_entry_first"))) void app_boot(void) { app_main(); }`。加载器靠这个节找到入口。
- 界面用整数坐标。不用 `lv_pct`，不调用 `lv_obj_get_width` 或 `lv_obj_get_height`。
- 定时器写成 `app_timer_create(回调, 毫秒, 0)`。第一个参数是函数，周期在第二个参数。
- 必须有「退出」：回调里 `fw_app_exit();` 然后立刻 return。
- 必须有一个会被读到的 `static volatile` 变量。空的 bss 会让封包失败。
- 目录名以字母开头，只含字母、数字、下划线，最长见档案 `app_folder_max`。图标名最多 `icon_bytes_max` 字节。

## 界面

版式、字号、配色和控件写法以 page=ui 为准。刻度盘、圆弧、进度条、曲线、滑杆、开关、复选框、指示灯、转圈都可以用。技术约束仍是上面这一节。入口、退出和 `static volatile` 不能省。
