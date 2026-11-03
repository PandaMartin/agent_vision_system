# 模型平台化设计（NN 子系统重构定稿）

状态：**平台已落地, 链路已上板验证通过**（2026-10-22: mnv2_ssdlite_voc 检测
239ms/帧≈4.2fps, 框/类别/坐标正确; 自检只由模型推送触发, 开机不自跑, 见
`sw/impl/nn/nn_selftest.*`, 推理应用落地后随调试打印一并移除）。
**遗留问题**: 全系统偶发挂死(2026-10-22初查): 首次排查以为是ncnn原语Mat的
fastFree问题, 深入后定位为**堆本身被写坏**—— mallinfo全堆遍历在原始开机即可
复现冻结(且伴随桌面图标registry异常), malloc/free等只碰自身块的操作长期无症状,
遍历类操作(大malloc扫bin/free合并邻居/全堆walk)一踩就死; 时好时坏具非确定性
(同一固件昨夜开机正常、今晨原始开机即坏), 嫌疑指向开机DMA/网络收包路径,
**写坏者待查**(排查工具: nnmod打探针模块+推送即自检, 免重烧固件;
 mallinfo/`_sbrk(0)`/`__malloc_lock`直调可三分{锁/遍历/分配})。
模块预处理暂用纯C双线性绕开, 堆修好后可换回ncnn原语。
推理应用UI已在新架构上重写落地(2026-10-24, int_app/app_infer.c:
四面板+实时推理回泵+图片单次推理+模型管理)。
本文取代《推理应用设计方案》中"参数化预处理档案 / model.cfg"相关章节。

## 0. 一句话

固件 = 通用推理运行时（装载、调度、ncnn 前向、结果发布）；模型 = SD 卡上的三个数据文件 + 一个自描述程序模块（`model.mod`）。**除 ncnn 前向本身外，一切带模型语义的知识（预处理、输出清单、解码、NMS、阈值）全部在 SD 卡上，加模型不改固件。**

## 1. 背景与目标

现状把模型类型知识编进了固件：`post` 只有 classify / SSD 两条分支，散布在 profile 结构、cfg 解析、前向函数、后处理、应用层共 5 处。每接入一种新架构网络（如 RetinaFace 的 9 输出头 + letterbox + 锚框解码）都要在 5 个文件里开洞。

本次收敛过程中逐条确立的原则（讨论结论，实施时不许回退）：

1. 加新模型不改固件代码；
2. 框架对**所有**模型的调用方式完全一致（同一张函数表、同一套契约）；
3. **不是所有模型都能用的逻辑一律放 SD 卡**——NMS、置信度门槛、坐标裁剪、letterbox 几何、锚框数学全部属此类；
4. 固件只保留"网络推理本身"（ncnn 前向）和与任何模型无关的机械（装载/调度/发布）。

## 2. 总体形态

```
┌────────────── 固件 = 纯推理运行时（不含任何模型知识）──────────────┐
│ 模型包加载器   扫 /models、载权重、载 labels.txt、载 model.mod      │
│ 调度器        按固定顺序调模块的两个函数（对所有模型逐字相同）        │
│ 前向          ncnn + 按模块的输出声明抽取（唯一不可替换阶段）        │
│ 发布          NnResult → 结果卡/视频叠加框；labels.txt 翻译类别名   │
│ 平台原语导出   fw_exports.ld 全仓库符号（ncnn 像素缩放/颜色转换、    │
│              expf 等），供模块调用。固件本体不再引用的ncnn预处理     │
│              原语由 nn_runner.cpp 的 nn_primitive_keep 驻留锚钉住,   │
│              否则链接器裁掉、导出表消失。fw.map 须 --no-demangle     │
│              (UserConfig USER_LINK_OPTIONS)否则C++符号进不了导出表   │
└───────────────────────────────────────────────────────────────────┘
                                    ▲ 唯一接口：sw/impl/nn/nn_pipeline.h
┌────────────────── SD 模型包（加模型 = 拷一个文件夹）────────────────┐
│ /models/<名字>/                                                   │
│   model.param.bin   网络结构（ncnn2mem 产物）                       │
│   model.bin         权重                                          │
│   labels.txt        类别名，每行一个（固件发布层翻译 label 号用）     │
│   model.mod         模型程序模块（见 §4）                           │
│                                                                   │
│ 没有 model.cfg：所有参数（输入尺寸、通道序、缩放方式、阈值、锚框      │
│ 参数…）都是模块里的常量。调参 = 改模块源码重新打包（不碰固件）。      │
└───────────────────────────────────────────────────────────────────┘
```

## 3. 冻结契约（`sw/impl/nn/nn_pipeline.h`）

接口头已随本文提交，**按 `NN_PIPELINE_ABI_VER` 冻结管理**。要点：

- 六个数据类型：`NnImage / NnTensor / NnGeom / NnResult / NnRole / NnModelDef`，全部纯 C，**不含任何 ncnn 类型**（接口稳定的前提，也是模块可独立编译的前提）。
- 模块交给固件的是一张函数表 `NnModelDef`：名字、UI 类别（kind）、**输出声明**（blob_in + out_blob[]/out_role[]，模块声明、固件执行）、两个必备函数钩子 `preprocess` / `decode`。
- 一次推理 = 调度器的五行（对任何模型逐字相同）：

```c
mod->preprocess(&img, &in, &geom);            // ① 模块：图像 → 网络输入张量
nn_forward(in, def, outs, &out_n);            // ② 固件：ncnn 前向+按声明抽取
mod->decode(outs, out_n, &geom, res);         // ③ 模块：解码(含阈值/NMS/裁剪)
publish(res);                                 // ④ 固件：原样发布
```

- 分工边界：**③ 输出的 `NnResult` 是终稿**（框坐标已是 `NnImage` 坐标系像素），固件不做任何加工；NMS 等检测策略全部在模块内完成。

## 4. model.mod 模块规范

复用桌面 SD 应用（ZAPP）的全套机制，仅换名字与基址：

| 项 | 值 |
|---|---|
| 文件名 | 所有模型统一叫 `model.mod`，不同模型以文件夹名区分（`/models/<模型名>/model.mod`） |
| 魔数 | `"NMOD"` |
| 头部 | 64 字节 `NmodHeader`：magic(4) / hdr_size(=64) / **abi_ver** / **fw_version** / load_size / bss_size / name[16] / entry_off(=0) / total_size / bss_off / pad[12]（与 `tools/nnmod/nnmod.py` 封包逐字节一致） |
| 加载基址 | `MEM_MODEL_MOD_BASE = 0x21000000`（SD 应用区之后，4MB，已登记 mem_layout.h） |
| 链接 | 链接地址=加载地址，零重定位；bss 不占 bin，加载器清零 |
| 入口 | `.mod_entry_first` 节的 `nn_mod_boot()`，返回 `const NnModelDef*` |
| 版本门 | 双重：`abi_ver == NN_PIPELINE_ABI_VER`（构建时自动从 nn_pipeline.h 提取）且 `fw_version` 与固件符号导出版本一致，不符拒载并串口说明 |
| 构建 | **`tools/nnmod/nnmod.py`（独立工具，不复用 sdapp 打包逻辑）**：交叉编译 + `tools/nnmod/mod.ld` 链接 0x21000000 + 封 NmodHeader。`fw_exports.ld/json` 仍消费 `tools/sdapp export` 的产物——符号导出保持单一事实来源，本工具不重复实现 |
| 约束 | 无 STL（ncnn SIMPLESTL）；运行于 app_task 上下文；纯计算，需要像素缩放/expf 等能力时直接调固件导出符号，**不要手写像素循环**（固件原语是 NEON 优化的） |

## 5. 固件职责最终清单

| 模块 | 职责 | 来源 |
|---|---|---|
| model_mgr | SD 扫描 / 权重与 labels 加载 / **model.mod 加载校验** / 激活切换 | 改造（删 cfg 解析） |
| nn_engine（现 nn_runner） | 仅前向：外部内存包 Mat + 按输出声明抽取 | 改造（预处理引擎删除） |
| nn_module（新） | model.mod 加载/校验/卸载 | 新增（镜像 loader.c） |
| app_infer_be | 取图 → nn_infer → 发布，零类型分支 | 塌缩 |
| 发布层（bridge/UI/vidwin/ovl） | 不动 | 不动 |
| 已删除 | model_profile.h 的档案结构、post_process.*、参数化预处理、内置 NMS | 不以任何形式保留（含共享源码库）；后续模型各自从零实现 |

## 6. 模块开发规则（无成品、无模板、无共享库）

**所有模型一视同仁：每个模型的 `model.mod` 都从零编写**，无论它与已有模型多么相似。仓库不提供可复制的成品模块、填空模板或共享源码库（nms.c 之类一律不设）——模块之间不存在任何共享的模型语义代码。

模块开发者手里只有两样东西：

- 契约头 `sw/impl/nn/nn_pipeline.h`：必须实现的符号——`.mod_entry_first` 节的 `nn_mod_boot()`，函数表里的 `preprocess` / `decode` 两个钩子；
- 固件导出的平台原语（`fw_exports.ld` 全仓库符号：ncnn 像素缩放/颜色转换、expf 等）——这是平台的 libc，不属于模型语义，允许调用。

一个模块源码 = 一个或几个 .c 文件，自行实现预处理数学、输出声明、解码数学、NMS 与全部阈值常量 → `tools/nnmod/nnmod.py build` → `model.mod`。已有模型的模块源码可以**参考**，但不存在任何被复用的构件。

模块源码统一放 `src/sd/models/<模型名>/`（与 SD 应用 `src/sd/apps/` 同属一棵 SD 加载内容源码树，见 `src/sd/README.md`），产出 `model.mod` 后与三个数据文件一起部署到 SD 卡 `/models/<模型名>/`。

```
tools/nnmod/
  nnmod.py            模型模块编译封包工具（model.mod 的唯一生产工具）
  mod.ld              模块链接脚本（0x21000000，入口节 .mod_entry_first）
```

### 6.1 模块代码禁用清单（2026-10-22 定, 随堆写坏问题修复动态放宽）

**一直禁（平台契约）**:

- 无 STL / 无 new-delete（ncnn SIMPLESTL + freestanding, 模块没有完整 libc）;
- 无全局动态初始化（加载器不跑 init_array, 全局对象构造函数不会执行——
  全局/静态只能是编译期常量, 工作区一律 bss 数组, 加载器负责清零）;
- 契约头 `nn_pipeline.h` 只能 include 不能改。

**堆写坏问题修复前, 额外禁（固件存在非确定性堆踩踏, 遍历/合并类操作会引爆）**:

- 模块代码内 **malloc/calloc/realloc/free、C++ new/delete** 一律不用——
  工作区用 bss 静态数组（模型区4MB, 够用; 参考 mnv2 的 s_in[300*300*3]）;
- **不要在模块里创建/销毁 ncnn::Mat 局部对象**（构造分配+析构释放都走堆,
  实测 free 合并邻居时踩坏块即全系统挂死）——预处理自己做数学, 参考
  mnv2 的纯C双线性; 纯数学类原语（expf 等）不受此限;
- 不调 mallinfo/malloc_stats/malloc_trim 等堆遍历类函数;
- ncnn 前向引擎内部的 malloc/free 经实测正常（240ms×3 连跑存活）, 固件侧
  不受此清单约束。

**安全可用**: xil_printf、memcpy/memset/str系列、fw_exports 导出的数学原语、
自身 bss 工作区、契约内的 NnTensor/NnResult/NnGeom 纯C结构。

**兜底**: 违规即便挂死也不再 brick——看门狗自动复位, 连挂2次进入保护模式
（桌面可用, 文件应用删除问题model.mod后重推即可, 免拔卡, 见 nn_wdt.c）。

## 7. 新模型接入手册

接**任何**模型的流程都一样（没有快慢档之分）：

1. 从零编写模块源码（放 `src/sd/models/<模型名>/`）：实现契约两函数（`preprocess` / `decode`），输出声明与全部阈值、锚参数等常量自理；
2. `python tools/nnmod/nnmod.py build src/sd/models/<模型名>` → `model.mod`；
3. 与 `model.param.bin`、`model.bin`、`labels.txt` 一起拷入 SD 卡 `/models/<模型名>/`。

唯一例外是硬边界：网络用到固件 ncnn 没有的层时必须重编固件（缺层时 `load_param` 明确报错）。

## 8. 实施步骤

1. 契约头 `nn_pipeline.h`（已提交）+ mem_layout 登记（已完成）+ `tools/nnmod/`（已提交）+ nn_module 加载器（实施时做）。
2. 引擎接线（nn_engine 仅前向）；**从零手写前两个模块**（SqueezeNet 分类、VOC SSD 检测）供回归，行为须与改前一致。
3. 从零手写 RetinaFace 模块（letterbox 预处理 + 锚框解码 + NMS），四文件拷 SD，按原交接文档 §6 验收（单人脸 15%~40% 画面出 1 框、无人不满屏框、score_th 0.5 起步）。
4. 《模型接入指南》：契约符号清单 + 从零开发流程 + 打包命令。

## 9. 纪律（定稿即生效）

1. **契约冻结**：`nn_pipeline.h` 的任何签名/结构改动 = abi_ver 升级 = 全部在野模块失配。改之前先问：能不能不动它？
2. **模块一律从零编写、双函数齐全**：没有成品可拷、没有模板可填；已有模型源码只可参考、不可复用。
3. **固件无兜底**：model.mod 缺失/版本不符 → 该模型不可用，串口给明确原因；模型文件夹请整包拷贝。
4. **行为一致性靠验收、不靠共享代码**：NMS/预处理各模块自写，新模型接入必须经 PC 端对拍与板上验收把关（错 variance、错通道这类错误没有共享代码可追溯，全靠验收拦截）。
