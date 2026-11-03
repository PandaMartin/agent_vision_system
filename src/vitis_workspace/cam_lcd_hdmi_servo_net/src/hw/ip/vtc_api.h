/************************************************************************/
/*																		*/
/*	vtc_api.h	--	VTC(Video Timing Controller)IP驱动					*/
/*																		*/
/************************************************************************/
/*	Author: Sam Bobrowicz												*/
/*	Copyright 2014, Digilent Inc.										*/
/*	(自hw/lcd/lcd.c拆出: 原lcd.c名为面板驱动实为VTC寄存器编程, 归位ip层;	*/
/*	 LcdCtrl->VtcDev, lcd_init/start/stop/set_mode->vtc_dev_*)			*/
/************************************************************************/

#ifndef HW_IP_VTC_API_H
#define HW_IP_VTC_API_H

/* ------------------------------------------------------------ */
/*				Include File Definitions						*/
/* ------------------------------------------------------------ */

#include "xil_types.h"
#include "xvtc.h"
#include "lcd_modes.h"

/* ------------------------------------------------------------ */
/*					General Type Declarations					*/
/* ------------------------------------------------------------ */

typedef struct {
	XVtc vtc;		 	/*VTC driver struct*/
	VideoMode vMode; 	/*Current Video mode*/
	double pxlFreq;		/*Frequency of clock currently being generated*/
	int running;		/*0=stopped 1=running(原LcdState)*/
} VtcDev;

/* ------------------------------------------------------------ */
/*					Procedure Declarations						*/
/* ------------------------------------------------------------ */

//初始化VTC驱动实例(默认模式800x480, 后续set_mode指定实际模式)
int vtc_dev_init(VtcDev *dev, UINTPTR vtcBaseAddr);

//切换视频模式(运行中会先停止, 需重新start)
int vtc_dev_set_mode(VtcDev *dev, const VideoMode *newMode);

//按当前模式启动时序发生器(配置全部Timing并EnableGenerator)
int vtc_dev_start(VtcDev *dev);

//停止时序发生器
int vtc_dev_stop(VtcDev *dev);

#endif /* HW_IP_VTC_API_H */
