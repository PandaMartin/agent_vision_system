// =============================================================================
// nn_selftest —— 神经网络平台链路开机自检(临时设施, 推理应用UI回归后可删)
//
// 验证覆盖: model.mod 版本门/加载重定位/模块调固件导出原语/模块preprocess/
// ncnn前向按声明抽取/模块decode —— 即 nn_pipeline.h 契约的整条链。
// 触发方式: SD卡放置 0:/nn_selftest.bmp(24位BMP, 宽度须4的倍数)即测,
// 无此文件一行打印跳过(平时启动零开销)。调用点: main.cpp app_task。
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

//遍历 /models 全部模型: 激活 -> 对测试图连跑数次 nn_infer -> 串口打印
//耗时与结果终稿(检测框/Top-K, 类别名经labels.txt翻译)。全部失败/跳过
//只打串口, 不影响系统启动
void nn_selftest_run(void);

#ifdef __cplusplus
}
#endif
