// =============================================================================
// gpio_i2c —— GPIO位操作模拟I2C引擎实现
//
// 时序逐行平移自原touch/myiic.c(正点原子36_touch_draw_lcd), 引脚操作由
// 硬编码ct_*改为按总线实例的引脚号经emio_gpio操作, 延时由固定usleep(2)
// 改为按总线实例配置(两线时序本身对延时极不敏感, SCCB/I2C规格均兼容)
// =============================================================================

#include "gpio_i2c.h"

#include "sleep.h"

#include "emio_gpio.h"

//半节拍延时
static void dly(const gpio_i2c_t* bus)
{
    usleep(bus->delay_us);
}

void gi2c_init(const gpio_i2c_t* bus)
{
    emio_gpio_init();
    emio_pin_out(bus->scl_pin, 1);   //总线空闲态拉高
    emio_pin_out(bus->sda_pin, 1);
}

void gi2c_start(const gpio_i2c_t* bus)
{
    emio_pin_out(bus->sda_pin, 1);   //sda线输出
    emio_pin_write(bus->scl_pin, 1);
    dly(bus);
    emio_pin_write(bus->sda_pin, 0); //START: SCL为高时SDA由高变低
    dly(bus);
    emio_pin_write(bus->scl_pin, 0); //钳住I2C总线, 准备发送或接收数据
    dly(bus);
}

void gi2c_stop(const gpio_i2c_t* bus)
{
    emio_pin_out(bus->sda_pin, 0);   //sda线输出(拉低)
    emio_pin_write(bus->scl_pin, 0);
    emio_pin_write(bus->sda_pin, 0); //STOP: SCL为高时SDA由低变高
    dly(bus);
    emio_pin_write(bus->scl_pin, 1);
    dly(bus);
    emio_pin_write(bus->sda_pin, 1); //发送I2C总线结束信号
    dly(bus);
}

int gi2c_wait_ack(const gpio_i2c_t* bus)
{
    uint8_t ucErrTime = 0;
    emio_pin_in(bus->sda_pin);       //SDA设置为输入
    emio_pin_write(bus->sda_pin, 1);
    dly(bus);
    emio_pin_write(bus->scl_pin, 1);
    dly(bus);
    while (emio_pin_read(bus->sda_pin))
    {
        ucErrTime++;
        if (ucErrTime > 250)
        {
            gi2c_stop(bus);
            return 1;                //应答失败
        }
    }
    emio_pin_write(bus->scl_pin, 0); //时钟输出0
    dly(bus);
    return 0;                        //应答成功
}

static void gi2c_ack(const gpio_i2c_t* bus)
{
    emio_pin_write(bus->scl_pin, 0);
    emio_pin_out(bus->sda_pin, 0);
    dly(bus);
    emio_pin_write(bus->scl_pin, 1);
    dly(bus);
    emio_pin_write(bus->scl_pin, 0);
    dly(bus);
}

static void gi2c_nack(const gpio_i2c_t* bus)
{
    emio_pin_write(bus->scl_pin, 0);
    emio_pin_out(bus->sda_pin, 1);
    dly(bus);
    emio_pin_write(bus->scl_pin, 1);
    dly(bus);
    emio_pin_write(bus->scl_pin, 0);
    dly(bus);
}

void gi2c_send_byte(const gpio_i2c_t* bus, uint8_t txd)
{
    uint8_t t;
    emio_pin_out(bus->sda_pin, 1);   //sda线输出
    emio_pin_write(bus->scl_pin, 0); //拉低时钟开始数据传输
    for (t = 0; t < 8; t++)
    {
        emio_pin_write(bus->sda_pin, (txd & 0x80) >> 7);
        txd <<= 1;
        dly(bus);
        emio_pin_write(bus->scl_pin, 1);
        dly(bus);
        emio_pin_write(bus->scl_pin, 0);
        dly(bus);
    }
}

uint8_t gi2c_read_byte(const gpio_i2c_t* bus, int ack)
{
    uint8_t i, receive = 0;
    emio_pin_in(bus->sda_pin);       //SDA设置为输入
    for (i = 0; i < 8; i++)
    {
        emio_pin_write(bus->scl_pin, 0);
        dly(bus);
        emio_pin_write(bus->scl_pin, 1);
        receive <<= 1;
        if (emio_pin_read(bus->sda_pin))
            receive++;
        dly(bus);
    }
    if (!ack)
        gi2c_nack(bus);              //发送nACK
    else
        gi2c_ack(bus);               //发送ACK
    return receive;
}
