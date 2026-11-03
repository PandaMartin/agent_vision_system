// =============================================================================
// gpio_i2c —— GPIO位操作模拟I2C引擎(参数化: 引脚号 + 节拍延时)
//
// 合并原touch/myiic(IIC_*)与ov5640/emio_sccb_cfg(sccb_*)两套约90%重复的
// 位操作实现。引脚操作经emio_gpio(全板唯一实例), 时序半节拍延时按总线
// 实例配置:
//   触摸总线 delay_us=2 (原myiic节奏, ~300kbps)
//   OV5640 SCCB delay_us=4 (取原sccb混合延时的较慢一侧, 保守)
//
// SCCB语义映射(原sccb_* -> 本引擎):
//   sccb_ack(第9拍释放SDA不校验)     == gi2c_wait_ack(忽略返回值)
//   sccb_rece_byte+sccb_host_no_ack  == gi2c_read_byte(ack=0)
//
// 分层: bus层, 供dev层(触摸芯片/OV5640)实例化各自的总线
// =============================================================================

#ifndef HW_BUS_GPIO_I2C_H
#define HW_BUS_GPIO_I2C_H

#include <stdint.h>

typedef struct {
    uint32_t scl_pin;    //SCL引脚(GPIO编号 = 54个MIO + EMIO位号)
    uint32_t sda_pin;    //SDA引脚
    uint32_t delay_us;   //半节拍延时(us), 决定总线速率
} gpio_i2c_t;

//总线初始化: GPIO实例就绪 + 两线输出拉高(空闲态)
void gi2c_init(const gpio_i2c_t* bus);

//产生起始信号(SCL高时SDA由高变低)
void gi2c_start(const gpio_i2c_t* bus);

//产生停止信号(SCL高时SDA由低变高)
void gi2c_stop(const gpio_i2c_t* bus);

//第9个时钟等待应答。返回1=无应答(超250次自动Stop), 0=收到应答。
//SCCB场景调用后忽略返回值即可(电学行为等同释放SDA的don't-care第9拍)
int gi2c_wait_ack(const gpio_i2c_t* bus);

//发送一个字节(MSB first)
void gi2c_send_byte(const gpio_i2c_t* bus, uint8_t b);

//接收一个字节; ack=1回ACK继续读, ack=0回NAck结束读
uint8_t gi2c_read_byte(const gpio_i2c_t* bus, int ack);

#endif /* HW_BUS_GPIO_I2C_H */
