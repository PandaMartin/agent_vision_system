// =============================================================================
// axi_gpio —— PL端AXI GPIO IP薄包装实现
// =============================================================================

#include "axi_gpio.h"

#include "xgpio.h"

static XGpio s_gpio;      //静态实例(当前仅屏ID一处使用)
static int   s_opened = 0;

int axi_gpio_open(u32 baseaddr)
{
    //SDT流程下按基地址初始化
    if (XGpio_Initialize(&s_gpio, baseaddr) != XST_SUCCESS)
        return -1;
    s_opened = 1;
    return 0;
}

u32 axi_gpio_read(u32 channel)
{
    (void)s_opened;
    return XGpio_DiscreteRead(&s_gpio, channel);
}

void axi_gpio_set_dir(u32 channel, u32 dirmask)
{
    XGpio_SetDataDirection(&s_gpio, channel, dirmask);
}
