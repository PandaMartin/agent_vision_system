// =============================================================================
// nn_wdt —— 模型子系统看门狗实现(见 nn_wdt.h)
//
// 全部限 app_task 上下文调用(与 model_mgr 同规)。SCU WDT看门狗模式两拍
// 超时: 第一拍到引中断(无处理器, 默认返回), 第二拍到硬件复位PS(DDR保留)。
// =============================================================================

#include "sw/impl/nn/nn_wdt.h"

#include "hw/mem_layout.h"     //MEM_GUARD_SCRATCH_ADDR(挂死计数scratch, DDR真顶部)

#include "xil_types.h"
#include "xparameters.h"
#include "xscuwdt.h"
#include "xstatus.h"

//≈4s: SCU WDT计数时钟≈CPU/4(666MHz→166.7MHz), 32位计数最大≈25s
#define NN_WDT_COUNTS   666666666u

static XScuWdt s_wdt;
static int     s_wdt_ready = 0;

// -----------------------------------------------------------------------------
void nn_wdt_arm(void)
{
    if (!s_wdt_ready)
    {
        XScuWdt_Config* cfg = XScuWdt_LookupConfig((UINTPTR)XPAR_SCUWDT_BASEADDR);
        if (cfg == 0)
            return;
        if (XScuWdt_CfgInitialize(&s_wdt, cfg,
                                  (u32)XPAR_SCUWDT_BASEADDR) != XST_SUCCESS)
            return;
        s_wdt_ready = 1;
    }
    XScuWdt_SetWdMode(&s_wdt);                  //看门狗模式(超时→复位)
    XScuWdt_LoadWdt(&s_wdt, NN_WDT_COUNTS);
    XScuWdt_Start(&s_wdt);
}

void nn_wdt_kick(void)
{
    if (s_wdt_ready)
        XScuWdt_RestartWdt(&s_wdt);             //重载计数
}

void nn_wdt_disarm(void)
{
    if (s_wdt_ready)
    {
        XScuWdt_SetTimerMode(&s_wdt);           //回定时器模式(不再复位)
        XScuWdt_Stop(&s_wdt);
    }
}

// -----------------------------------------------------------------------------
//挂数scratch(地址登记在 mem_layout.h, DDR真顶部, 与任何固件段不相交):
//[0]=magic [1]=连续挂死计数; WDT复位保留, 上电随机由magic识别
#define NN_GUARD_SCRATCH   MEM_GUARD_SCRATCH_ADDR
#define NN_GUARD_MAGIC     0x4E4E4744u   //"NNGD"

int nn_guard_take_skip_slot(void)
{
    volatile u32* p = (volatile u32*)NN_GUARD_SCRATCH;
    if (p[0] == NN_GUARD_MAGIC)
        p[1]++;
    else
    {
        p[0] = NN_GUARD_MAGIC;
        p[1] = 0;
    }
    if (p[1] >= NN_GUARD_MAX)
    {
        p[1] = 0;                               //触发后清零, 下轮扫描重试
        return 1;
    }
    return 0;
}

void nn_guard_clear(void)
{
    volatile u32* p = (volatile u32*)NN_GUARD_SCRATCH;
    p[0] = 0;
    p[1] = 0;
}
