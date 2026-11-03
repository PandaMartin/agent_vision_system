// =============================================================================
// cam_chain —— 摄像头+HDMI显示链路子系统(链路编排层, 可选子系统)
//
// 一条链拉起整个摄像头/HDMI系统: ov5640传感器(dev/ov5640, SCCB经bus层)
// -> VDMA1(S2MM写三帧环 + MM2S读出) -> VTC1(HDMI时序); 链路自由跑,
// 74.25MHz HDMI像素时钟由PL端clk_wiz_1硬件固定输出, 无需软件配置
//
// 属可选子系统: 系统核心(串口/LCD/触摸/SD)开机初始化, 本链由应用
// 调cam_chain_start/stop按需启停(M1起监控应用持有)
//
// 帧访问(供impl/cam层经这些查询构建抓帧/叠加): 帧环为固定地址三缓冲
// =============================================================================

#ifndef HW_CHAIN_CAM_CHAIN_H
#define HW_CHAIN_CAM_CHAIN_H

#include "xil_types.h"

//拉起摄像头+HDMI链路(传感器时序按尺寸查表)
//返回0成功; -1尺寸无VideoMode; -2无传感器时序; -3 OV5640初始化失败;
//-4 帧流校验超时(写指针不推进); -5 VDMA配置失败
//幂等: 流已在跑同尺寸立即成功(常热模型, 详见cam_chain.c顶部说明)
//注意: 启动后HDMI输出默认关(采集/LCD预览已可用), 经cam_chain_hdmi_set打开
int cam_chain_start(u16 width, u16 height);

//带一次冷重试的启动: 任何失败都全停静默后完整重来一次再判定成败
int cam_chain_start_retry(u16 width, u16 height);

//HDMI输出独立开关(链路出流期间): 开=VDMA读通道+VTC1发生器;
//关=发生器先停(HDMI无信号自熄)再停读通道。未出流时不允许开
void cam_chain_hdmi_set(int on);
int  cam_chain_hdmi_get(void);

//退到后台(常热): 只关HDMI; 传感器与VDMA写通道继续跑(帧进帧环, 无消费者),
//下次start同尺寸立即成功。开机后从未start过则本函数为无操作。
//(实测传感器断流后无法可靠重启[s2mm帧尺寸错], 故流一旦启动永不停)
void cam_chain_stop(void);

//链路是否在出帧
int cam_chain_running(void);

//---- 帧环查询(仅运行时有意义) ----
int cam_chain_width(void);            //帧宽(0=未启动)
int cam_chain_height(void);           //帧高
u32 cam_chain_frame_count(void);      //帧环深度(=3)
u32 cam_chain_write_index(void);      //VDMA当前正在写的帧环索引
u8* cam_chain_frame_addr(int idx);    //帧环内第idx帧的地址(RGB888)

#endif /* HW_CHAIN_CAM_CHAIN_H */
