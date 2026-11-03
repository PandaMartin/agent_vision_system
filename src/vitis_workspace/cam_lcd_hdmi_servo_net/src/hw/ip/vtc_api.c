/************************************************************************/
/*																		*/
/*	vtc_api.c	--	VTC(Video Timing Controller)IP驱动实现				*/
/*																		*/
/************************************************************************/
/*	Author: Sam Bobrowicz												*/
/*	Copyright 2014, Digilent Inc.										*/
/*	(自hw/lcd/lcd.c逐行平移, 仅改名LcdCtrl->VtcDev/lcd_*->vtc_dev_*)	*/
/************************************************************************/

#include "vtc_api.h"

#include <string.h>

#include "xil_io.h"

int vtc_dev_stop(VtcDev *dev)
{
	/*
	 * If already stopped, do nothing
	 */
	if (dev->running == 0)
	{
		return XST_SUCCESS;
	}

	/*
	 * Disable the vtc core, and wait for the current frame to finish (the core cannot stop
	 * mid-frame)
	 */
	XVtc_DisableGenerator(&dev->vtc);

	/*
	 * Update Struct state
	 */
	dev->running = 0;

	return XST_SUCCESS;
}

int vtc_dev_start(VtcDev *dev)
{
	XVtc_Timing vtcTiming;
	XVtc_SourceSelect SourceSelect;

	/*
	 * If already started, do nothing
	 */
	if (dev->running == 1)
	{
		return XST_SUCCESS;
	}

	/*
	 * Configure the vtc core with the display mode timing parameters
	 */
	vtcTiming.HActiveVideo = dev->vMode.width;	/**< Horizontal Active Video Size */
	vtcTiming.HFrontPorch = dev->vMode.hps - dev->vMode.width;	/**< Horizontal Front Porch Size */
	vtcTiming.HSyncWidth = dev->vMode.hpe - dev->vMode.hps;		/**< Horizontal Sync Width */
	vtcTiming.HBackPorch = dev->vMode.hmax - dev->vMode.hpe + 1;		/**< Horizontal Back Porch Size */
	vtcTiming.HSyncPolarity = dev->vMode.hpol;	/**< Horizontal Sync Polarity */
	vtcTiming.VActiveVideo = dev->vMode.height;	/**< Vertical Active Video Size */
	vtcTiming.V0FrontPorch = dev->vMode.vps - dev->vMode.height;	/**< Vertical Front Porch Size */
	vtcTiming.V0SyncWidth = dev->vMode.vpe - dev->vMode.vps;	/**< Vertical Sync Width */
	vtcTiming.V0BackPorch = dev->vMode.vmax - dev->vMode.vpe + 1;;	/**< Horizontal Back Porch Size */
	vtcTiming.V1FrontPorch = dev->vMode.vps - dev->vMode.height;	/**< Vertical Front Porch Size */
	vtcTiming.V1SyncWidth = dev->vMode.vpe - dev->vMode.vps;	/**< Vertical Sync Width */
	vtcTiming.V1BackPorch = dev->vMode.vmax - dev->vMode.vpe + 1;;	/**< Horizontal Back Porch Size */
	vtcTiming.VSyncPolarity = dev->vMode.vpol;	/**< Vertical Sync Polarity */
	vtcTiming.Interlaced = 0;		/**< Interlaced / Progressive video */

	/* Setup the VTC Source Select config structure. */
	/* 1=Generator registers are source */
	/* 0=Detector registers are source */
	memset((void *)&SourceSelect, 0, sizeof(SourceSelect));
	SourceSelect.VBlankPolSrc = 1;
	SourceSelect.VSyncPolSrc = 1;
	SourceSelect.HBlankPolSrc = 1;
	SourceSelect.HSyncPolSrc = 1;
	SourceSelect.ActiveVideoPolSrc = 1;
	SourceSelect.ActiveChromaPolSrc= 1;
	SourceSelect.VChromaSrc = 1;
	SourceSelect.VActiveSrc = 1;
	SourceSelect.VBackPorchSrc = 1;
	SourceSelect.VSyncSrc = 1;
	SourceSelect.VFrontPorchSrc = 1;
	SourceSelect.VTotalSrc = 1;
	SourceSelect.HActiveSrc = 1;
	SourceSelect.HBackPorchSrc = 1;
	SourceSelect.HSyncSrc = 1;
	SourceSelect.HFrontPorchSrc = 1;
	SourceSelect.HTotalSrc = 1;

	XVtc_SelfTest(&(dev->vtc));

	XVtc_RegUpdateEnable(&(dev->vtc));
	XVtc_SetGeneratorTiming(&(dev->vtc), &vtcTiming);
	XVtc_SetSource(&(dev->vtc), &SourceSelect);
    /*
	 * Enable VTC core, releasing backpressure on VDMA
	 */
	XVtc_EnableGenerator(&(dev->vtc));

	dev->running = 1;

	return XST_SUCCESS;
}

int vtc_dev_init(VtcDev *dev, UINTPTR vtcBaseAddr)
{
	int Status;
	XVtc_Config *vtcConfig;

	/*
	 * Initialize all the fields in the VtcDev struct
	 */
	dev->running = 0;
	dev->vMode = VMODE_800x480;

	/* Initialize the VTC driver so that it's ready to use look up
	 * configuration in the config table, then initialize it.
	 */
	vtcConfig = XVtc_LookupConfig(vtcBaseAddr);
	/* Checking Config variable */
	if (NULL == vtcConfig) {
		return (XST_FAILURE);
	}
	Status = XVtc_CfgInitialize(&(dev->vtc), vtcConfig, vtcConfig->BaseAddress);
	/* Checking status */
	if (Status != (XST_SUCCESS)) {
		return (XST_FAILURE);
	}

	return XST_SUCCESS;
}

int vtc_dev_set_mode(VtcDev *dev, const VideoMode *newMode)
{
	int Status;

	/*
	 * If currently running, stop
	 */
	if (dev->running == 1)
	{
		Status = vtc_dev_stop(dev);
		if (Status != XST_SUCCESS)
		{
			return XST_FAILURE;
		}
	}

	dev->vMode = *newMode;

	return XST_SUCCESS;
}
