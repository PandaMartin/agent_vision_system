/************************************************************************/
/*																		*/
/*	lcd_modes.c	--	VideoMode模式表定义与查找							*/
/*																		*/
/************************************************************************/
/*	Author: Sam Bobrowicz												*/
/*	Copyright 2014, Digilent Inc.										*/
/*	(自lcd_modes.h拆出: 原static表每编译单元一份, 改为唯一定义+extern)	*/
/************************************************************************/

#include "lcd_modes.h"

const VideoMode VMODE_480x272 = {
	.label = "480x272@60Hz",
	.width = 480,
	.height = 272,
	.hps = 482,
	.hpe = 523,
	.hmax = 525,
	.hpol = 0,
	.vps = 274,
	.vpe = 284,
	.vmax = 286,
	.vpol = 0,
	.freq = 9
};

const VideoMode VMODE_640x480 = {
	.label = "640x480@60Hz",
	.width = 640,
	.height = 480,
	.hps = 656,
	.hpe = 752,
	.hmax = 799,
	.hpol = 0,
	.vps = 490,
	.vpe = 492,
	.vmax = 524,
	.vpol = 0,
	.freq = 25.12
};

const VideoMode VMODE_800x480 = {
	.label = "800x480@60Hz",
	.width = 800,
	.height = 480,
	.hps = 840,
	.hpe = 968,
	.hmax = 1056,
	.hpol = 0,
	.vps = 490,
	.vpe = 492,
	.vmax = 525,
	.vpol = 0,
	.freq = 33.0
};

const VideoMode VMODE_800x600 = {
	.label = "800x600@60Hz",
	.width = 800,
	.height = 600,
	.hps = 840,
	.hpe = 968,
	.hmax = 1055,
	.hpol = 1,
	.vps = 601,
	.vpe = 605,
	.vmax = 627,
	.vpol = 1,
	.freq = 40.0
};

const VideoMode VMODE_1024x600 = {
	.label = "1024x600@60Hz",
	.width = 1024,
	.height = 600,
	.hps = 1164,
	.hpe = 1184,
	.hmax = 1344,
	.hpol = 0,
	.vps = 620,
	.vpe = 623,
	.vmax = 635,
	.vpol = 0,
	.freq = 50.0
};

const VideoMode VMODE_1280x800 = {
	.label = "1280x800@60Hz",
	.width = 1280,
	.height = 800,
	.hps = 1360,
	.hpe = 1370,
	.hmax = 1440,
	.hpol = 0,
	.vps = 810,
	.vpe = 813,
	.vmax = 823,
	.vpol = 0,
	.freq = 70.0
};

const VideoMode VMODE_1280x1024 = {
	.label = "1280x1024@60Hz",
	.width = 1280,
	.height = 1024,
	.hps = 1328,
	.hpe = 1440,
	.hmax = 1687,
	.hpol = 1,
	.vps = 1025,
	.vpe = 1028,
	.vmax = 1065,
	.vpol = 1,
	.freq = 108.0
};

const VideoMode VMODE_1280x720 = {
	.label = "1280x720@60Hz",
	.width = 1280,
	.height = 720,
	.hps = 1390,
	.hpe = 1430,
	.hmax = 1649,
	.hpol = 1,
	.vps = 725,
	.vpe = 730,
	.vmax = 749,
	.vpol = 1,
	.freq = 74.25, //74.2424 is close enough
};

const VideoMode VMODE_1920x1080 = {
	.label = "1920x1080@60Hz",
	.width = 1920,
	.height = 1080,
	.hps = 2008,
	.hpe = 2052,
	.hmax = 2199,
	.hpol = 1,
	.vps = 1084,
	.vpe = 1089,
	.vmax = 1124,
	.vpol = 1,
	.freq = 148.5 //148.57 is close enough
};

//按宽高查支持的VideoMode, 不支持返回NULL (原lcd_detect.c的查找逻辑迁入)
const VideoMode* lcd_find_mode(u32 width, u32 height)
{
    static const VideoMode* const modes[] = {
        &VMODE_480x272,
        &VMODE_640x480,
        &VMODE_800x480,
        &VMODE_800x600,
        &VMODE_1024x600,
        &VMODE_1280x720,
        &VMODE_1280x800,
        &VMODE_1280x1024,
        &VMODE_1920x1080,
    };
    const int n = (int)(sizeof(modes) / sizeof(modes[0]));
    for (int i = 0; i < n; i++)
    {
        if (modes[i]->width == width && modes[i]->height == height)
            return modes[i];
    }
    return 0;
}
