/************************************************************************/
/*																		*/
/*	lcd_modes.h	--	VideoMode definitions		 						*/
/*																		*/
/************************************************************************/
/*	Author: Sam Bobrowicz												*/
/*	Copyright 2014, Digilent Inc.										*/
/************************************************************************/
/*  Module Description: 												*/
/*																		*/
/*		This file contains the definition of the VideoMode type, and	*/
/*		also defines several common video modes							*/
/*																		*/
/************************************************************************/
/*  Revision History:													*/
/* 																		*/
/*		2/17/2014(SamB): Created										*/
/*																		*/
/************************************************************************/

#ifndef LCD_MODES_H_
#define LCD_MODES_H_

#include "xil_types.h"  //本文件用到了u32，需自包含此头文件

typedef struct {
	char label[64]; /* Label describing the resolution */
	u32 width; /*Width of the active video frame*/
	u32 height; /*Height of the active video frame*/
	u32 hps; /*Start time of Horizontal sync pulse, in pixel clocks (active width + H. front porch)*/
	u32 hpe; /*End time of Horizontal sync pulse, in pixel clocks (active width + H. front porch + H. sync width)*/
	u32 hmax; /*Total number of pixel clocks per line (active width + H. front porch + H. sync width + H. back porch) */
	u32 hpol; /*hsync pulse polarity*/
	u32 vps; /*Start time of Vertical sync pulse, in lines (active height + V. front porch)*/
	u32 vpe; /*End time of Vertical sync pulse, in lines (active height + V. front porch + V. sync width)*/
	u32 vmax; /*Total number of lines per frame (active height + V. front porch + V. sync width + V. back porch) */
	u32 vpol; /*vsync pulse polarity*/
	double freq; /*Pixel Clock frequency*/
} VideoMode;

//模式表定义在lcd_modes.c(原static定义在头文件导致每个编译单元复制一份)
extern const VideoMode VMODE_480x272;
extern const VideoMode VMODE_640x480;
extern const VideoMode VMODE_800x480;
extern const VideoMode VMODE_800x600;
extern const VideoMode VMODE_1024x600;
extern const VideoMode VMODE_1280x800;
extern const VideoMode VMODE_1280x1024;
extern const VideoMode VMODE_1280x720;
extern const VideoMode VMODE_1920x1080;

//按宽高查支持的VideoMode, 不支持返回NULL
const VideoMode* lcd_find_mode(u32 width, u32 height);

#endif /* LCD_MODES_H_ */
