# 案例模板：模型包结构与接入流程

## 适用范围
ncnn 推理运行时 + "模型即文件"架构的嵌入式项目。

## 模板内容（工程内位置）
SD 卡 `/models/<模型名>/` 下共四件：
```
model.param.bin  model.bin   labels.txt      ← ncnn 权重与标签（三数据文件）
model.mod                                     ← 模型程序模块（nnmod.py 产出）
```
模块源码实现冻结契约头 `src/sw/impl/nn/nn_pipeline.h` 的两函数（preprocess / decode），可调用固件导出的平台原语（ncnn 像素缩放/颜色转换、expf 等），不要手写像素循环。

## 纪律（来自 `src/sd/models/README.md` 与 `docs/模型平台化设计.md`）
1. 模型文件夹名 ≤ 20 字符（板端路径缓冲 48 字节，超长板端拒收 "name too long"）；
2. 契约头按 ABI 版本冻结，只 include 不修改；
3. 固件无兜底：model.mod 缺失或版本不符 → 该模型明确不可用，串口说明原因；
4. 构建命令：`python tools/nnmod/nnmod.py build src/sd/models/<模型名>`。

## 已验证效果
mnv2_ssdlite_voc（VOC 检测）与 wd_face_ret_fp32（人脸检测）两包按此结构上板：前者 2026-10-22 实测 239 ms/帧 ≈ 4.2 fps、框/类别/坐标正确；后者驱动人脸跟踪闭环应用。

## 失效条件
更换推理框架（如 TFLite）时契约头与原语导出需重定义；ABI 升级后旧 mod 必须重编。
