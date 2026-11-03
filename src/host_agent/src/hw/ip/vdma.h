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

#ifndef HW_IP_VDMA_H_
#define HW_IP_VDMA_H_

/* ------------------------------------------------------------ */
/*				Include File Definitions						*/
/* ------------------------------------------------------------ */

#include "xaxivdma.h"
#include "xparameters.h"
#include "xil_exception.h"

/* ------------------------------------------------------------ */
/*					General Type Declarations					*/
/* ------------------------------------------------------------ */

typedef enum
{
	ONLY_READ=1,    //VDMA只开启读通道
	ONLY_WRITE,     //VDMA只开启写通道
	BOTH            //同时开启VDMA写通道和读通道
}vdma_run_mode;

/* ------------------------------------------------------------ */
/*					Procedure Declarations						*/
/* ------------------------------------------------------------ */
int run_vdma_frame_buffer(XAxiVdma* InstancePtr, UINTPTR BaseAddress, int hsize,
		int vsize, int buf_base_addr, int number_frame_count,
		int enable_frm_cnt_intr,vdma_run_mode mode);

//GenLock(EnableSync)配置期开关: 1=通道带GenlockEn(动态GenLock主/从帧同步,
//防撕裂); 0=通道不带(纯自由循环)。须在run_vdma_frame_buffer之前调用。
//只影响之后的通道配置, 运行期切换用vdma_write_genlock
void vdma_genlock(int enable);

//运行期切换写通道GenlockEn(PG020: 置0后"Genlock input is ignored by S2MM",
//动态GenLock主设备变回普通自由循环, 不再受读从设备停跑影响 —— HDMI关闭
//前必须先解耦, 否则历史实测会反压拖垮写通道)。置1恢复主/从帧同步(防撕裂)。
//内部经XAxiVdma_DmaConfig只改SYNC_EN等位, RS/Circular保持, 通道不重启
//baseaddr: VDMA基地址(查找时复用配置上下文); 返回0成功
int vdma_write_genlock(XAxiVdma* InstancePtr, UINTPTR baseaddr, int enable);
/* ------------------------------------------------------------ */

/************************************************************************/

#endif /* HW_IP_VDMA_H_ */





