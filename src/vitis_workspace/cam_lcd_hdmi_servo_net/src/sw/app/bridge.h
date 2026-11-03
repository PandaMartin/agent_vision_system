// =============================================================================
// gui_model —— 业务数据桥(app_task发布 -> gui任务拉取)
//
// 全工程唯一的跨任务共享可变状态: app_task在业务节点"顺手广播"快照,
// gui任务定时整体拷贝读取。内部mutex保护, 本模块纯数据、不含任何LVGL代码。
// 开机后不可变的数据(模型列表/屏型/版本)不进桥, 由使用方直接只读。
//
// 分组generation计数: gui页面只关心自己那几组, 比对代数决定是否解析重绘,
// 避免无效invalidate。摄像头缩略图走独立双缓冲(app写后台, gui读前台)。
// =============================================================================
#pragma once

#include "xil_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "FreeRTOS.h"

//结果条目上限
#define GUI_TOPK_MAX    5      //分类top-k条数
#define GUI_DET_CLS_MAX 8      //检测类别摘要条数
#define GUI_LBL_MAX     24     //字符串字段容量(含\0)
#define GUI_HIST_N      64     //推理耗时历史环深
#define GUI_FS_MAX      64     //目录列举单屏条数上限
#define GUI_FS_PATH_MAX 48     //当前路径容量(与AppCmd.path一致)
#define GUI_FS_NAME_MAX 28     //条目名容量(与SdDirEntry.name一致)
#define GUI_SDAPP_MAX   8      //SD应用图标上限
#define GUI_MODEL_MAX   8      //SD模型清单上限(=NN_MAX_MODELS)
#define GUI_DET_BOX_MAX 16     //检测框叠加条数上限(GUI与vidwin共用)

//SD应用加载状态机(gen_app随每次迁移递增)
enum {
    APPST_IDLE = 0,    //桌面态
    APPST_LOADING,     //app_task读取/校验中(图标按下后)
    APPST_READY,       //已拷入应用区+cache同步, gui侧可调entry
    APPST_RUNNING,     //应用运行中
    APPST_EXIT_REQ,    //应用请求退出(gui侧清理中)
    APPST_FAILED,      //加载失败(错误文本见app_err)
};

//文件传输状态机(传输应用, gen_xfer驱动)
enum {
    XFER_IDLE = 0,     //未监听(应用未开)
    XFER_LISTEN,       //监听中, 等PC连接
    XFER_WAIT,         //应用已请求监听, 但网络栈初始化中(PHY自协商可达20s)
    XFER_CONN,         //PC已连接, 会话协商中(等协议行/分配缓冲)
    XFER_RX,           //接收文件中(进度看xfer_done/total, 速率在xfer_msg)
    XFER_DONE,         //最近一个文件已落SD(结果进xfer_res_*)
    XFER_ERR,          //出错(文本见xfer_msg, 结果进xfer_res_*)
};

//分类top-k条目(app侧从模型档案labels拷入)
typedef struct {
    char name[GUI_LBL_MAX];
    int  score;                //0..100
} GuiTopKItem;

//检测类别摘要(每类计数+最高分)
typedef struct {
    char name[GUI_LBL_MAX];
    int  count;
    int  best;                 //0..100
} GuiDetCls;

//目录列举条目(app_task从sd层拷入)
typedef struct {
    char    name[GUI_FS_NAME_MAX];
    uint8_t is_dir;
    uint32_t size;             //字节(目录项无意义)
} GuiFsEntry;

//检测框叠加条目(推理应用: app_task发布, gui结果面板与vidwin叠加共用;
//坐标为"源帧"像素系——摄像头1280x720或当前查看图片)
typedef struct {
    char    name[12];          //类别名(ASCII, 截断)
    uint8_t cls;               //类别号(配色索引)
    uint8_t score;             //置信度0~100
    uint16_t x0, y0, x1, y1;
} GuiDetBox;

//gui任务读到的整体快照(一次mutex内拷贝)
typedef struct {
    //---- 开机信息 ----
    int      sd_ok;
    int      n_models;
    int      scr_id;            //LCD屏ID(装配点探测后随boot发布)
    int      scr_w, scr_h;      //LCD分辨率
    int      touch_id;          //触摸芯片ID(未识别为0)
    //---- 运行状态 ----
    int      cam_on;            //可选子系统(摄像头链路)运行中
    int      cam_hdmi;          //HDMI输出开启(链路运行期间独立开关)
    int      running;
    unsigned cycle;
    //---- 推理指标 ----
    unsigned infer_ms;         //最近一次
    unsigned avg_ms;           //环内平均
    unsigned peak_ms;          //环内峰值
    u16      hist[GUI_HIST_N]; //耗时历史(旧->新排列, hist_n为有效长度)
    int      hist_n;
    //---- 识别结果 ----
    int      post_type;        //0=分类 1=检测(最近一次有效的类型)
    int      topk_n;
    GuiTopKItem topk[GUI_TOPK_MAX];
    int      det_total;        //检测目标总数
    int      det_n_cls;
    GuiDetCls   det_cls[GUI_DET_CLS_MAX];
    //---- 当前模型 ----
    int      model_idx;
    char     model_name[GUI_LBL_MAX];
    char     model_type[GUI_LBL_MAX];   //"分类"/"检测"
    //---- 拍照 ----
    int      cap_ok;
    char     cap_file[GUI_LBL_MAX];
    //---- 照片查看(相机应用, app_task代读BMP后发布像素) ----
    void*    photo_pix;         //BGR像素(顶行在先), 0=无; 生命周期: 置0后的
                                //旧缓冲由app_task延迟释放, gui侧只在pix非0时引用
    int      photo_w;
    int      photo_h;
    char     photo_name[GUI_LBL_MAX];   //文件名(IMG_0001.bmp)
    //---- 文件系统(builtin/files应用, app_task代读SD) ----
    char     fs_path[GUI_FS_PATH_MAX];   //本列表对应的目录
    int      fs_err;                     //0成功; -1未挂载; -2打开失败
    int      fs_truncated;               //条目超出GUI_FS_MAX被截断
    int      fs_n;
    GuiFsEntry fs[GUI_FS_MAX];
    //---- 文件操作结果(文件应用批量复制/删除, 每完成一条命令累加一次;
    //      GUI按gen_fsop对账增量做成败汇总) ----
    uint32_t fsop_seq;                   //已完成操作条数(含失败)
    uint32_t fsop_ok;                    //累计成功条数
    uint32_t fsop_err;                   //累计失败条数
    //---- SD应用(M2) ----
    char     sdapp_name[GUI_SDAPP_MAX][16];   //图标名(AppHeader.name)
    char     sdapp_file[GUI_SDAPP_MAX][32];   //文件名(/apps/下)
    int      sdapp_n;
    int      app_state;                       //APPST_xxx
    char     app_err[32];
//---- SD模型清单(推理应用, 开机/重扫时app_task发布) ----
char     models_name[GUI_MODEL_MAX][GUI_LBL_MAX];   //显示名(SD文件夹名)
char     models_tag[GUI_MODEL_MAX][16];  //类型标签(模块自述tag原样显示: 分类/检测…)
//int     n_models;                      //复用上方开机字段(=models_n)
    //---- 检测框叠加(推理应用: gen_det变化时重画) ----
    int      det_src_w, det_src_h;            //框坐标所属源帧尺寸
    int      det_box_n;                       //0=清空(分类结果/停止)
    GuiDetBox det_box[GUI_DET_BOX_MAX];
    //---- 视频窗口(vidwin任务发布, 1s一次) ----
    unsigned vid_fps;           //窗口实际刷新帧率
    unsigned vid_us;            //单帧降采样+刷cache耗时(us)
    //---- 网口(net任务开机发布; 状态栏图标/传输应用用) ----
    int      net_up;            //lwIP栈+GEM0网卡就绪(静态IP已配)
    char     net_ip[16];        //板端IP文本
    //---- 文件传输(传输应用; net任务/app_task发布) ----
    int      xfer_state;        //XFER_xxx
    char     xfer_file[GUI_FS_NAME_MAX];   //当前/最近文件名
    uint32_t xfer_done;         //已收字节
    uint32_t xfer_total;        //总字节
    char     xfer_msg[40];      //结果/错误文本(完成/失败原因; RX时=速率文本)
    char     xfer_peer[16];     //PC端地址(连接起有值, 到下次连接前保留)
    uint8_t  xfer_res;          //最近一次结果: 0无 1成功 2失败(不随后续事件清除)
    char     xfer_res_name[GUI_FS_NAME_MAX];   //结果文件名(结果面板/底行)
    char     xfer_res_msg[40];  //结果附加文本(失败原因; 成功为空)
    uint32_t xfer_res_gen;      //结果代数(每记一次结果+1; UI据此不漏结果——
                                //DONE紧跟LISTEN挤进同一刷新拍时事件会被顶掉)
    //---- 主题壁纸(开机app_task加载0:/theme/wallpaper.bmp, gui常驻引用;
    //      pix所有权过户给gui, 不再释放; 0=无壁纸保持渐变背景) ----
    void*    wall_pix;
    //---- 分组代数(gui侧比对, 变了才解析) ----
    uint32_t gen_boot, gen_cam, gen_run, gen_infer, gen_res, gen_model,
             gen_cap, gen_fs, gen_fsop, gen_sdapp, gen_app, gen_vid,
             gen_photo, gen_models, gen_det, gen_net, gen_xfer, gen_wall;
} GuiSnapshot;

//初始化(app_init早期、gui_init之前调用一次; 创建mutex与静态缓冲)
//返回0成功; -1 mutex创建失败
int gui_model_init(void);

//---- 发布侧(app_task调用) ----
void gui_model_pub_boot(int sd_ok, int n_models,
                        int scr_id, int scr_w, int scr_h, int touch_id);
void gui_model_pub_cam(int on, int hdmi);                       //摄像头链路/HDMI状态
void gui_model_pub_run(int running, unsigned cycle);
void gui_model_pub_infer(unsigned ms);                       //自动维护环/均值/峰值
void gui_model_pub_classify(int n, const GuiTopKItem* items);
void gui_model_pub_detect(int n_cls, int total, const GuiDetCls* cls);
void gui_model_pub_model(int idx, const char* name, const char* type);
void gui_model_pub_capture(int ok, const char* file);
void gui_model_pub_photo(void* pix, int w, int h, const char* name);
void gui_model_pub_fs(const char* path, int err, int truncated,
                      const GuiFsEntry* entries, int n);
void gui_model_pub_fsop(int ok);                        //文件操作结果累加(app_task)
void gui_model_pub_sdapps(const char (*names)[16], const char (*files)[32], int n);
void gui_model_pub_app_state(int st, const char* err);
void gui_model_pub_vid(unsigned fps, unsigned us);              //vidwin任务调用
void gui_model_pub_models(const char (*names)[GUI_LBL_MAX],
                          const char (*tags)[16], int n);       //SD模型清单
void gui_model_pub_det(int src_w, int src_h,
                       const GuiDetBox* boxes, int n);          //检测框叠加
void gui_model_pub_net(int up, const char* ip);                 //网口栈状态
void gui_model_pub_wallpaper(void* pix);                        //主题壁纸(app_task)
void gui_model_pub_xfer(int state, const char* file,
                        uint32_t done, uint32_t total,
                        const char* msg);                       //文件传输进度

//---- 消费侧(gui任务调用) ----
void gui_model_snapshot(GuiSnapshot* out);                   //mutex内整体拷贝

//det分组代数的免锁快读(u32对齐单读原子; vid任务用作"要不要做整快照"的
//廉价把关, 推理未运行时视频任务零桥开销)
uint32_t gui_model_det_gen_fast(void);

#ifdef __cplusplus
}
#endif
