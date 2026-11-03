# models —— 模型程序模块源码

每个模型一个文件夹，**文件夹名 = 模型名**，也是 SD 卡上 `/models/<文件夹名>/` 的名字。

```
src/sd/models/<模型名>/        ← 本目录: 模块源码(从零手写, 无模板可复制)
  *.c                          ← 实现 nn_pipeline.h 契约的两个函数:
                                  preprocess(预处理) + decode(解码/NMS/阈值),
                                  及 .mod_entry_first 节的 nn_mod_boot()
  build/                       ← 编译中间件(可删)
  model.mod                    ← nnmod.py 产出(拷到SD卡对应模型文件夹)
```

构建与部署：

```
python tools/nnmod/nnmod.py build src/sd/models/<模型名>
→ 产出 src/sd/models/<模型名>/model.mod
→ 拷入 SD:/models/<模型名>/model.mod
   (同文件夹另需 model.param.bin / model.bin / labels.txt 三个数据文件)
```

规则（详见 `docs/模型平台化设计.md`）：

1. **文件夹名 ≤ 20 字符**（板端推送协议的 SD 路径缓冲 48 字节：
   `0:/models/`+名+`/model.param.bin` ≤ 47，超长板端拒收 name too long）；
2. 模块一律从零编写，仓库无成品/模板/共享源码库可复制；
3. 契约头 `src/sw/impl/nn/nn_pipeline.h` 按 ABI 版本冻结，只 include 不修改；
4. 可调固件导出的平台原语（ncnn 像素缩放、expf 等），不要手写像素循环；
5. 固件无兜底：model.mod 缺失或版本不符，该模型不可用（串口明示原因）。
