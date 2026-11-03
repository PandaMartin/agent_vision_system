// =============================================================================
// lwip_echo_server_freertos —— FreeRTOS + lwIP220(SOCKET_API) TCP回环服务
//
// 移植自 standalone 的 lwip_echo_server 例程(已上板验证硬件链路OK):
//   · 硬件/XSA 与 standalone 平台完全一致(ps7_init零差异),
//     ENET0+MDIO+gem0时钟配置已验证可通(ping/echo实测)
//   · RAW_API在FreeRTOS上不可用(lwIP的DRC限制), 换SOCKET_API重写,
//     初始化序列与17主项目 net_server.c 完全相同——本应用同时是
//     17网口推送初始化序列(lwip_init+tcpip_init+xemac_add+收包线程)
//     的上板验证
//
// 行为: 静态IP 192.168.1.10/24(免DHCP等待), TCP 6001端口收啥回啥
// 验证: PC配192.168.1.100/24 -> ping 192.168.1.10
//       -> netassist连6001发任意文本看回显
// =============================================================================

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "xil_printf.h"
#include "xparameters.h"

#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "netif/xadapter.h"

#include "xiltimer.h"                      //XTime/sleep自诊断用
#include "sleep.h"

#define ECHO_PORT        6001

#define NW_STACK         1024    //字(4KB): 网络初始化线程(xemac_add较深)
#define ECHO_STACK       1024    //字(4KB): echo服务线程
#define IN_STACK         512     //字(2KB): 收包线程
#define THREAD_PRIO      2       //与lwipopts的DEFAULT_THREAD_PRIO一致
                                 //(tcpip线程由bsp配置为prio 3)

static struct netif server_netif;

//收包线程(xadapter.c定义, socket模式下由应用负责拉起)
extern void xemacif_input_thread(struct netif *netif);

//------------------------------------------------------------------------------
// echo服务线程: accept循环, 每连接收啥回啥
static void echo_thread(void *arg)
{
    (void)arg;

    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0)
    {
        xil_printf("[echo] socket() failed\r\n");
        vTaskDelete(0);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) < 0
        || listen(ls, 4) < 0)
    {
        xil_printf("[echo] bind/listen %d failed\r\n", ECHO_PORT);
        closesocket(ls);
        vTaskDelete(0);
    }
    xil_printf("[echo] TCP echo server listening on port %d\r\n", ECHO_PORT);

    char buf[1460];
    for (;;)
    {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        const int c = accept(ls, (struct sockaddr*)&cli, &clen);
        if (c < 0)
            continue;

        xil_printf("[echo] connect from %s\r\n",
                   inet_ntoa(cli.sin_addr));

        for (;;)
        {
            const int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0)
                break;                     //对端关闭/出错

            int sent = 0;                  //小回包完整发回
            while (sent < n)
            {
                const int w = send(c, buf + sent, n - sent, 0);
                if (w <= 0)
                    goto next;
                sent += w;
            }
        }
next:
        closesocket(c);
        xil_printf("[echo] connection closed\r\n");
    }
}

//------------------------------------------------------------------------------
// 网络初始化线程: 起协议栈 -> 配网卡 -> 拉收包线程 -> 起echo线程
static void network_thread(void *arg)
{
    (void)arg;

    //本lwIP树的tcpip_init不会自动调lwip_init(树内未定义LWIP_XINIT),
    //两个都要显式调; tcpip线程负责TCP定时器(重传/延迟ACK)
    lwip_init();
    tcpip_init(NULL, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));         //等tcpip线程就绪

    ip_addr_t ip, nm, gw;
    IP4_ADDR(&ip,  192, 168, 1, 10);
    IP4_ADDR(&nm,  255, 255, 255, 0);
    IP4_ADDR(&gw,  192, 168, 1, 1);

    //MAC地址(多板同网时手动改末字节)
    unsigned char mac[6] = { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };

    if (xemac_add(&server_netif, &ip, &nm, &gw, mac,
                  XPAR_XEMACPS_0_BASEADDR) == 0)
    {
        xil_printf("[net] xemac_add failed (GEM0 @0x%x)\r\n",
                   (unsigned)XPAR_XEMACPS_0_BASEADDR);
        vTaskDelete(0);
    }
    netif_set_default(&server_netif);
    netif_set_up(&server_netif);

    //收包线程(socket模式由应用负责; 中断收包经信号量唤醒喂给lwIP)
    sys_thread_new("xemac_in", (void(*)(void*))xemacif_input_thread,
                   &server_netif, IN_STACK, THREAD_PRIO);

    xil_printf("[net] up: 192.168.1.10, echo on tcp %d\r\n", ECHO_PORT);

    sys_thread_new("echo", echo_thread, 0, ECHO_STACK, THREAD_PRIO);
    vTaskDelete(0);                        //初始化线程使命完成
}

//------------------------------------------------------------------------------
int main()
{
    xil_printf("\r\n-----lwIP TCP echo server (FreeRTOS) ------\r\n");
    xil_printf("Board IP: 192.168.1.10, echo port %d\r\n\r\n", ECHO_PORT);

    //sleep自诊断: lwIP驱动里PHY自协商等待全靠sleep(1)(仅重试5次),
    //若它不真正延时, 自协商必失败(Auto negotiation error)。
    //全局定时器按CPU/2计数: 计数差应约等于 XPAR_CPU/2。
    {
        XTime t0, t1;
        XTime_GetTime(&t0);
        sleep(1);
        XTime_GetTime(&t1);
        xil_printf("[diag] CPU_CLK=%u, sleep(1)=%u counts "
                   "(正常应约%u)\r\n",
                   (unsigned)XPAR_CPU_CORE_CLOCK_FREQ_HZ,
                   (unsigned)(t1 - t0),
                   (unsigned)(XPAR_CPU_CORE_CLOCK_FREQ_HZ / 2));
    }

    sys_thread_new("nw", network_thread, 0, NW_STACK, THREAD_PRIO);

    vTaskStartScheduler();                 //正常情况下不返回

    xil_printf("scheduler returned, halt.\r\n");
    while (1) {}
    return 0;
}
