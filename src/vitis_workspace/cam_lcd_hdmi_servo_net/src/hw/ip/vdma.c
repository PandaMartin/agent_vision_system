/******************************************************************************
*
* Copyright (C) 2014 - 2015 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/
/*****************************************************************************/
/**
 *
 * @file vdma_api.c
 *
 * This file has high level API to configure and start the VDMA.The file assumes that:
 * The design has VDMA with both MM2S and S2MM path enable.
 * The API's has tested with hardware that has tow VDMA and MM2S to S2MM are back
 * to back connected for each VDMA.
 *
 * ***************************************************************************/
#define DEBUG_MODE		0


/******************** Include files **********************************/
#include "xaxivdma.h"
#include "xparameters.h"
#include "xil_exception.h"
#include "vdma.h"

/******************** Data structure Declarations *****************************/

typedef struct vdma_handle
{
	/* The device ID of the VDMA */
	unsigned int device_id;
	/* The state variable to keep track if the initialization is done*/
	unsigned int init_done;
	/** The XAxiVdma driver instance data. */
	XAxiVdma* InstancePtr;
	/* The XAxiVdma_DmaSetup structure contains all the necessary information to
	 * start a frame write or read. */
	XAxiVdma_DmaSetup ReadCfg;
	XAxiVdma_DmaSetup WriteCfg;
	/* Horizontal size of frame */
	unsigned int hsize;
	/* Vertical size of frame */
	unsigned int vsize;
	/* Buffer address from where read and write will be done by VDMA */
	unsigned int buffer_address;
	/* Flag to tell VDMA to interrupt on frame completion*/
	unsigned int enable_frm_cnt_intr;
	/* The counter to tell VDMA on how many frames the interrupt should happen*/
	unsigned int number_of_frame_count;
}vdma_handle;

/******************** Constant Definitions **********************************/

/*
 * Device related constants. These need to defined as per the HW system.
 */
vdma_handle vdma_context[XPAR_XAXIVDMA_NUM_INSTANCES];
static unsigned int context_init=0;

/******************* Function Prototypes ************************************/

static int ReadSetup(vdma_handle *vdma_context);
static int WriteSetup(vdma_handle *vdma_context);
static int StartTransfer(XAxiVdma *InstancePtr,vdma_run_mode mode);

/* GenLock(EnableSync)总开关, 影响之后run_vdma_frame_buffer的通道配置 */
static unsigned int vdma_genlock_enable = 1;

void vdma_genlock(int enable)
{
	vdma_genlock_enable = enable ? 1 : 0;
}

/* 运行期切换写通道GenlockEn(见vdma.h):
 * 经XAxiVdma_DmaConfig重写写通道CR —— 驱动的ChannelConfig读改写CR,
 * 只清/置SYNC_EN/FRMCNT/RD_PTR/TAIL_EN等位, RS(运行)与Circular保持,
 * 通道不重启不停顿 */
int vdma_write_genlock(XAxiVdma* InstancePtr, UINTPTR baseaddr, int enable)
{
	int i;
	vdma_handle *ctx = NULL;

	for(i=0; i < XPAR_XAXIVDMA_NUM_INSTANCES; i++) {
		if (vdma_context[i].init_done &&
			vdma_context[i].device_id == baseaddr) {
			ctx = &vdma_context[i];
			break;
		}
	}
	if (ctx == NULL || ctx->InstancePtr != InstancePtr) {
		xil_printf("vdma_write_genlock: no ctx for baseaddr 0x%x\r\n",
				baseaddr);
		return XST_FAILURE;
	}

	ctx->WriteCfg.EnableSync = enable ? 1 : 0;
	return XAxiVdma_DmaConfig(ctx->InstancePtr, XAXIVDMA_WRITE,
			&ctx->WriteCfg);
}


/*****************************************************************************/
/**
*
* run_vdma_frame_buffer API
*
* This API is the interface between application and other API. When application will call
* this API with right argument, This API will call rest of the API to configure the read
* and write path of VDMA,based on ID. After that it will start both the read and write path
* of VDMA
*
* @param	InstancePtr is the handle to XAxiVdma data structure.
* @param	BaseAddress is the base address of current VDMA
* @param	hsize is the horizontal size of the frame. It will be in Pixels.
* 		The actual size of frame will be calculated by multiplying this
* 		with tdata width.
* @param 	vsize is the Vertical size of the frame.
* @param	buf_base_addr is the buffer address where frames will be written
*		and read by VDMA.
* @param 	number_frame_count specifies after how many frames the interrupt
*		should come.
* @param 	enable_frm_cnt_intr is for enabling frame count interrupt
*		when set to 1.
* @param    select to sets up the read channel,the write channel or both of
*       the read and write channel.
* @return
*		- XST_SUCCESS if example finishes successfully
*		- XST_FAILURE if example fails.
*
******************************************************************************/
int run_vdma_frame_buffer(XAxiVdma* InstancePtr, UINTPTR BaseAddress, int hsize,
		int vsize, int buf_base_addr, int number_frame_count,
		int enable_frm_cnt_intr,vdma_run_mode mode)
{
	int Status,i;
	vdma_handle *ctx;
	XAxiVdma_Config *Config;
	XAxiVdma_FrameCounter FrameCfgPtr;

	/* This is one time initialization of state machine context.
	 * In first call it will be done for all VDMA instances in the system.
	 */
	if(context_init==0) {
		for(i=0; i < XPAR_XAXIVDMA_NUM_INSTANCES; i++) {
			vdma_context[i].InstancePtr = NULL;
			vdma_context[i].device_id = -1;
			vdma_context[i].hsize = 0;
			vdma_context[i].vsize = 0;
			vdma_context[i].init_done = 0;
			vdma_context[i].buffer_address = 0;
			vdma_context[i].enable_frm_cnt_intr = 0;
			vdma_context[i].number_of_frame_count = 0;

		}
		context_init = 1;
	}

	/* The below initialization will happen for each VDMA. The API argument
	 * will be stored in internal data structure
	 */

	/* The information of the XAxiVdma_Config comes from hardware build.
	 * The user IP should pass this information to the AXI DMA core.
	 */
	Config = XAxiVdma_LookupConfig(BaseAddress);
	if (!Config) {
		xil_printf("No video DMA found for baseaddr 0x%x\r\n",BaseAddress );
		return XST_FAILURE;
	}

	/* 按基地址选择VDMA上下文槽位：同一VDMA重复配置时复用原槽位，
	 * 新VDMA取空闲槽位，避免多VDMA配置互相覆盖 */
	ctx = NULL;
	for(i=0; i < XPAR_XAXIVDMA_NUM_INSTANCES; i++) {
		if (vdma_context[i].init_done && vdma_context[i].device_id == BaseAddress) {
			ctx = &vdma_context[i];
			break;
		}
	}
	if (ctx == NULL) {
		for(i=0; i < XPAR_XAXIVDMA_NUM_INSTANCES; i++) {
			if (vdma_context[i].init_done == 0) {
				ctx = &vdma_context[i];
				break;
			}
		}
	}
	if (ctx == NULL) {
		xil_printf("No free VDMA context slot for baseaddr 0x%x\r\n",BaseAddress );
		return XST_FAILURE;
	}

	ctx->device_id = BaseAddress;
	ctx->InstancePtr = InstancePtr;
	ctx->init_done = 1;

	/* 每次调用都重新CfgInitialize: 其内部会复位VDMA硬件并重读配置。
	 * 若按init_done跳过, "停止后再启动"的链路(如摄像头链反复启停)不复位,
	 * 通道状态机无法重新同步 —— 实测二次启动后S2MM写通道冻结、
	 * 写指针停走(LCD窗口静止), 而MM2S读通道自由跑显示旧帧造成"HDMI正常"假象 */
	Status = XAxiVdma_CfgInitialize(ctx->InstancePtr,
					Config, Config->BaseAddress);
	if (Status != XST_SUCCESS) {
		xil_printf("Configuration Initialization failed %d\r\n",
				Status);
		return XST_FAILURE;
	}
	ctx->vsize = vsize;

	ctx->buffer_address = buf_base_addr;
	ctx->enable_frm_cnt_intr = enable_frm_cnt_intr;
	ctx->number_of_frame_count = number_frame_count;
	ctx->hsize = hsize * (Config->Mm2SStreamWidth>>3);

	/* Setup the write channel */
	if ((mode == BOTH) || (mode == ONLY_WRITE)) {
		Status = WriteSetup(ctx);
		if (Status != XST_SUCCESS) {
			xil_printf("Write channel setup failed %d\r\n", Status);
			if (Status == XST_VDMA_MISMATCH_ERROR)
				xil_printf("DMA Mismatch Error\r\n");
			return XST_FAILURE;
		}
	}

	/* Setup the read channel */
	if ((mode == BOTH) || (mode == ONLY_READ)) {
		Status = ReadSetup(ctx);
		if (Status != XST_SUCCESS) {
			xil_printf("Read channel setup failed %d\r\n", Status);
			if (Status == XST_VDMA_MISMATCH_ERROR)
				xil_printf("DMA Mismatch Error\r\n");
			return XST_FAILURE;
		}
	}

	/* The frame counter interrupt is enabled, setting VDMA for same */
	if(ctx->enable_frm_cnt_intr) {
		FrameCfgPtr.ReadDelayTimerCount = 1;
		FrameCfgPtr.ReadFrameCount = number_frame_count;
		FrameCfgPtr.WriteDelayTimerCount = 1;
		FrameCfgPtr.WriteFrameCount = number_frame_count;

		XAxiVdma_SetFrameCounter(ctx->InstancePtr,&FrameCfgPtr);
		/* Enable DMA read and write channel interrupts. The configuration for interrupt
		 * controller will be done by application	 */
		XAxiVdma_IntrEnable(ctx->InstancePtr,
				XAXIVDMA_IXR_ERROR_MASK |
				XAXIVDMA_IXR_FRMCNT_MASK,XAXIVDMA_WRITE);
		XAxiVdma_IntrEnable(ctx->InstancePtr,
				XAXIVDMA_IXR_ERROR_MASK |
				XAXIVDMA_IXR_FRMCNT_MASK,XAXIVDMA_READ);
	} else	{
		/* Enable DMA read and write channel interrupts. The configuration for interrupt
		* controller will be done by application	 */
		XAxiVdma_IntrEnable(ctx->InstancePtr,
					XAXIVDMA_IXR_ERROR_MASK,XAXIVDMA_WRITE);
		XAxiVdma_IntrEnable(ctx->InstancePtr,
					XAXIVDMA_IXR_ERROR_MASK ,XAXIVDMA_READ);
	}

	/* Start the DMA engine to transfer */
	Status = StartTransfer(ctx->InstancePtr,mode);
	if (Status != XST_SUCCESS) {
		if(Status == XST_VDMA_MISMATCH_ERROR)
			xil_printf("DMA Mismatch Error\r\n");
		return XST_FAILURE;
	}
#if DEBUG_MODE
	xil_printf("Code is in Debug mode, Make sure that buffer addresses are at valid memory \r\n");
	xil_printf("In triple mode, there has to be six consecutive buffers for Debug mode \r\n");
	{
		u32 pixels,j,Addr = ctx->buffer_address;
		u8 *dst,*src;
		u32 total_pixel = ctx->WriteCfg.Stride * ctx->vsize;
		src = (unsigned char *)Addr;
		dst = (unsigned char *)Addr + (total_pixel * ctx->InstancePtr->MaxNumFrames);

		for(j=0;j<ctx->InstancePtr->MaxNumFrames;j++) {
			for(pixels=0;pixels<total_pixel;pixels++) {
				if(src[pixels] != dst[pixels]) {
					xil_printf("VDMA transfer failed: SRC=0x%x, DST=0x%x\r\n",
							src[pixels],dst[pixels]);
					exit(-1);
				}
			}
			src = src + total_pixel;
			dst = dst + total_pixel;
		}
	}
	xil_printf("VDMA transfer is happening and checked for 3 frames \r\n");
#endif

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
*
* This function sets up the read channel
*
* @param	vdma_context is the context pointer to the VDMA engine.
*
* @return	XST_SUCCESS if the setup is successful, XST_FAILURE otherwise.
*
* @note		None.
*
******************************************************************************/
static int ReadSetup(vdma_handle *vdma_context)
{
	int Index;
	u32 Addr;
	int Status;

	vdma_context->ReadCfg.VertSizeInput = vdma_context->vsize;
	vdma_context->ReadCfg.HoriSizeInput = vdma_context->hsize;

	vdma_context->ReadCfg.Stride = vdma_context->hsize;
	vdma_context->ReadCfg.FrameDelay = 0;  /* This example does not test frame delay */

	vdma_context->ReadCfg.EnableCircularBuf = 1;
	vdma_context->ReadCfg.EnableSync =  vdma_genlock_enable;  /* Gen-Lock(可关) */

	vdma_context->ReadCfg.PointNum = 0;
	vdma_context->ReadCfg.EnableFrameCounter = 0; /* Endless transfers */

	vdma_context->ReadCfg.FixedFrameStoreAddr = 0; /* We are not doing parking */
	/* Configure the VDMA is per fixed configuration, This configuration is being used by majority
	 * of customer. Expert users can play around with this if they have different configurations */

	Status = XAxiVdma_DmaConfig(vdma_context->InstancePtr, XAXIVDMA_READ, &vdma_context->ReadCfg);
	if (Status != XST_SUCCESS) {
		xil_printf("Read channel config failed %d\r\n", Status);
		return XST_FAILURE;
	}

	/* Initialize buffer addresses
	 *
	 * These addresses are physical addresses
	 */
	Addr = vdma_context->buffer_address;

	for(Index = 0; Index < vdma_context->InstancePtr->MaxNumFrames; Index++) {
		vdma_context->ReadCfg.FrameStoreStartAddr[Index] = Addr;

		/* Initializing the buffer in case of Debug mode */

#if DEBUG_MODE
		{
			u32 i;
			u8 *src;
			u32 total_pixel = vdma_context->ReadCfg.Stride * vdma_context->vsize;
			src = (unsigned char *)Addr;
			xil_printf("Read Buffer %d address: 0x%x \r\n",Index,Addr);
			for(i=0;i<total_pixel;i++)
			{
				src[i] = i & 0xFF;
			}
		}
#endif
		Addr +=  vdma_context->hsize * vdma_context->vsize;
	}

	/* Set the buffer addresses for transfer in the DMA engine
	 * The buffer addresses are physical addresses
	 */
	Status = XAxiVdma_DmaSetBufferAddr(vdma_context->InstancePtr, XAXIVDMA_READ,
			vdma_context->ReadCfg.FrameStoreStartAddr);
	if (Status != XST_SUCCESS) {
		xil_printf(
			"Read channel set buffer address failed %d\r\n", Status);

		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
*
* This function sets up the write channel
*
* @param	dma_context is the context pointer to the VDMA engine..
*
* @return	XST_SUCCESS if the setup is successful, XST_FAILURE otherwise.
*
* @note		None.
*
******************************************************************************/
static int WriteSetup(vdma_handle *vdma_context)
{
	int Index;
	u32 Addr;
	int Status;

	vdma_context->WriteCfg.VertSizeInput = vdma_context->vsize;
	vdma_context->WriteCfg.HoriSizeInput = vdma_context->hsize;

	vdma_context->WriteCfg.Stride = vdma_context->hsize;
	vdma_context->WriteCfg.FrameDelay = 0;  /* This example does not test frame delay */

	vdma_context->WriteCfg.EnableCircularBuf = 1;
	vdma_context->WriteCfg.EnableSync =  vdma_genlock_enable;  /*  Gen-Lock(可关) */

	vdma_context->WriteCfg.PointNum = 0;
	vdma_context->WriteCfg.EnableFrameCounter = 0; /* Endless transfers */

	vdma_context->WriteCfg.FixedFrameStoreAddr = 0; /* We are not doing parking */
	/* Configure the VDMA is per fixed configuration, This configuration
	 * is being used by majority of customers. Expert users can play around
	 * with this if they have different configurations
	 */

	Status = XAxiVdma_DmaConfig(vdma_context->InstancePtr, XAXIVDMA_WRITE, &vdma_context->WriteCfg);
	if (Status != XST_SUCCESS) {
		xil_printf(
			"Write channel config failed %d\r\n", Status);

		return Status;
	}

	/* Initialize buffer addresses
	 *
	 * Use physical addresses
	 */
	Addr = vdma_context->buffer_address;
	/* If Debug mode is enabled write frame is shifted 3 Frames
	 * store ahead to compare read and write frames
	 */
#if DEBUG_MODE
	Addr = Addr + vdma_context->InstancePtr->MaxNumFrames * \
			(vdma_context->WriteCfg.Stride * vdma_context->vsize);
#endif

	for(Index = 0; Index < vdma_context->InstancePtr->MaxNumFrames; Index++) {
		vdma_context->WriteCfg.FrameStoreStartAddr[Index] = Addr;
#if DEBUG_MODE
		xil_printf("Write Buffer %d address: 0x%x \r\n",Index,Addr);
#endif

		Addr += (vdma_context->hsize * vdma_context->vsize);
	}

	/* Set the buffer addresses for transfer in the DMA engine */
	Status = XAxiVdma_DmaSetBufferAddr(vdma_context->InstancePtr,
			XAXIVDMA_WRITE,
			vdma_context->WriteCfg.FrameStoreStartAddr);
	if (Status != XST_SUCCESS) {
		xil_printf("Write channel set buffer address failed %d\r\n",
				Status);
		return XST_FAILURE;
	}

	/* Clear data buffer
	 */
#if DEBUG_MODE
	memset((void *)vdma_context->buffer_address, 0,
			vdma_context->ReadCfg.Stride * vdma_context->ReadCfg.VertSizeInput * vdma_context->InstancePtr->MaxNumFrames);
#endif
	return XST_SUCCESS;
}

/*****************************************************************************/
/**
*
* This function starts the DMA transfers. Since the DMA engine is operating
* in circular buffer mode, video frames will be transferred continuously.
*
* @param	InstancePtr points to the DMA engine instance
*
* @return
*		- XST_SUCCESS if both read and write start successfully
*		- XST_FAILURE if one or both directions cannot be started
*
* @note		None.
*
******************************************************************************/
static int StartTransfer(XAxiVdma *InstancePtr,vdma_run_mode mode)
{
	int Status;
	if ((mode == BOTH) || (mode == ONLY_WRITE)) {
		/* Start the write channel of VDMA */
		Status = XAxiVdma_DmaStart(InstancePtr, XAXIVDMA_WRITE);
		if (Status != XST_SUCCESS) {
			xil_printf("Start Write transfer failed %d\r\n", Status);
			return XST_FAILURE;
		}
	}
	/* Start the Read channel of VDMA */
	if ((mode == BOTH) || (mode == ONLY_READ)) {
		Status = XAxiVdma_DmaStart(InstancePtr, XAXIVDMA_READ);
		if (Status != XST_SUCCESS) {
			xil_printf("Start read transfer failed %d\r\n", Status);
			return XST_FAILURE;
		}
	}
	return XST_SUCCESS;
}
