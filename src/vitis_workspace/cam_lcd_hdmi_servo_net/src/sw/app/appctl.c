// =============================================================================
// appctl —— 应用命令总线实现(FreeRTOS队列的一层薄封装)
// =============================================================================

#include "appctl.h"

#include "FreeRTOS.h"
#include "queue.h"

//队列深度: 推理期间连续触摸先排队(满了丢弃, 语义即合并)
#define APPCTL_QUEUE_LEN 16

static QueueHandle_t s_queue;

int appctl_init(void)
{
    s_queue = xQueueCreate(APPCTL_QUEUE_LEN, sizeof(AppCmd));
    return s_queue != 0 ? 0 : -1;
}

int appctl_post(const AppCmd* cmd)
{
    return xQueueSend(s_queue, cmd, 0) == pdTRUE ? 0 : -1;
}

int appctl_take(AppCmd* cmd, TickType_t wait)
{
    return xQueueReceive(s_queue, cmd, wait) == pdTRUE;
}

void appctl_post_front(const AppCmd* cmd)
{
    xQueueSendToFront(s_queue, cmd, 0);
}
