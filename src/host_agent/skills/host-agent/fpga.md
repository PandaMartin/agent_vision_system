# FPGA 工程

当前板的软件工程，是含有 `tools/nnmod/nnmod.py` 的那一层目录。上位机仓库已经带上编译所需的 `src/sw`、`src/hw`、`bsp/include`、`tools/` 和 `firmware/`。设置里的板工程留空时，编译桌面应用和 `model.mod` 用这一份，不要求另一台电脑上另有 Vivado 工程。符号查询优先用仓库这份。固件重新编译并 `sdapp.py export` 之后，对话调用 `adapt_firmware`：`status` 对比版本，`import` 覆盖 `tools/` 和 `firmware/`，`rebuild` 重编本机应用和 `model.mod`，`program` 经 JTAG 下载。左侧「FPGA 代码」有同样的替换和下载按钮。重新上电后要再下载一次。不要把某台电脑的盘符写进说明。不要手改函数地址。

地址以该工程 `src/hw/mem_layout.h` 为准，并与 `board_profile.json` 里的模型区字段对照。换板或改内存图之后，以这两个文件的当前内容为准。

## 工程里有什么

```
<软件工程根>/
  src/sw/          固件，随 Vitis 编进 ELF
    app/int_app/   板子自带的界面，含推理应用
    app/ext_app/   SD 应用的加载器。桌面应用只包含 loader.h
    impl/nn/       通用 ncnn 运行时：装载、前向、发布
    impl/cam|lcd|lvgl|net|sd|sys/   摄像头、显示、界面、网口、SD、系统
  src/hw/          与 PL 相连的地址和设备：mem_layout.h、bus、dev、ip
  src/sd/          不参与固件编译。apps 是桌面应用，models 是模型包
  tools/sdapp/     导出符号表，编译 SD 应用
  tools/nnmod/     把模型语义编译成 model.mod
  tools/netpush/   网口下发
  tools/font/      界面字库字符集
```

`src/sd/apps` 里的 `snake`、`face_track` 是板工程自带应用。上位机新写的应用放在上位机仓库的 `apps/`，不要覆盖这两个目录。

Vivado 的 `.runs`、`.gen` 和 Vitis 的 `build/` 是本机生成物。换电脑打开工程时会重新生成。修改 `src/sw` 或 `src/hw` 之后重新链接固件，要在 Vitis 里做。上位机下载已经编好的镜像时，优先用 `xsct.bat` 或 Vivado 的 `xsdb.bat`；没有这两份时用仓库 `prebuilt/openocd`。Vitis HLS 不能下载。

## 固件和模型怎么分工

固件是通用运行时。它扫描 `0:/models` 的直接子目录，加载四个文件，按同一张函数表调用每个模型。加一个新网络时，换 SD 卡上的模型包，不改固件。

一次推理的顺序固定：

1. `model.mod` 的 preprocess 把一帧图像变成网络输入。图像积木开在这里，工作内存在模型区后面。
2. 固件按模块声明的 blob 做 ncnn 前向。
3. `model.mod` 的 decode 输出最终结果。分类是 Top-K，SSD 在模块里做 DetectionOutput，RetinaFace 在模块里做锚框解码。
4. 固件只负责把结果公布到界面。

四个文件是 `model.param.bin`、`model.bin`、`labels.txt`、`model.mod`。板上没有 `model.cfg`，也没有 `image_ops.cfg`。输入尺寸、mean、norm、阈值和锚框都是 `model.mod` 里的常量。工程文档若还写着用 `model.cfg` 区分后处理，以本页和 `src/docs/模型平台化设计.md` 为准。

`model.mod` 装入 `mem_layout.h` 的 `MEM_MODEL_MOD_BASE`，长度 `MEM_MODEL_MOD_SIZE`（当前档案是 `model_mod_base` / `model_mod_size`，4MB 代码区）。权重不放在这 4MB 里，由固件从堆上分配。堆在 `mem_layout.h` 里从 `0x02000000` 到 `0x10000000`。因此 fp32 的 `model.bin` 可以大于 4MB。

积木用的临时图像放在模型区之后，档案字段是 `ops_scratch_base` 和 `ops_scratch_size`。帧的宽或高超过档案里的 `photo_max_w`、`photo_max_h` 时，带积木的预处理会失败。不要把这块工作区挪到 `MEM_WORK_BUFFER_ADDR`，推理应用用那里放摄像头快照。

SD 应用加载基址在 `mem_layout.h` 里是 `0x20000000`，长度 16MB，上界接模型区。应用是裸机器码，按 `fw_exports.ld` 里的地址调用固件。固件重编后地址会变，必须先 `python sdapp/sdapp.py export`，再重编应用。版本不一致时板子拒绝加载。

## 工具各自做什么

- `sdapp.py export`：从刚编好的固件生成 `fw_exports.ld` 和版本号。对话用 `query_symbols` 查这张表和 `font/ui_symbols.txt`，不要把整张表抄进回复。
- `sdapp.py build`：编译一个应用目录。上位机调用时会带上 `prebuilt/` 里的编译器。
- `nnmod.py`：编译 `model.mod`。预处理、积木和后处理由上位机按模型包生成源码再封包。
- `netpush/net_push.py`：和上位机 `board_push.py` 相同的 TFTP 写路径。板上要先打开「文件传输」。

字库缺字时，先告诉用户这个字不能上屏。补字要改 `tools/font` 并重新生成固件里的字体，然后重编固件。这一步超出上位机直接完成的范围。

## 对话可以做和不要做

可以：按 page=apps 写 SD 应用，按模型包生成并编译 `model.mod`，把已有的应用 bin 和模型四文件经网口写到板子，解释上述目录和调用关系。

不要：修改 `src/sw`、`src/hw`、Vivado 源文件，不要把别的板子的固件替换包覆盖到这个工程，不要编造 `mem_layout.h` 或符号表里没有的地址。
