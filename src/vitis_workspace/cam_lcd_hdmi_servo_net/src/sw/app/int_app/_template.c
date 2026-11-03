// =============================================================================
// 应用模板 —— 新建内置应用的骨架(复制改名后填空; 本文件不进编译)
//
// 四步接入:
//   1. 复制本文件为 sw/app/int_app/app_<name>.c, 实现 app_xxx 实例
//   2. sw/app/int_app/pages.h 加 extern 声明
//   3. sw/app/launcher.c 的 launcher_init() 里 launcher_register(&app_xxx)
//   4. UserConfig.cmake 源清单加 "sw/app/int_app/app_<name>.c"
//      (直接改文件, 别用Vitis IDE的添加源文件功能)
//
// UI铁律(违反任意一条都会出现控件消失/布局错乱):
//   a. 控件只用固定坐标+固定尺寸, 位置从 800x480 与 launcher 布局常量推算
//      (应用内容区 = x:8..792, y:48..472, 即784x424)
//   b. 禁止 lv_obj_get_width/height 读其他容器(LVGL惰性布局, 读到0)
//   c. 禁止 lv_pct() 参与像素加减(编码值不可算术)
//   d. lv_obj_set_flex_align 第一个参数才是主轴(SPACE_BETWEEN/EVENLY放第一)
//   e. 容器不需要点击时 remove CLICKABLE|SCROLLABLE 标志(默认可点击会吞触摸)
//
// 数据流(重活一律不走GUI线程):
//   GUI按钮 -> appctl_post(命令) -> main.cpp app_task 执行(SD/相机/推理)
//   结果 -> gui_model_pub_xxx 发布 -> 本应用 refresh() 比对代数后重绘
//   FatFs只允许app_task碰(参考int_app/app_files.c的FS_LIST范式)
//
// 新文案引入了字库外的汉字? 重跑: node tools/font/ui_chars.js 然后重新生成字体
// (流程见 tools/README.md 字库维护)
// =============================================================================

#include "lvgl.h"

#include "sw/app/launcher.h"
#include "sw/app/bridge.h"
#include "sw/app/appctl.h"
#include "sw/impl/lcd/lvgl_port.h"

//---- 配色(与launcher一致) ----
#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF

//---- 控件引用 ----
static lv_obj_t* s_lbl_demo;

//---- (可选)生命周期外的资源: 在on_close里释放, LVGL对象不用管 ----

// -----------------------------------------------------------------------------
static void app_on_open(lv_obj_t* parent)
{
    //内容区根节点(784x424): 固定尺寸, 不读parent
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 784, 424);

    //在这里建你的界面...
    s_lbl_demo = lv_label_create(root);
    lv_obj_set_pos(s_lbl_demo, 0, 0);
    lv_obj_set_style_text_font(s_lbl_demo, &font_cn_20, 0);
    lv_obj_set_style_text_color(s_lbl_demo, lv_color_hex(COL_CYAN), 0);
    lv_label_set_text(s_lbl_demo, "hello");
}

static void app_on_close(void)
{
    //释放非LVGL资源(如有); LVGL对象由launcher整屏删除, 此处不得再碰
}

//周期刷新(refresh_ms=0则不设): 读快照, 代数变了才重绘
static void app_refresh(void)
{
    // GuiSnapshot s;
    // gui_model_snapshot(&s);
    // if (s.gen_xxx == s_last_gen) return;
    // ...
}

// -----------------------------------------------------------------------------
const LauncherApp app_xxx = {
    .name       = "名称",              //图标下名称(字库需有这几个字)
    .icon       = LV_SYMBOL_SETTINGS,  //图标(Montserrat符号集)
    .on_open    = app_on_open,
    .on_close   = app_on_close,        //可为NULL
    .refresh    = app_refresh,         //可为NULL
    .refresh_ms = 200,
};
