# 新训 RetinaFace 首次上板交接包

路径：`handoff/wider_face_retinaface_fp32/`

这是**一个大文件夹**。下面按用途分子目录，整夹发给板端即可。  
上次 `retinaface_mnet25_fp32` 没上过板，不要混用那套 blob。

| 子目录 | 用途 | 怎么用 |
|---|---|---|
| `SD卡模型/` | 板上跑的四文件 | 整夹改名为 `wider_face_retinaface_fp32`，放到 `SD:/models/` |
| `板端固件替换/` | 要覆盖进 Vitis 的源码 | 按里面的 `覆盖说明.txt` 盖到 `ov5640_ncnn/src/`，重新编译 ELF |
| `对照资料/` | blob 编号和网络图 | 查层名用，**不要拷 SD，不要编进 ELF** |
| `说明文档/` | 改法、验收、训练来源 | 先读 `说明文档/README.md` |

顺序：先覆盖固件并确认 SqueezeNet / VOC SSD 仍能切 → 再拷 `SD卡模型/` → 串口 `l` 看到本模型后 `s`。
