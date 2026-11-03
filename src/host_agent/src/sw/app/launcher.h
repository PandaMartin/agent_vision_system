// =============================================================================
// launcher —— 桌面启动器(系统壳: 桌面屏 + 应用生命周期)
//
// 桌面 = 状态栏(存储/相机状态/帧率) + 应用图标网格; 每个应用独占一个screen:
// 打开 = 框架建屏(顶栏返回键+标题+内容区) -> 应用on_open填内容 -> 滑入动画;
// 返回 = 桌面滑回 + 整屏删除(LVGL对象生命周期由框架强制, 应用零清理负担,
// on_close只处理非LVGL资源)。新应用 = 实现LauncherApp + launcher_init里一行注册。
// toast挂在lv_layer_top, 桌面与应用内都可见。
// =============================================================================
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

//界面版本(系统页显示)
#define LAUNCHER_VERSION  "v2.2"

//应用屏布局常量(vidwin的帧缓冲"洞"坐标与此镜像, 两处必须一致)
#define LAUNCHER_APP_TOPBAR_H  40     //应用屏顶栏高(返回键+标题)
#define LAUNCHER_APP_PAD        8     //应用屏/桌面边距

//应用描述符: 框架按此驱动一个应用的生命周期
typedef struct {
    const char* name;                       //图标下名称(中文, font_cn_16渲染)
    const void* icon;                       //图标(LV_SYMBOL_*, Montserrat 32)
    void (*on_open)(lv_obj_t* parent);      //建应用内容(parent=顶栏之下的内容区)
    void (*on_close)(void);                 //退出清理(只处理非LVGL资源; LVGL对象
                                            //由框架整屏删除, 此回调里不得再碰) 可为NULL
    void (*refresh)(void);                  //周期刷新(读快照更新自身, 可为NULL)
    uint32_t  refresh_ms;                   //刷新周期ms, 0=不周期刷新
    uint32_t  accent;                       //桌面tile主色(渐变顶色, 0=默认青)
} LauncherApp;

//初始化桌面(主题设置后调用一次): 状态栏+图标网格+toast+节拍定时器
void launcher_init(void);

//注册应用(launcher_init内部按需调用; 注册顺序=图标顺序), 上限8个
void launcher_register(const LauncherApp* app);

//当前应用索引(注册顺序, 0=监控); -1=桌面
//vidwin视频窗口用它判断"监控应用是否在场"
int launcher_active_app(void);

//打开应用(桌面图标点击, gui任务上下文); 已在应用中则忽略
void launcher_app_open(int idx);

//退出当前应用回桌面(返回键); 桌面态忽略
void launcher_app_close(void);

//轻提示(仅gui任务上下文): 底部胶囊淡入停留淡出, 排队逐条显示
void launcher_toast(const char* text);

//加长停留版(操作结果等需要细读的提示, 停留约4s; 排队同上)
void launcher_toast_long(const char* text);

#ifdef __cplusplus
}
#endif
