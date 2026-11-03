// =============================================================================
// nn_wdt —— 模型子系统看门狗 + 连续挂死保护门
//
// 背景: model.mod 是SD加载执行的裸机码, 一个写坏/有死循环的模块会把系统
// 整体挂死, 且挂点在开机扫描路径时连桌面都进不去, 只能拔卡自救。
//
// 机制:
//   nn_wdt_arm   进入扫描/激活/自检时上SCU私有看门狗(~4s, 两拍后硬件复位,
//                DDR内容保留); nn_wdt_kick 在耗时节点喂狗; nn_wdt_disarm
//                出口关狗(切回定时器模式并停表)。
//   nn_guard_take_skip_slot  每轮完整扫描开始时调用: DDR顶部scratch记连续
//                挂死次数, 连续>=NN_GUARD_MAX次(看门狗复位不清DDR)返回1,
//                调用方进入保护模式——跳过模块执行, 桌面可用, 用户经文件应用
//                删除问题model.mod后重推即可自救, 无需拔卡。
//   nn_guard_clear  扫描正常完成时清零计数。
//
// scratch落址0x1FFFFF00: 高于堆上限(0x10000000)堆不可达, 低于SD应用区
// (0x20000000), 远离VDMA帧缓存(0x011-0x020)与模型区(0x21000000)。
// =============================================================================
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NN_GUARD_MAX  2u    //连续挂死次数阈值

void nn_wdt_arm(void);
void nn_wdt_kick(void);
void nn_wdt_disarm(void);

//返回1=本轮应跳过模块执行(计数已复位, 下次扫描重试)
int nn_guard_take_skip_slot(void);
void nn_guard_clear(void);

#ifdef __cplusplus
}
#endif
