// =============================================================================
// loader —— SD应用加载器实现(运行时侧: 应用计时器记账 + 退出请求)
//
// 加载搬运在main.cpp的app_task(FatFs纪律); 本文件只做gui侧运行时:
//   - app_timer记账表: 应用创建的定时器全部登记, 退出时由launcher统一删除
//     (直接lv_timer_create的应用定时器会泄漏, 见loader.h契约)
//   - fw_app_exit: 置bridge退出标志, launcher tick里完成退出序列
// 全部函数只在gui任务上下文调用, 表无需加锁
// =============================================================================

#include <string.h>

#include "sw/app/ext_app/loader.h"
#include "sw/app/bridge.h"

#define APP_TIMER_MAX 8

static lv_timer_t* s_timers[APP_TIMER_MAX];
static int         s_timer_n;

lv_timer_t* app_timer_create(lv_timer_cb_t cb, uint32_t period_ms,
                             void* user_data)
{
    if (s_timer_n >= APP_TIMER_MAX)
        return 0;                            //超限: 应用侧自查(返回NULL)
    lv_timer_t* t = lv_timer_create(cb, period_ms, user_data);
    if (t)
        s_timers[s_timer_n++] = t;
    return t;
}

void app_timer_del(lv_timer_t* t)
{
    if (t == 0)
        return;
    for (int i = 0; i < s_timer_n; i++)
    {
        if (s_timers[i] == t)
        {
            s_timers[i] = s_timers[--s_timer_n];
            break;
        }
    }
    lv_timer_del(t);
}

//launcher在调用entry前/退出清理后重置记账表
void loader_timers_reset(void)
{
    s_timer_n = 0;
}

//launcher退出序列: 删除全部登记的定时器(应用泄漏的也在此兜住)
void loader_timers_kill(void)
{
    while (s_timer_n > 0)
        lv_timer_del(s_timers[--s_timer_n]);
}

void fw_app_exit(void)
{
    gui_model_pub_app_state(APPST_EXIT_REQ, 0);
}
