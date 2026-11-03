// =============================================================================
// servo —— TTC硬件PWM双轴云台舵机驱动(16_servo已实测: 归中+极性判定)
//
// 硬件: TTC0两路波形经EMIO引出到PL管脚(XDC已约束):
//   TTC0_WAVE0_OUT -> W8 -> 水平(pan)舵机信号   (TTC0 counter0)
//   TTC0_WAVE1_OUT -> U9 -> 俯仰(tilt)舵机信号  (TTC0 counter1)
// 波形: 50Hz标准RC舵机PWM, 高脉宽500~2500us线性对应0~180°(1500us=90°归中)
//
// 参数: 预分频/64 -> 计数1.736MHz, 周期34722计数(16位内), 0.576us/计数
// 波形由TTC硬件持续生成, 设置match后零CPU开销, 舵机位置自动保持
//
// 注意: 舵机5V请用外部电源(≥2A)并与板共地, 板载5V带不动连续转动
// =============================================================================

#ifndef HW_DEV_SERVO_H
#define HW_DEV_SERVO_H

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    SERVO_PAN = 0,      //水平轴(TTC0 counter0 -> W8)
    SERVO_TILT = 1,     //俯仰轴(TTC0 counter1 -> U9)
    SERVO_CH_N
} ServoCh;

//初始化两路50Hz硬件PWM并双轴归中90°; 返回0成功(失败已打印)
int servo_init(void);

//就绪查询(=1可受控; init失败时set/center为安全空操作, UI以此提示)
int servo_ready(void);

//双轴归中(90°)
void servo_center(void);

//设角度, 限幅[SERVO_ANGLE_MIN, SERVO_ANGLE_MAX](30~150, 改.c里的宏放宽)
void servo_set_angle(ServoCh ch, float deg);
float servo_get_angle(ServoCh ch);

//原始脉宽直设(500~2500us, 绕过角度限幅, 标定机械极限用)
void servo_set_pulse(ServoCh ch, u16 width_us);
u16 servo_get_pulse(ServoCh ch);

//脉宽极性取反: 高脉冲=match <-> 高脉冲=周期-match
//TTC波形极性无法软件读回, 若舵机对命令毫无反应/乱抖, 按一次即可修正
void servo_toggle_polarity(void);

//打印当前PWM参数(周期/脉宽/角度/极性)
void servo_print_info(void);

#ifdef __cplusplus
}
#endif

#endif /* HW_DEV_SERVO_H */
