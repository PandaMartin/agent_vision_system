// =============================================================================
// net_server —— 网口文件接收服务实现(标准TFTP服务端, UDP 69)
//
// 传输协议自定制TCP(FWPUT2)重构为标准TFTP(RFC1350, 参考正点原子
// 42_lwip_tftp_server例程): PC侧可用任意标准tftp客户端推送, 自家
// tools/netpush/net_push.py为TFTP客户端(带进度/重发)。
//
// 初始化序列与 lwip_echo_server_freertos 完全一致(18平台上板验证):
//   lwip_init() + tcpip_init() 都要显式调(本lwIP树未定义LWIP_XINIT,
//   tcpip_init不会自动带lwip_init; tcpip线程负责定时器)
//   -> xemac_add -> netif_set_default/up -> 应用自拉的收包线程
// BSP侧非默认配置(完整备案与恢复流程见 src/docs/平台网络配置记录.md,
// 一键核对: python tools/bsp_net_check.py):
//   UI参数 temac_phy_link_speed=CONFIG_LINKSPEED_AUTODETECT
//     (勿设强制速率: 强制100M与PC网卡并行检测失败链路起不来, 1000违反标准);
//   libsrc手改 lwipopts.h.in 加 #define LWIP_SO_RCVTIMEO 1
//     (阻塞收包可超时; 缺失则应用自动回退轮询, 速度减半以上);
//   libsrc手改 自协商等待5s->20s(项目早期补丁, 4处; 开机网络可靠性依赖)。
//
// 与参考例程的差异(本项目的硬约束): 例程在udp_recv回调里直接f_open/f_write,
// 而本项目有"FatFs单线程纪律"(只有app_task能碰文件系统, 见sd.h)。因此
// 收流路径改为: TFTP会话收满整文件进堆(动态倍增, 上限64MB) -> 经
// APP_CMD_NET_FILE移交app_task落SD(异步) -> app_task写完自发布UI结果,
// 失败时反向调net_server_write_fail向对端补发TFTP ERROR(尽力而为)。
//
// 关键设计——落SD异步, 服务永不等盘(2026-10-22压测教训): 初版在net任务里
// 等落SD回执再回监听, 结果app_task一旦迟滞(SD写尖峰), net任务连同69端口
// 整体停摆, 客户端WRQ全部无应答且持续远超设计超时(真机复现: 连推10个小
// 文件, 第5个起"写SD…"卡住, 此后WRQ数分钟无人应答)。改为尾块ACK后立即
// 投递队列+返回监听: 落盘再慢也只排队, 不占服务; 客户端视角传输在尾ACK
// 即已成功, 落盘结果以板面为准(自家net_push.py收尾后按体量多听片刻尽力
// 捕获失败ERROR)。会话串行指收流串行; 落盘天然由app_task串行。
//
// 服务范围: 只支持WRQ(PC向板推送, 即原FWPUT2的用途), RRQ回错误码2
// (net任务读SD违反FatFs纪律); 模式只认octet; 文件名映射规则:
//   "0:/..."开头原样使用; 裸名/相对路径一律挂"0:/"根下(标准tftp客户端
//   无法指定SD全路径); 含".."或非ASCII字节拒绝。
//   重复DATA(块号=上一块)=己方ACK丢失, 重发ACK; 其余乱序丢弃(客户端靠
//   重发自愈); 会话包源地址/端口须与发起方一致(TID校验), 不一致忽略。
//
// UDP相对TCP退役的健壮性条目(FWPUT2时代真机踩坑记录, 换协议后天然消失):
//   - accept()不受超时约束的永久阻塞+自探针证实(UDP recvfrom吃RCVTIMEO,
//     且主socket轮询模式每拍都让出CPU并刷新活动时刻);
//   - TIME_WAIT PCB堆积耗尽MEMP_NUM_TCP_PCB(UDP无连接, 无TIME_WAIT);
//   - 会话末被动关闭等FIN(UDP无连接, 直接close)。
// 仍然保留的纪律:
//   - 应答(ACK/ERROR)一律MSG_DONTWAIT发送, 发不出去就丢——TFTP靠客户端
//     重发自愈, 服务循环绝不能挂在发送上(本BSP无LWIP_SO_SNDTIMEO);
//   - 所有等待有界且检查s_stop(应用退出<=1个超时周期内中止会话);
//   - 落SD异步(服务永不等盘), 见下方"关键设计"。
//
// 任务模型: nstack任务一个(优先级1, 与gui/app同级时间片, 防止任何意外
// 忙等饿死界面), 栈初始化后常驻"监听请求"循环(信号量驱动); 单会话串行。
//
// 看门狗(net_server_watchdog, netwdt任务1s节拍, 不依赖app_task):
//   - 栈初始化45s未就绪(合法自协商<=约25s)判死, 打开应用直接报失败;
//   - "该监听却缺席>3s"(socket/bind失败后循环已退)弹票重试;
//   - 活动时刻冻结>10s=卡在系统调用内(主socket轮询每拍都刷新活动时刻,
//     与accept时代"健康阻塞与卡死外观相同"不同, 冻结只可能是真卡死,
//     无需自探针): closesocket监听/会话fd撬醒, 取票重建。
// =============================================================================

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "xil_printf.h"
#include "xparameters.h"

#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "netif/xadapter.h"

#include "sw/impl/net/net_server.h"
#include "sw/app/bridge.h"
#include "sw/app/appctl.h"
#include "sw/app/fw_version_gen.h"

//板端固定地址(PC配 192.168.1.100/24 直连); TFTP标准端口69
#define NET_IP0        192
#define NET_IP1        168
#define NET_IP2        1
#define NET_IP3        10
#define NET_PORT       69       //TFTP标准端口(RFC1350)

#define NET_STACK      2048     //字(8KB): 会话(收流/发布)在nstack任务内
#define NET_PRIO       1        //与gui/app同级时间片: 意外循环也不饿死界面
#define IN_STACK       512      //字(2KB): 收包线程(与echo例程一致)
#define WDT_STACK      1024     //字(4KB): 看门狗任务(含socket撬锁调用)

//TFTP协议常量(RFC1350, 固定512字节块, 不协商blksize——标准客户端最兼容)
#define TFTP_BLK       512      //数据段上限; <512即尾块
#define TFTP_HDR       4        //操作码(2)+块号(2)
#define TFTP_PKT_MAX   (TFTP_HDR + TFTP_BLK)   //单包上限(DATA满块)
#define TFTP_PATH_MAX  128      //WRQ文件名接收上限(后再映射到48字节SD路径)

//TFTP操作码
#define TFTP_RRQ       1        //读请求(板不支持, 回错误)
#define TFTP_WRQ       2        //写请求(PC推送, 本服务的主路径)
#define TFTP_DATA      3
#define TFTP_ACK       4
#define TFTP_ERROR     5

//TFTP错误码(RFC1350)
#define TFTP_E_NOTDEF  0        //not defined(附错误文本)
#define TFTP_E_ACCESS  2        //access violation(路径/权限拒绝)
#define TFTP_E_FULL    3        //disk full(内存/落盘失败)
#define TFTP_E_ILLEGAL 4        //illegal TFTP operation(模式/包异常)

#define NET_MAX_SIZE   (64u * 1024u * 1024u)   //单文件上限(整文件进堆)

#define NET_POLL_MS    100      //主socket轮询周期(空转时让出CPU)
#define NET_DATA_MS    20       //recv无数据时的等待粒度(回退路径, 让出CPU)
#define NET_T_POLL_MS  400      //会话socket阻塞recv的SO_RCVTIMEO
#define NET_T_FIRST_MS 5000     //WRQ后等首块DATA时限(死客户端会话快速收敛)
#define NET_T_IDLE_MS  30000    //数据流链路级空闲超时
#define NET_T_WDT_MS   10000    //监听器无活动阈值(看门狗踢锁)
#define NET_T_WANT_MS  3000     //监听意图在而监听器缺席的宽限(正常启停<0.5s)
#define NET_T_INIT_MS  45000    //栈初始化判死阈值(PHY自协商合法上限约25s+余量)

static struct netif s_netif;
static volatile int s_stack_ok;              //栈初始化完成(监听请求的前提)
static volatile int s_stack_dead;            //栈判死(初始化卡死/任务没起来): 起监听直接失败
static volatile TickType_t s_boot_tick;      //nstack启动时刻(初始化看门狗基准)
static volatile int s_stop;                  //停止监听+中止会话(应用退出置位)
static volatile int s_want_listen;           //应用侧监听意图(stop清除; 陈旧票据据此丢弃)
static volatile TickType_t s_want_tick;      //意图置位时刻(监听缺席型看门狗的宽限基准)
static SemaphoreHandle_t s_req_sem;          //监听请求(net_server_start给票)
static volatile TickType_t s_listen_tick;    //net侧最后活动时刻(所有等待循环都刷新;
                                             //冻结>10s=卡在不返回的系统调用里)
static volatile int s_listening;             //监听循环是否在运行
static volatile int s_ls_fd = -1;            //当前监听socket(看门狗撬锁用; -1无)
static volatile int s_ls_kicked;             //监听fd已被看门狗外部关闭(退出路径不再close防复用误关)
static volatile int s_sess_fd = -1;          //当前会话socket(同上; 撬醒卡在会话内的调用)
static volatile int s_sess_kicked;           //会话fd已被看门狗外部关闭(会话收尾不再close)

//收包线程(xadapter.c定义, socket模式下由应用负责拉起)
extern void xemacif_input_thread(struct netif *netif);

// -----------------------------------------------------------------------------
// 小工具: 网络字节序读写(手工拼包/解包, 不做对齐假设)
static void put_u16(uint8_t* p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static unsigned get_u16(const uint8_t* p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

//路径取文件名(进度显示用)
static const char* path_basename(const char* path)
{
    const char* p = strrchr(path, '/');
    return p ? p + 1 : path;
}

//设非阻塞: recv无事件时立即返回, 由调用方vTaskDelay让出CPU
static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, O_NONBLOCK);
}

//收包模式: 能设SO_RCVTIMEO就走阻塞收(快路径, 数据一到立刻返回),
//否则退回非阻塞轮询。ms=本次socket的阻塞上限
static int s_rx_tmo_ok;
static void setup_rx_mode(int fd, unsigned ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000u;
    tv.tv_usec = (ms % 1000u) * 1000u;
    s_rx_tmo_ok = (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                              &tv, sizeof(tv)) == 0);
    if (!s_rx_tmo_ok)
        set_nonblock(fd);
}

//一次recv返回<0后的空转等待: 阻塞路径的超时已在recv里耗时, 轮询路径睡一拍
static void rx_wait(unsigned* idle)
{
    if (s_rx_tmo_ok)
    {
        *idle += NET_T_POLL_MS;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(NET_DATA_MS));
    *idle += NET_DATA_MS;
}

// -----------------------------------------------------------------------------
// TFTP包发送(ACK/ERROR, 均<=4+30字节): 一律MSG_DONTWAIT, 发不出去(对端不收/
// 发送资源暂缺)直接丢——TFTP客户端等不到应答会重发, 我方重复应答即自愈;
// 服务循环绝不挂在发送上(本BSP无LWIP_SO_SNDTIMEO, 阻塞send可挂分钟级)。
static void send_pkt(int fd, const uint8_t* pkt, int len,
                     const struct sockaddr_in* to)
{
    sendto(fd, pkt, (size_t)len, MSG_DONTWAIT,
           (const struct sockaddr*)to, sizeof(*to));
}

static void send_ack(int fd, unsigned blk, const struct sockaddr_in* to)
{
    uint8_t pkt[TFTP_HDR];
    put_u16(pkt, TFTP_ACK);
    put_u16(pkt + 2, blk);
    send_pkt(fd, pkt, sizeof(pkt), to);
}

//错误包(码+文本): 会话内的拒绝从会话socket发; 主socket上的请求级拒绝
//经send_err_ephemeral走临时端口(标准TFTP: 服务应答来自非69的新TID)
static void send_err(int fd, unsigned code, const char* msg,
                     const struct sockaddr_in* to)
{
    uint8_t pkt[TFTP_HDR + 40];
    put_u16(pkt, TFTP_ERROR);
    put_u16(pkt + 2, code);
    unsigned n = (unsigned)strlen(msg);
    if (n > sizeof(pkt) - TFTP_HDR - 1)
        n = sizeof(pkt) - TFTP_HDR - 1;
    memcpy(pkt + TFTP_HDR, msg, n);
    pkt[TFTP_HDR + n] = 0;
    send_pkt(fd, pkt, TFTP_HDR + (int)n + 1, to);
}

//从临时端口(标准TID)发一条错误包给对端(主socket收到RRQ/坏包时的应答通道)
static void send_err_ephemeral(const struct sockaddr_in* to,
                               unsigned code, const char* msg)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_in any;
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_port = 0;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&any, sizeof(any)) == 0)
        send_err(fd, code, msg, to);
    closesocket(fd);
}

// -----------------------------------------------------------------------------
// WRQ文件名 -> SD全路径。映射规则: "0:/..."原样; 其余挂"0:/"根下(标准
// tftp客户端只能给裸名/相对路径)。拒绝".."与非ASCII(FatFs长名风险,
// 与PC工具net_push的客户端侧检查同源)。
//返回0成功; 非0=错误文本(进TFTP ERROR包, ASCII)
static const char* map_path(const char* fname, char* out, unsigned cap)
{
    if (fname[0] == 0)
        return "empty name";
    for (const char* p = fname; *p; p++)
        if ((unsigned char)*p >= 0x80)
            return "non-ascii name";
    if (strstr(fname, "..") != 0)
        return "no .. allowed";
    if (strncmp(fname, "0:/", 3) == 0)
    {
        if (strlen(fname) >= cap)
            return "name too long";
        strcpy(out, fname);
    }
    else
    {
        const char* src = (fname[0] == '/') ? fname + 1 : fname;
        if (strlen(src) + 3 >= cap)
            return "name too long";
        snprintf(out, cap, "0:/%s", src);
    }
    return 0;
}

//WRQ载荷解析: [2..]为文件名\0模式\0(选项不识别——不协商, 客户端退回裸TFTP)
//返回0成功; 非0=错误文本
static const char* parse_wrq(const uint8_t* req, int len,
                             char* fname, unsigned cap)
{
    const uint8_t* end = req + len;
    const uint8_t* p = req + 2;

    const uint8_t* fn = p;                    //文件名z串
    while (p < end && *p) p++;
    if (p >= end)
        return "bad packet";
    const unsigned flen = (unsigned)(p - fn);
    if (flen == 0 || flen >= cap)
        return "bad name";
    memcpy(fname, fn, flen);
    fname[flen] = 0;

    p++;                                      //跳过文件名的\0 -> 模式串
    if (p >= end)
        return "no mode";
    const char ascii_lower_diff = 'a' - 'A';
    const char want[] = "octet";
    for (unsigned i = 0; i < sizeof(want); i++)   //含结尾\0: 模式串须恰为octet
    {
        char c = (p + i < end) ? *(const char*)(p + i) : 0;
        if (c >= 'A' && c <= 'Z')
            c += ascii_lower_diff;
        if (c != want[i])
            return "octet only";
    }
    return 0;
}

//接收进度发布(节流: 每64KB一次, 桥快照拷贝不小, 逐包发太密);
//msg捎带实测速率, 传输页直接显示。TFTP未知文件总量(WRQ不带尺寸):
//total恒0, UI按"已收字节+速率"显示(见app_transfer.c refresh_live)
static char     s_rx_name[28];
static uint32_t s_rx_t0;             //数据流首包时刻tick(速率基准)
static uint32_t s_rx_last;           //进度节流基准(每会话清零)
static void pub_rx_progress(uint32_t done)
{
    if (done - s_rx_last < 64u * 1024u && done != 0)
        return;
    char spd[16];
    const uint32_t el = (uint32_t)xTaskGetTickCount() - s_rx_t0;
    const uint32_t kbps = (el > 0)
        ? (uint32_t)((uint64_t)done * configTICK_RATE_HZ / el / 1024u) : 0;
    if (kbps >= 1024u)
        snprintf(spd, sizeof(spd), "%u.%u MB/s",
                 (unsigned)(kbps / 1024u),
                 (unsigned)((kbps % 1024u) * 10u / 1024u));
    else
        snprintf(spd, sizeof(spd), "%u KB/s", (unsigned)kbps);
    s_rx_last = done;
    gui_model_pub_xfer(XFER_RX, s_rx_name, done, 0, spd);
}

// -----------------------------------------------------------------------------
// WRQ会话: 解析请求 -> 映射SD路径 -> 会话socket(临时端口) -> ACK(0)
// -> 512字节锁步收流(动态倍增缓冲) -> 尾块即ACK -> 交app_task落SD
// -> 回执对账 -> 发布结果(失败补发ERROR, 尽力而为)。
static void tftp_wrq_session(const struct sockaddr_in* cli,
                             const uint8_t* req, int len)
{
    char fname[TFTP_PATH_MAX];
    const char* why = parse_wrq(req, len, fname, sizeof(fname));

    char path[48];
    if (why == 0)
        why = map_path(fname, path, sizeof(path));

    //会话socket: bind临时端口(标准TID; 后续DATA都到这个口)
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        xil_printf("[net] session socket failed\r\n");
        gui_model_pub_xfer(XFER_ERR, "", 0, 0, "监听失败");
        return;
    }
    struct sockaddr_in any;
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_port = 0;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&any, sizeof(any)) < 0)
    {
        xil_printf("[net] session bind failed\r\n");
        closesocket(fd);
        return;
    }
    s_sess_fd = fd;
    s_sess_kicked = 0;

    if (why != 0)
    {
        send_err(fd, TFTP_E_ACCESS, why, cli);   //路径/模式拒绝: 码2
        xil_printf("[net] reject '%s': %s\r\n", fname, why);
        goto out;
    }

    xil_printf("[net] wrq %s <- %s:%d\r\n", path,
               inet_ntoa(cli->sin_addr), ntohs(cli->sin_port));
    gui_model_pub_xfer(XFER_CONN, inet_ntoa(cli->sin_addr), 0, 0, "");
    gui_model_pub_xfer(XFER_RX, path_basename(path), 0, 0, "");
    strncpy(s_rx_name, path_basename(path), sizeof(s_rx_name) - 1);
    s_rx_name[sizeof(s_rx_name) - 1] = 0;
    s_rx_t0 = (uint32_t)xTaskGetTickCount();
    s_rx_last = 0;

    send_ack(fd, 0, cli);                        //ACK(0): 客户端见此开流

    //---- 收流: 锁步DATA/ACK ----
    setup_rx_mode(fd, NET_T_POLL_MS);            //阻塞+超时优先, 回退非阻塞
    uint8_t* buf = 0;                            //整文件缓冲(net任务malloc)
    uint32_t cap = 0, total = 0;
    unsigned expect = 1;                         //期望块号(u16语义, 回绕正确)
    unsigned idle_lim = NET_T_FIRST_MS;          //首块前用短时限(死客户端快收敛)
    unsigned idle = 0;
    for (;;)
    {
        s_listen_tick = xTaskGetTickCount();
        if (s_stop)
            goto abort_stop;

        uint8_t pkt[TFTP_PKT_MAX];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        const int n = recvfrom(fd, pkt, sizeof(pkt), 0,
                               (struct sockaddr*)&from, &flen);
        if (n < 0)
        {
            rx_wait(&idle);
            if (idle >= idle_lim)
                goto abort_timeout;
            continue;
        }
        if (n < TFTP_HDR)
            continue;                            //碎包: 丢弃
        //TID校验: 会话包源须与发起方一致(其余会话/杂流的包不得搅局)
        if (from.sin_addr.s_addr != cli->sin_addr.s_addr
            || from.sin_port != cli->sin_port)
            continue;

        const unsigned op = get_u16(pkt);
        if (op == TFTP_DATA)
        {
            const unsigned blk = get_u16(pkt + 2);
            const int dlen = n - TFTP_HDR;
            if (blk == expect)
            {
                if (dlen > TFTP_BLK || dlen < 0)
                {
                    send_err(fd, TFTP_E_ILLEGAL, "bad block", cli);
                    goto abort_free;
                }
                if (total + (uint32_t)dlen > NET_MAX_SIZE)
                {
                    send_err(fd, TFTP_E_FULL, "too large", cli);
                    xil_printf("[net] rx %s over size cap\r\n", path);
                    goto abort_free;
                }
                if (total + (uint32_t)dlen > cap)   //倍增扩容(首块起64KB)
                {
                    uint32_t nc = cap ? cap : (64u * 1024u);
                    while (nc < total + (uint32_t)dlen && nc < NET_MAX_SIZE)
                        nc *= 2u;
                    uint8_t* nb = (uint8_t*)realloc(buf, nc);
                    if (nb == 0)
                    {
                        send_err(fd, TFTP_E_FULL, "no mem", cli);
                        gui_model_pub_xfer(XFER_ERR, path_basename(path),
                                           0, 0, "内存不足");
                        goto abort_free;
                    }
                    buf = nb;
                    cap = nc;
                }
                memcpy(buf + total, pkt + TFTP_HDR, (size_t)dlen);
                total += (uint32_t)dlen;
                idle = 0;
                idle_lim = NET_T_IDLE_MS;        //进入数据流: 恢复长时限
                send_ack(fd, blk, cli);
                pub_rx_progress(total);
                expect = (expect + 1) & 0xFFFFu; //u16回绕(>32MB文件)
                if (dlen < TFTP_BLK)             //尾块: 传输段完成, 转异步落盘
                    break;
            }
            else if (blk == ((expect - 1) & 0xFFFFu))
            {
                send_ack(fd, blk, cli);          //重复块=己方ACK丢失: 重ACK
            }
            //其余乱序块: 丢弃(客户端等不到ACK会重发)
        }
        else if (op == TFTP_ERROR)
        {
            xil_printf("[net] client abort %s\r\n", path);
            gui_model_pub_xfer(XFER_ERR, path_basename(path), 0, 0, "PC中止");
            goto abort_free;
        }
        //其余(对会话口的ACK/杂包): 丢弃
    }

    //尾块已ACK(客户端视角传输已完)。缓冲移交app_task落SD(FatFs纪律):
    //缓冲随命令过户, 对方写完free并自发布UI终态; 落SD失败由app_task反向
    //调net_server_write_fail补发ERROR(尽力而为)。net任务绝不在此等盘——
    //等回执=69端口整体停摆(app_task迟滞时WRQ全部无应答, 压测真机踩过)
    gui_model_pub_xfer(XFER_RX, path_basename(path), total, total, "写SD…");
    xil_printf("[net] rx %s done (%u bytes), commit\r\n", path, total);
    {
        uint8_t* hand = buf ? buf : (uint8_t*)malloc(1);  //空文件也给非NULL
        if (hand == 0)
        {
            gui_model_pub_xfer(XFER_ERR, path_basename(path), 0, 0, "内存不足");
            goto abort_free;
        }
        buf = 0;                                 //所有权已过户, 本函数不再free

        AppCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = APP_CMD_NET_FILE;
        cmd.val = (s32)total;
        cmd.ptr = hand;
        strncpy(cmd.path, path, sizeof(cmd.path) - 1);
        cmd.xfer_ip = ntohl(cli->sin_addr.s_addr);   //会话对端随命令过户:
        cmd.xfer_port = ntohs(cli->sin_port);        //落SD失败补发ERROR用
        if (appctl_post(&cmd) != 0)
        {
            free(hand);
            send_err(fd, TFTP_E_FULL, "busy", cli);
            gui_model_pub_xfer(XFER_ERR, path_basename(path), 0, 0, "系统忙");
            xil_printf("[net] commit queue full %s\r\n", path);
        }
        //已受理: 不发LISTEN(UI保持"写SD…"), 终态由app_task发布
    }
    goto out;

abort_stop:
    send_err(fd, TFTP_E_NOTDEF, "aborted", cli);
    gui_model_pub_xfer(XFER_ERR, path_basename(path), total, 0, "PC中止");
    free(buf);
    goto out;
abort_timeout:
    send_err(fd, TFTP_E_NOTDEF, "timeout", cli);
    gui_model_pub_xfer(XFER_ERR, path_basename(path), total, 0, "链路超时");
    xil_printf("[net] rx %s idle timeout (%u bytes)\r\n", path, total);
    /* fall through */
abort_free:
    free(buf);
out:
    if (!s_sess_kicked)                 //被看门狗外部关闭过就不再close(fd可能已复用)
        closesocket(fd);
    s_sess_kicked = 0;
    s_sess_fd = -1;
}

// -----------------------------------------------------------------------------
// 监听循环(持有期间串行处理WRQ), 退出条件=s_stop(应用退出)。
// 主socket轮询模式: SO_RCVTIMEO(100ms)优先, 回退非阻塞+20ms让出——
// 每拍都刷新活动时刻, 冻结只可能是卡死(看门狗直接踢, 无需探针)。
static void net_listen_loop(void)
{
    const int ls = socket(AF_INET, SOCK_DGRAM, 0);
    if (ls < 0)
    {
        xil_printf("[net] socket() failed\r\n");
        gui_model_pub_xfer(XFER_ERR, "", 0, 0, "监听失败");
        return;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        xil_printf("[net] bind udp %d failed\r\n", NET_PORT);
        closesocket(ls);
        gui_model_pub_xfer(XFER_ERR, "", 0, 0, "监听失败");
        return;
    }
    s_ls_fd = ls;
    s_ls_kicked = 0;
    s_listen_tick = xTaskGetTickCount();
    s_listening = 1;
    setup_rx_mode(ls, NET_POLL_MS);            //100ms一级的等待粒度
    xil_printf("[net] listening on udp %d (tftp)\r\n", NET_PORT);
    gui_model_pub_xfer(XFER_LISTEN, "", 0, 0, "");

    int hb = 0;                                //收包错误心跳(限10条, 诊断用)
    while (!s_stop)
    {
        uint8_t pkt[TFTP_PKT_MAX];
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        const int n = recvfrom(ls, pkt, sizeof(pkt), 0,
                               (struct sockaddr*)&cli, &clen);
        s_listen_tick = xTaskGetTickCount();   //返回即活动(每拍刷新)
        if (n < 0)
        {
            if (!s_rx_tmo_ok)
            {
                if (++hb <= 10)
                    xil_printf("[net] recv err e=%d\r\n", errno);
                vTaskDelay(pdMS_TO_TICKS(NET_DATA_MS));
            }
            continue;                          //空转: 让出CPU(s_stop每拍生效)
        }
        hb = 0;
        if (n < TFTP_HDR)
            continue;                          //碎包: 丢弃

        const unsigned op = get_u16(pkt);
        if (op == TFTP_WRQ)
        {
            tftp_wrq_session(&cli, pkt, n);
            s_listen_tick = xTaskGetTickCount();          //会话结束也是活动证据
            gui_model_pub_xfer(XFER_LISTEN, "", 0, 0, "");  //回"等待连接"
            setup_rx_mode(ls, NET_POLL_MS);    //会话改过全局收包模式: 恢复主口参数
            continue;
        }
        if (op == TFTP_RRQ)
        {
            //读请求不支持(net任务不读SD, FatFs单线程纪律)
            send_err_ephemeral(&cli, TFTP_E_ACCESS, "upload only");
            xil_printf("[net] rrq rejected (upload only)\r\n");
            continue;
        }
        //其余(DATA/ACK/ERROR到69口): 旧会话残包或探测包, 丢弃
    }

    s_listening = 0;
    if (s_ls_fd == ls && !s_ls_kicked)         //被看门狗外部关闭过就不重复close
        closesocket(ls);
    s_ls_fd = -1;
    gui_model_pub_xfer(XFER_IDLE, "", 0, 0, "");
    xil_printf("[net] listener stopped\r\n");
}

// -----------------------------------------------------------------------------
// nstack任务: 栈初始化(一次) -> 监听请求循环(信号量驱动)
static void net_task(void* arg)
{
    (void)arg;

    s_boot_tick = xTaskGetTickCount();   //初始化看门狗基准(net_server_watchdog)
    lwip_init();
    tcpip_init(NULL, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));            //等tcpip线程就绪

    ip_addr_t ip, nm, gw;
    IP4_ADDR(&ip,  NET_IP0, NET_IP1, NET_IP2, NET_IP3);
    IP4_ADDR(&nm,  255, 255, 255, 0);
    IP4_ADDR(&gw,  NET_IP0, NET_IP1, NET_IP2, 1);

    //MAC地址(多板同网时手动改末字节)
    unsigned char mac[6] = { 0x00, 0x0a, 0x35, 0x18, 0x00, 0x01 };

    if (xemac_add(&s_netif, &ip, &nm, &gw, mac,
                  XPAR_XEMACPS_0_BASEADDR) == 0)
    {
        xil_printf("[net] xemac_add failed (GEM0 @0x%x)\r\n",
                   (unsigned)XPAR_XEMACPS_0_BASEADDR);
        s_stack_dead = 1;               //判死: 此后打开应用直接报"网络启动失败"
        gui_model_pub_xfer(XFER_ERR, "", 0, 0, "网络启动失败");
        gui_model_pub_net(0, "");
        vTaskDelete(0);
    }
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);

    //收包线程(socket模式由应用负责; 中断收包经信号量唤醒喂给lwIP)
    sys_thread_new("xemac_in", (void(*)(void*))xemacif_input_thread,
                   &s_netif, IN_STACK, DEFAULT_THREAD_PRIO);

    s_stack_ok = 1;
    char iptext[16];
    snprintf(iptext, sizeof(iptext), "%u.%u.%u.%u",
             NET_IP0, NET_IP1, NET_IP2, NET_IP3);
    gui_model_pub_net(1, iptext);
    xil_printf("[net] up: %s (tftp server on udp %d)\r\n",
               iptext, NET_PORT);
    //版本指纹: 与model.mod头部fw_version比对, 一眼确认固件与模块是否配对
    xil_printf("[net] build 0x%08x\r\n", FW_EXPORTS_VERSION);

    //应用先于栈就绪的场合: net_server_start已无条件给票缓冲在此, 直接消费。
    //s_want_listen意图位过滤陈旧票据——stop之后到达的票不再复活监听
    for (;;)
    {
        xSemaphoreTake(s_req_sem, portMAX_DELAY);
        if (!s_want_listen)
            continue;
        s_stop = 0;
        if (!s_want_listen)               //stop与本行穿插的极窗: 再核一次
            continue;
        net_listen_loop();
    }
}

// -----------------------------------------------------------------------------
static void net_wdt_task(void* arg);        //前置: net_stack_boot先于定义使用

int net_stack_boot(void)
{
    s_req_sem = xSemaphoreCreateBinary();
    if (s_req_sem == 0)
    {
        s_stack_dead = 1;               //任务没起来: 打开应用直接报失败
        return -1;
    }
    //lwIP线程API: 栈深单位=字(本端口直接透传给xTaskCreate)
    if (sys_thread_new("nstack", net_task, 0, NET_STACK, NET_PRIO) == 0)
    {
        s_stack_dead = 1;
        return -1;
    }
    //看门狗独立任务: 不随app_task忙而停摆
    if (sys_thread_new("netwdt", net_wdt_task, 0, WDT_STACK, NET_PRIO) == 0)
        xil_printf("[net] watchdog task create failed (self-heal off)\r\n");
    return 0;
}

int net_server_start(void)
{
    if (s_stack_dead)
        return -1;                      //栈判死: 上层直接示错"网络启动失败"
    s_want_listen = 1;                  //先立意图: 陈旧票据过滤/看门狗都依据它
    s_want_tick = xTaskGetTickCount();
    if (!s_stack_ok)
        //栈初始化中(开机后PHY自协商可达20s): 界面先显示WAIT, 就绪自动转LISTEN
        gui_model_pub_xfer(XFER_WAIT, "", 0, 0, "");
    //无条件给票: 二元信号量会缓冲, net_task就绪后立即消费开监听(单核竞态
    //下"检查pending再决定给票"会丢单, 真机踩过)
    xSemaphoreGive(s_req_sem);
    return 0;
}

void net_server_stop(void)
{
    s_want_listen = 0;                  //先清意图: net_task丢弃陈旧票据(桌面不留监听)
    s_stop = 1;                         //监听/会话在<=1个超时周期内退出
    xil_printf("[net] stop requested\r\n");
}

//健康看门狗(独立wdt任务1s节拍, 不依赖app_task——app_task可能正忙于任何
//长命令, 看门狗跟着停摆就没人救net侧了)
void net_server_watchdog(void)
{
    if (s_stack_ok)
    {
        if (!s_want_listen)
            return;
        if (!s_listening)
        {
            //该监听却缺席: 正常启停<0.5s, 持续>3s即异常(socket/bind失败后
            //循环已退, 弹票重试直到成功——端口被占等瞬时状况可自愈)
            if ((uint32_t)(xTaskGetTickCount() - s_want_tick)
                > pdMS_TO_TICKS(NET_T_WANT_MS))
            {
                xil_printf("[net] wdt: listener missing, bounce\r\n");
                s_stop = 1;
                xSemaphoreGive(s_req_sem);
            }
            return;
        }
        if ((uint32_t)(xTaskGetTickCount() - s_listen_tick)
            > pdMS_TO_TICKS(NET_T_WDT_MS))
        {
            //活动时刻冻结=卡在不返回的系统调用里: 主socket轮询模式每拍都
            //刷新活动时刻, 冻结只可能是recvfrom/sendto某一次永久不返回
            //(与accept时代不同, 无需自探针区分健康阻塞)。closesocket把阻塞
            //中的调用以错误撬醒: 监听fd救主循环, 会话fd救会话; 监听循环退出
            //后取票重建。先立kicked再关(防退出路径二次close复用后的fd误伤
            //别的socket); 若最终未能撬醒, 串口已留证(须重启)
            xil_printf("[net] wdt: net wedged (sess_fd=%d), kick\r\n",
                       (int)s_sess_fd);
            s_stop = 1;
            xSemaphoreGive(s_req_sem);
            if (s_ls_fd >= 0)
            {
                s_ls_kicked = 1;
                closesocket(s_ls_fd);
            }
            if (s_sess_fd >= 0)
            {
                s_sess_kicked = 1;
                closesocket(s_sess_fd);
            }
        }
    }
    else if (!s_stack_dead &&
             (uint32_t)(xTaskGetTickCount() - s_boot_tick)
             > pdMS_TO_TICKS(NET_T_INIT_MS))
    {
        //45s未就绪: 合法自协商≤约25s, 超出即卡死(BSP的PHY软复位等待无超时,
        //MDIO异常时net_task悬在xemac_add内)——判死, 界面从"网络初始化…"
        //转明确失败, 不再无限等待
        s_stack_dead = 1;
        xil_printf("[net] wdt: stack init stuck %us, give up\r\n",
                   (unsigned)NET_T_INIT_MS);
        gui_model_pub_xfer(XFER_ERR, "", 0, 0, "网络初始化超时");
        gui_model_pub_net(0, "");
    }
}

//看门狗任务: 1s节拍驱动net_server_watchdog(独立于app_task/gui)
static void net_wdt_task(void* arg)
{
    (void)arg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        net_server_watchdog();
    }
}

// -----------------------------------------------------------------------------
// app_task落SD失败时的反向通知: 朝会话对端补发一条TFTP ERROR(码3, 尽力而为
// ——客户端收到尾ACK后可能已收摊, 报文落空无害; 自家net_push.py收尾后按
// 体量多听片刻可捕获)。ip/port为主机序(AppCmd随命令过户的会话对端)。
void net_server_write_fail(u32 ip_host, u16 port_host)
{
    struct sockaddr_in any, to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(ip_host);
    to.sin_port = htons(port_host);

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_port = 0;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&any, sizeof(any)) == 0)
        send_err(fd, TFTP_E_FULL, "sd write failed", &to);
    closesocket(fd);
    xil_printf("[net] write fail, ERROR -> %s:%u\r\n",
               inet_ntoa(to.sin_addr), (unsigned)port_host);
}
