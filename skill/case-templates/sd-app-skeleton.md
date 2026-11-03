# 案例模板：SD 应用开发骨架与工具链

## 适用范围
Zynq + FreeRTOS 固件已具备"命令总线 + 应用装载器"形态的项目；或新项目想按此形态起步。

## 模板内容（工程内位置）
1. 应用骨架：`src/sw/app/int_app/_template.c`（内置应用模板）；`src/sd/apps/snake/`（SD 应用已上板成例）。
2. 装载契约：应用为独立 bin，64 字节 AppHeader（含图标名/版本/入口约定），校验通过搬入固定地址执行，经固件导出符号表 `fw_exports.ld` 调用平台服务。
3. 工具链三步（均在固件组件根目录执行，详见 `tools/README.md`）：
```
python tools/sdapp/sdapp.py export                                    # 固件符号表→导出表（打印 NOTE 需再 Build）
python tools/sdapp/sdapp.py build src/sd/apps/<名> --name "图标名"     # 交叉编译+封包 → <名>.bin
python tools/netpush/net_push.py src/sd/apps/<名>/<名>.bin            # 板端"文件传输"应用内推送安装
```
4. 图形入口：`python tools/gui.py`（推送/SD 应用/字库三页，等价命令行）。

## 已验证效果
snake 应用按此流程上板运行（工程记载"已上板验证"）；人脸跟踪 SD 应用 face_track 同构。

## 失效条件
固件未实现 AppHeader 装载器/符号导出表时第 3 步不可用；更换工具链版本后 sdapp.py 的交叉编译器路径需同步。
