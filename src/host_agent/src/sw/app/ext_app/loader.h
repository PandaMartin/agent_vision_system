// =============================================================================
// loader —— SD应用加载器(M2): 固件=常驻系统, 应用=SD加载的裸机器码模块
//
// 应用bin布局: [AppHeader 64B][text+data](加载到APP_LOAD_BASE, 链接地址=
// 加载地址, 零重定位); bss不占bin, 由加载器清零。应用直接调用固件函数:
// 链接 tools/sdapp/fw_exports.ld(固件.map自动导出的全仓库符号, 带版本号)。
//
// 应用契约(违反=应用自身坏, 不伤系统):
//   入口  APP_LOAD_BASE处的app_boot(链接脚本保证第一函数, 恒定入口0偏移)
//         其内部调app_main(); entry须快速返回(建screen+app_timer驱动逻辑)
//   线程  entry及timer回调都在gui任务上下文执行(FPU已登记)
//   计时  用app_timer_create/del(框架记账, 退出时强制删除);
//         直接lv_timer_create会泄漏且退出后仍会触发
//   退出  调fw_app_exit()后立即从回调返回, 不得再碰任何LVGL对象——
//         框架加载回桌面(动画结束自动删除应用screen及全部子对象)
//   加载  每次启动都重新从SD拷贝+bss清零, 应用.static状态天然复位
//
// 应用构建: tools/sdapp/sdapp.py build(编译-链接-封头), 见docs/应用开发指南
// =============================================================================
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

//应用区(DDR 1GB, 固件实体在0x100000~0x11400000, 应用独立高位段)
#define APP_LOAD_BASE   0x20000000u
#define APP_REGION_SIZE 0x01000000u     //16MB
#define APP_MAGIC       "ZAPP"
#define APP_HDR_SIZE    64u

//bin头部(前64字节, 小端; 与tools/sdapp/sdapp.py的封包严格一致)
typedef struct {
    char     magic[4];       //"ZAPP"
    uint32_t hdr_size;       //=64
    uint32_t fw_version;     //必须等于固件符号导出版本(不匹配=拒载)
    uint32_t load_size;      //header之后text+data字节数(拷到APP_LOAD_BASE)
    uint32_t bss_size;       //bss字节数(加载器清零, 紧随load区)
    char     name[16];       //桌面图标名(UTF-8, 截断补0)
    uint32_t entry_off;      //=0(app_boot恒在APP_LOAD_BASE)
    uint32_t total_size;     //整个bin字节数(=64+load_size, 完整性)
    uint32_t bss_off;        //bss相对APP_LOAD_BASE的偏移(数据尾8对齐的空隙
                             //由sdapp.py build封包时填入; 加载器按此清零, 勿假设紧贴)
    uint8_t  pad[16];        //封包尾填充: 保证sizeof==64(缺它=镜像错位16字节)
} AppHeader;

/* 结构必须与封包格式逐字节一致(48!=64曾导致应用整体错位16字节的惨案) */
typedef char apphdr_size_check[(sizeof(AppHeader) == APP_HDR_SIZE) ? 1 : -1];

//---- 导出给应用的运行时(符号经fw_exports.ld对应用可见) ----

//应用定时器(框架记账; 退出时框架强制删除; gui任务上下文回调)
lv_timer_t* app_timer_create(lv_timer_cb_t cb, uint32_t period_ms, void* user_data);
void app_timer_del(lv_timer_t* t);

//请求退出应用(任意gui上下文回调里调): 返回后不得再碰LVGL对象
void fw_app_exit(void);

//---- 内部(launcher用, 应用勿调) ----
void loader_timers_reset(void);    //entry调用前重置记账表
void loader_timers_kill(void);     //退出时强制删除全部记账定时器

#ifdef __cplusplus
}
#endif
