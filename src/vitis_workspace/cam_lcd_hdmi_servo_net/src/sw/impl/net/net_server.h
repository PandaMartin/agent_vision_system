// =============================================================================
// net_server —— 网口文件接收服务(标准TFTP服务端, lwIP220 SOCKET_API, UDP 69)
//
// 分两层:
//   栈层   net_stack_boot(): main末尾拉起"nstack"任务, 做lwip_init+tcpip_init
//          +xemac_add(静态IP)+收包线程, 完成后桥发布net_up/IP(状态栏图标)。
//          栈常驻(收包线程/定时器要活), 与是否传输无关, ping随时可用。
//   服务层 net_server_start/stop(): 由"文件传输"内置应用门控——进入应用
//          开始监听UDP 69(TFTP), 退出应用停监听并中止进行中的会话。
//          桌面态端口不监听, 传输只在应用内发生。
//   看门狗 net_server_watchdog(): 由net_stack_boot拉起的独立"netwdt"任务按
//          1s节拍驱动(不依赖app_task)——含栈初始化45s判死、"该监听却缺席"
//          弹票重试、活动时刻冻结>10s时closesocket撬醒卡死的调用并取票重建
//          (UDP主socket为轮询模式, 每拍刷新活动时刻, 冻结即真卡死, 无需探针)。
//
// 会话协议(标准TFTP/RFC1350, PC端任意tftp客户端或tools/netpush/net_push.py):
//   PC -> WRQ <文件名> octet (UDP 69) -> 板开会话socket(临时端口)回ACK(0)
//   -> PC连发512字节DATA(锁步ACK, 块号u16回绕) -> 尾块(数据<512B)板即回ACK
//   -> 板整文件缓冲经appctl(APP_CMD_NET_FILE)异步交app_task落SD(FatFs单
//   线程纪律, 本任务不碰文件系统) -> net任务立即回监听(服务永不等盘,
//   落盘再慢只排队); app_task写完自发布UI终态, 失败时反向调
//   net_server_write_fail向会话对端补发TFTP ERROR(尽力而为——标准客户端
//   收到尾ACK即报成功, 最终结果以板面为准)。
//   只支持WRQ(推送); RRQ回错误码2(net任务读SD违反FatFs纪律); 只认octet。
//   文件名映射: "0:/..."原样; 裸名/相对路径挂"0:/"根下; 含".."或非ASCII拒绝。
//
// 数据缓冲所有权: net任务malloc(动态倍增, 上限64MB) -> 尾块后随命令移交
// app_task(写SD+free); 会话对端地址随命令过户(失败补发ERROR用);
// 落SD后的应用/模型目录自动重扫在app侧(int_app/app_transfer.c)。
// =============================================================================

#ifndef NET_SERVER_H
#define NET_SERVER_H

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

//拉起网络栈任务(main里、vTaskStartScheduler之前调用一次)
//返回0=任务已创建(栈就绪后自动发布net_up; 同时拉起netwdt看门狗任务);
//-1=任务创建失败
int net_stack_boot(void);

//开始监听UDP 69/TFTP(文件传输应用进入时, app_task上下文)
//栈就绪=取票立即开监听; 栈未就绪(开机PHY自协商窗口)先回XFER_WAIT,
//net_task就绪后取票自动开监听。返回0=已受理; -1=栈已判死(初始化卡死/
//任务没起来), 上层应直接示错
int net_server_start(void);

//停止监听并中止进行中的会话(文件传输应用退出时; 会话在<=1个超时周期内
//结束, 给PC留出收到TFTP ERROR的窗口)。监听意图一并清除: 陈旧票据被
//net_task丢弃, 桌面态不会复活监听
void net_server_stop(void);

//健康看门狗单拍(由netwdt任务1s节拍调用; 保留导出便于复用)
//监听/会话卡死自愈(活动冻结判定+closesocket撬锁) + 栈初始化判死(45s)
void net_server_watchdog(void);

//app_task落SD失败的补发ERROR(会话对端已收到尾ACK, 尽力而为):
//ip/port为主机序, 来自AppCmd随命令过户的会话对端地址
void net_server_write_fail(u32 ip_host, u16 port_host);

#ifdef __cplusplus
}
#endif

#endif /* NET_SERVER_H */
