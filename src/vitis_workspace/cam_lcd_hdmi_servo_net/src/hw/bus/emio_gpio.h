// =============================================================================
// emio_gpio —— PS GPIO(含EMIO)唯一实例与通用引脚操作
//
// 全板只允许这一份XGpioPs实例: 原touch/emio_iic_cfg与ov5640/emio_sccb_cfg
// 各自LookupConfig初始化的重复实现收敛于此, 引脚按编号直接操作。
// GPIO编号 = 54个MIO + EMIO位号(本板分配: EMIO[0:1]=OV5640 SCCB,
// EMIO[2:5]=触摸SCL/SDA/RST/INT, 见各器件驱动)。
//
// 分层: bus层(最底层, 只碰GPIO寄存器); 上层为gpio_i2c与各dev
// =============================================================================

#ifndef HW_BUS_EMIO_GPIO_H
#define HW_BUS_EMIO_GPIO_H

#include <stdint.h>

//初始化唯一GPIO实例(幂等, 内部只执行一次; gi2c_init会自动调用)
void emio_gpio_init(void);

//设为输出并写电平
void emio_pin_out(uint32_t pin, uint8_t level);

//设为输入(方向+输出使能都关)
void emio_pin_in(uint32_t pin);

//输出电平(假定方向已配置)
void emio_pin_write(uint32_t pin, uint8_t level);

//读输入电平
uint8_t emio_pin_read(uint32_t pin);

#endif /* HW_BUS_EMIO_GPIO_H */
