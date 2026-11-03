// =============================================================================
// appctl —— 应用命令总线(LCD触摸GUI -> app_task)
//
// 串口控制已退役(仅调试打印), LCD界面是唯一控制入口: gui任务的按钮回调
// 把命令事件投进本队列, app_task消费执行; 队列满则丢弃(重复触摸语义合并,
// 沿用原串口按键的合并语义)。命令语义由应用层解释, 本模块只做搬运
// =============================================================================
#pragma once

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "FreeRTOS.h"
#include "queue.h"

//命令事件(gui任务/net任务生产, app_task消费)
//带路径的命令直接内嵌路径: 队列项变大但免除共享暂存区的竞态
typedef struct {
    u8  type;    //命令类型 APP_CMD_xxx
    s32 val;     //APP_CMD_MODEL_SELECT: 模型索引, APP_CMD_MODEL_NEXT=下一个
                 //APP_CMD_HDMI_SET: 0/1; APP_CMD_NET_FILE: 文件字节数
    char path[48]; //APP_CMD_FS_LIST/FS_DELETE: 目标目录/路径; APP_CMD_FS_COPY:
                   //源全路径; APP_CMD_NET_FILE: 目标全路径
    char path2[48]; //APP_CMD_FS_COPY: 目标全路径(重名由sd层自动加序号)
    void* ptr;   //APP_CMD_NET_FILE: 数据缓冲区(net任务malloc, app_task写SD后free)
    u32  xfer_ip;   //APP_CMD_NET_FILE: 会话对端IP(主机序, 落SD失败补发ERROR用)
    u16  xfer_port; //APP_CMD_NET_FILE: 会话对端端口(主机序, 同上)
    u32  seq;    //保留(原NET_FILE会话序号, 落SD改异步后不再使用)
} AppCmd;

enum {
    APP_CMD_RUN_TOGGLE = 1,   //开始/停止推理
    APP_CMD_CAPTURE,          //拍照(相机应用): 抓帧存BMP到 /photos
    APP_CMD_MODEL_SELECT,     //切换模型
    APP_CMD_CAM_OPEN,         //启动摄像头链路(可选子系统, 相机应用进入时)
    APP_CMD_CAM_CLOSE,        //停止摄像头链路(相机应用退出时)
    APP_CMD_HDMI_SET,         //HDMI输出开关(val: 0关 1开, 链路运行期间)
    APP_CMD_FS_LIST,          //列举SD目录(path), 结果经bridge快照回传
    APP_CMD_FS_DELETE,        //删除文件/空目录(path), 完成后GUI自行重列
    APP_CMD_FS_COPY,          //复制文件(path=源, path2=目标全路径), 完成后GUI自行重列
    APP_CMD_APP_RESCAN,       //扫描/apps下的SD应用(结果经bridge回传)
    APP_CMD_APP_LAUNCH,       //启动SD应用(val=bridge.sdapp下标)
    APP_CMD_PHOTO_LOAD,       //读BMP照片进查看缓冲(path=全路径, 经bridge回传)
    APP_CMD_PHOTO_UNLOAD,     //释放查看缓冲(退出查看器/退出相机应用时)
    APP_CMD_RUN_SET,          //推理运行开关(val: 0停 1跑; 推理应用)
    APP_CMD_PHOTO_INFER,      //对当前查看图片单次推理(推理应用)
    APP_CMD_MODELS_RESCAN,    //重扫SD /models(推理应用, 换卡后免重启)
    APP_CMD_NET_START,        //文件传输应用进入: 开始TFTP(UDP 69)监听
    APP_CMD_NET_STOP,         //文件传输应用退出: 停监听并中止进行中的会话
    APP_CMD_NET_FILE,         //网口收满一个文件(net任务->app_task落SD, 携ptr)
};

#define APP_CMD_MODEL_NEXT  (-2)

//创建命令队列(main里、任务启动前调用一次)
//返回0成功; -1队列创建失败
int appctl_init(void);

//投递命令(gui任务上下文, 不阻塞; 队列满丢弃)  返回0=已入队
int appctl_post(const AppCmd* cmd);

//取命令(app_task; wait=0非阻塞)  返回1=取到
int appctl_take(AppCmd* cmd, TickType_t wait);

//塞回队首(叠加期被打断的场景, 让命令下一轮开头优先消费)
void appctl_post_front(const AppCmd* cmd);

#ifdef __cplusplus
}
#endif
