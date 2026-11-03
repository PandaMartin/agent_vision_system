// =============================================================================
// servo —— TTC硬件PWM舵机驱动实现(见servo.h顶部说明)
//
// TTC波形发生器(UG585): interval+match双模式下, 输出在"计数到match"与
// "计数回卷到interval"两事件处各翻转一次 => PWM, 周期=interval计数。
// 波形用硬件Match1寄存器(驱动侧MatchIndex=0)。
// 两次翻转事件的先后决定高脉冲=match还是=周期-match, 该极性无法从
// 软件读回, 做成运行时可切(servo_toggle_polarity), 用舵机行为现场判定。
//
// 本板实测确认(2026-10-17, 16_servo归中实验):
//   高脉冲 = 周期-match (s_match_direct=0), 极性已按实测固定
// =============================================================================

#include "servo.h"

#include "xparameters.h"
#include "xttcps.h"
#include "xil_printf.h"

// -----------------------------------------------------------------------------
// 配置

//预分频: divider=2^(N+1), N=5 -> /64, 计数1.736MHz(0.576us/计数)
#define SERVO_PRESCALER_IDX     5u
#define SERVO_PWM_FREQ_HZ       50u                 //20ms周期

//首装安全限幅(舵机行程0~180°, 云台装好确认无干涉后可放宽宏)
#define SERVO_ANGLE_MIN         30.0f
#define SERVO_ANGLE_MAX         150.0f

//脉宽范围(us): 标准180°舵机
#define SERVO_PULSE_MIN_US      500u
#define SERVO_PULSE_MID_US      1500u
#define SERVO_PULSE_MAX_US      2500u

//通道 -> TTC计数器(SDT流程按基地址查):
//counter0(0xF8001000)->WAVE0->W8->pan; counter1(0xF8001004)->WAVE1->U9->tilt
static const u32 ch_base[SERVO_CH_N] = {
    XPAR_XTTCPS_0_BASEADDR,
    XPAR_XTTCPS_1_BASEADDR,
};

typedef struct
{
    XTtcPs  ttc;
    u32     tick_hz;        //分频后计数时钟
    u16     interval;       //20ms周期的计数值
    u16     pulse_us;       //当前目标脉宽
    float   angle;          //当前角度(与pulse_us同源, 供显示)
} ServoCtx;

static ServoCtx s_ch[SERVO_CH_N];
static int s_inited = 0;
//实测确认(2026-10-17, 舵机归中实验): 本板TTC波形高脉冲=周期-match,
//即输出在"计数到match"处拉低、"回卷到interval"处拉高
static int s_match_direct = 0;

// -----------------------------------------------------------------------------
// 内部

//脉宽(us) -> 计数值(四舍五入): counts = us/1e6 * tick_hz
static u16 pulse_to_counts(u32 tick_hz, u16 width_us)
{
    u64 c = (u64)width_us * (u64)tick_hz + 500000u;
    c /= 1000000u;
    return (u16)c;
}

//角度 -> 脉宽(us); +0.5四舍五入保证90°精确对应1500us
static u16 angle_to_pulse(float deg)
{
    if (deg < SERVO_ANGLE_MIN)  deg = SERVO_ANGLE_MIN;
    if (deg > SERVO_ANGLE_MAX)  deg = SERVO_ANGLE_MAX;
    return (u16)(SERVO_PULSE_MIN_US + (u32)(deg * (2000.0f / 180.0f) + 0.5f));
}

//把当前脉宽写进match寄存器(按极性映射)
static void ch_apply_pulse(ServoCtx* ch)
{
    const u16 c = pulse_to_counts(ch->tick_hz, ch->pulse_us);
    const u16 m = s_match_direct ? c : (u16)(ch->interval - c);
    XTtcPs_SetMatchValue(&ch->ttc, 0, m);       //波形只用Match1(驱动index 0)
}

//单通道初始化: interval+match双模式即PWM, 启动后硬件自保持
static int ch_setup(ServoCtx* ch, u32 base)
{
    XTtcPs_Config* cfg = XTtcPs_LookupConfig(base);
    if (cfg == 0)
    {
        xil_printf("[servo] TTC config not found @0x%08x\r\n", base);
        return -1;
    }
    if (XTtcPs_CfgInitialize(&ch->ttc, cfg, cfg->BaseAddress) != XST_SUCCESS)
    {
        xil_printf("[servo] TTC init failed @0x%08x\r\n", base);
        return -1;
    }

    ch->tick_hz = cfg->InputClockHz / (1u << (SERVO_PRESCALER_IDX + 1u));
    const u32 opts = XTTCPS_OPTION_INTERVAL_MODE | XTTCPS_OPTION_MATCH_MODE;
    XTtcPs_SetOptions(&ch->ttc, opts);
    XTtcPs_SetPrescaler(&ch->ttc, SERVO_PRESCALER_IDX);
    ch->interval = (u16)(cfg->InputClockHz
                         / ((1u << (SERVO_PRESCALER_IDX + 1u)) * SERVO_PWM_FREQ_HZ));
    XTtcPs_SetInterval(&ch->ttc, ch->interval);

    ch->pulse_us = SERVO_PULSE_MID_US;
    ch->angle = 90.0f;
    ch_apply_pulse(ch);

    XTtcPs_ResetCounterValue(&ch->ttc);
    XTtcPs_Start(&ch->ttc);
    return 0;
}

// -----------------------------------------------------------------------------
// 对外接口

int servo_init(void)
{
    if (s_inited)
        return 0;
    for (int i = 0; i < SERVO_CH_N; i++)
    {
        if (ch_setup(&s_ch[i], ch_base[i]) != 0)
            return -1;
    }
    s_inited = 1;

    xil_printf("[servo] TTC PWM 50Hz: interval=%u, tick=%u Hz\r\n",
               s_ch[0].interval, s_ch[0].tick_hz);
    xil_printf("[servo] mid pulse %u us = %u counts\r\n", SERVO_PULSE_MID_US,
               pulse_to_counts(s_ch[0].tick_hz, SERVO_PULSE_MID_US));
    return 0;
}

int servo_ready(void)
{
    return s_inited;
}

void servo_center(void)
{
    for (int i = 0; i < SERVO_CH_N; i++)
        servo_set_angle((ServoCh)i, 90.0f);
}

void servo_set_angle(ServoCh ch, float deg)
{
    if (!s_inited || ch >= SERVO_CH_N)
        return;
    ServoCtx* c = &s_ch[ch];
    if (deg < SERVO_ANGLE_MIN)  deg = SERVO_ANGLE_MIN;
    if (deg > SERVO_ANGLE_MAX)  deg = SERVO_ANGLE_MAX;
    c->angle = deg;
    c->pulse_us = angle_to_pulse(deg);
    ch_apply_pulse(c);
}

float servo_get_angle(ServoCh ch)
{
    return (ch < SERVO_CH_N) ? s_ch[ch].angle : 0.0f;
}

void servo_set_pulse(ServoCh ch, u16 width_us)
{
    if (!s_inited || ch >= SERVO_CH_N)
        return;
    ServoCtx* c = &s_ch[ch];
    if (width_us < SERVO_PULSE_MIN_US)  width_us = SERVO_PULSE_MIN_US;
    if (width_us > SERVO_PULSE_MAX_US)  width_us = SERVO_PULSE_MAX_US;
    c->pulse_us = width_us;
    c->angle = (float)(width_us - SERVO_PULSE_MIN_US) * (180.0f / 2000.0f);
    ch_apply_pulse(c);
}

u16 servo_get_pulse(ServoCh ch)
{
    return (ch < SERVO_CH_N) ? s_ch[ch].pulse_us : 0;
}

void servo_toggle_polarity(void)
{
    if (!s_inited)
        return;
    s_match_direct ^= 1;
    for (int i = 0; i < SERVO_CH_N; i++)
        ch_apply_pulse(&s_ch[i]);
    xil_printf("[servo] polarity: high-pulse = %s\r\n",
               s_match_direct ? "match" : "interval-match");
}

void servo_print_info(void)
{
    if (!s_inited)
    {
        xil_printf("[servo] not inited\r\n");
        return;
    }
    const ServoCtx* c = &s_ch[0];
    const u32 period_us = (u32)((u64)1000000u * c->interval / c->tick_hz);
    xil_printf("[servo] pwm %u Hz, period=%u us, prescaler=/64, "
               "high-pulse=%s\r\n", SERVO_PWM_FREQ_HZ, period_us,
               s_match_direct ? "match" : "interval-match");
    xil_printf("[servo] pan : W8,  %u deg, %u us\r\n",
               (u32)(s_ch[SERVO_PAN].angle + 0.5f), s_ch[SERVO_PAN].pulse_us);
    xil_printf("[servo] tilt: U9,  %u deg, %u us\r\n",
               (u32)(s_ch[SERVO_TILT].angle + 0.5f), s_ch[SERVO_TILT].pulse_us);
}
