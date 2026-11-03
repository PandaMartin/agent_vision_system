// =============================================================================
// emio_gpio —— PS GPIO(含EMIO)唯一实例与通用引脚操作实现
// =============================================================================

#include "emio_gpio.h"

#include "xgpiops.h"
#include "xparameters.h"

//SDT流程下xparameters.h不再定义DEVICE_ID, LookupConfig改用基地址
#define GPIOPS_BASEADDR  XPAR_XGPIOPS_0_BASEADDR

static XGpioPs s_gpiops;        //全板唯一的PS GPIO驱动实例
static int     s_inited = 0;    //幂等初始化标志

void emio_gpio_init(void)
{
    if (s_inited)
        return;
    XGpioPs_Config* cfg = XGpioPs_LookupConfig(GPIOPS_BASEADDR);
    XGpioPs_CfgInitialize(&s_gpiops, cfg, cfg->BaseAddr);
    s_inited = 1;
}

void emio_pin_out(uint32_t pin, uint8_t level)
{
    XGpioPs_SetDirectionPin(&s_gpiops, pin, 1);
    XGpioPs_SetOutputEnablePin(&s_gpiops, pin, 1);
    XGpioPs_WritePin(&s_gpiops, pin, level);
}

void emio_pin_in(uint32_t pin)
{
    XGpioPs_SetDirectionPin(&s_gpiops, pin, 0);
    XGpioPs_SetOutputEnablePin(&s_gpiops, pin, 0);
}

void emio_pin_write(uint32_t pin, uint8_t level)
{
    XGpioPs_WritePin(&s_gpiops, pin, level);
}

uint8_t emio_pin_read(uint32_t pin)
{
    return (uint8_t)XGpioPs_ReadPin(&s_gpiops, pin);
}
