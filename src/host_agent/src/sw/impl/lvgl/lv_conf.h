// =============================================================================
// lv_conf.h —— LVGL 9.4.0 配置(基于 lv_conf_template.h 裁剪)
//
// 目标平台: Zynq-7020 PS (Cortex-A9, NEON, FreeRTOS, 裸帧缓冲LCD RGB888)
// 渲染路径: LVGL软件渲染(NEON加速) -> 部分重绘缓冲(RGB888) -> flush_cb
//           逐行memcpy到VDMA0帧缓存(BGR字节序与LVGL RGB888一致) + 刷D-Cache
// OS集成:   LV_OS_NONE, lv_timer_handler由sw/gui自己的FreeRTOS任务驱动,
//           tick通过lv_tick_set_cb挂到FreeRTOS tick
// =============================================================================
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/
//32位色深(XRGB8888): RGB888显示格式的配套选择(lv_color_t=32bit,
//渐变/文字等中间层走ARGB8888, 目标格式由lv_display_set_color_format设RGB888)
#define LV_COLOR_DEPTH 32

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/
//全部用LVGL内置实现(TLSF/lv_memcpy/lv_sprintf), 不依赖newlib的C++/locale
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#define LV_LIMITS_INCLUDE       <limits.h>
#define LV_STDARG_INCLUDE       <stdarg.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
//LVGL对象/样式/图表等的长寿堆(静态数组, 落在DDR): 仪表盘UI实测余量充足
#define LV_MEM_SIZE (192 * 1024U)
#define LV_MEM_POOL_EXPAND_SIZE 0
#define LV_MEM_ADR 0
#if LV_MEM_ADR == 0
    #undef LV_MEM_POOL_INCLUDE
    #undef LV_MEM_POOL_ALLOC
#endif
#endif

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DEF_REFR_PERIOD  33      //约30fps
#define LV_DPI_DEF          130

/*=================
 * OPERATING SYSTEM
 *=================*/
//不用LVGL的OS抽象层: 独占式单任务访问(仅gui任务碰LVGL API), 无需内部锁
#define LV_USE_OS   LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/
#define LV_DRAW_BUF_STRIDE_ALIGN                1   //行距=w*3(RGB888紧凑), flush按整行拷贝
#define LV_DRAW_BUF_ALIGN                       4

#define LV_DRAW_TRANSFORM_USE_MATRIX            0
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE    (24 * 1024)
#define LV_DRAW_LAYER_MAX_MEMORY 0
#define LV_DRAW_THREAD_STACK_SIZE    (8 * 1024)   //LV_OS_NONE下不使用
#define LV_DRAW_THREAD_PRIO          1

#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1
    //按需保留格式: 目标RGB888 + 文字/渐变中间层ARGB8888/XRGB8888 + 掩码A8
    #define LV_DRAW_SW_SUPPORT_RGB565       0
    #define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0
    #define LV_DRAW_SW_SUPPORT_RGB565A8     0
    #define LV_DRAW_SW_SUPPORT_RGB888       1
    #define LV_DRAW_SW_SUPPORT_XRGB8888     1
    #define LV_DRAW_SW_SUPPORT_ARGB8888     1
    #define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
    #define LV_DRAW_SW_SUPPORT_L8           0
    #define LV_DRAW_SW_SUPPORT_AL88         0
    #define LV_DRAW_SW_SUPPORT_A8           1
    #define LV_DRAW_SW_SUPPORT_I1           0

    #define LV_DRAW_SW_I1_LUM_THRESHOLD 127

    #define LV_DRAW_SW_DRAW_UNIT_CNT    1   //单核A9单绘制单元

    #define LV_USE_DRAW_ARM2D_SYNC      0
    #define LV_USE_NATIVE_HELIUM_ASM    0

    //复杂渲染器: 圆角/阴影/斜线/圆弧都要(炫酷UI必需)
    #define LV_DRAW_SW_COMPLEX          1
    #if LV_DRAW_SW_COMPLEX == 1
        #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
        #define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
    #endif

    //Cortex-A9 NEON指令加速blend/memset(工程已带 -mfpu=neon-vfpv3)
    #define  LV_USE_DRAW_SW_ASM     LV_DRAW_SW_ASM_NEON

    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS    0   //复杂渐变(角度/径向)暂不用
#endif

#define LV_USE_NEMA_GFX 0
#define LV_USE_PXP 0
#define LV_USE_G2D 0
#define LV_USE_DRAW_DAVE2D 0
#define LV_USE_DRAW_SDL 0
#define LV_USE_DRAW_VG_LITE 0
#define LV_USE_DRAW_DMA2D 0
#define LV_USE_DRAW_OPENGLES 0
#define LV_USE_PPA  0
#define LV_USE_DRAW_EVE 0

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/
#define LV_USE_LOG 0

//断言失败打印后继续(默认while(1)会悄悄挂死gui任务, 难以定位)
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE "xil_printf.h"
#define LV_ASSERT_HANDLER xil_printf("[lvgl] assert fail\r\n");

#define LV_USE_REFR_DEBUG 0
#define LV_USE_LAYER_DEBUG 0
#define LV_USE_PARALLEL_DRAW_DEBUG 0

/*-------------
 * Others
 *-----------*/
#define LV_ENABLE_GLOBAL_CUSTOM 0

#define LV_CACHE_DEF_SIZE       0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_GRADIENT_MAX_STOPS   3     //仪表盘渐变用3个色标
#define LV_COLOR_MIX_ROUND_OFS  0
#define LV_OBJ_STYLE_CACHE      0

#define LV_USE_OBJ_ID           0
#define LV_USE_OBJ_NAME         0
#define LV_OBJ_ID_AUTO_ASSIGN   LV_USE_OBJ_ID
#define LV_USE_OBJ_ID_BUILTIN   1

#define LV_USE_OBJ_PROPERTY 0
#define LV_USE_OBJ_PROPERTY_NAME 1

#define LV_USE_GESTURE_RECOGNITION 0

/*=====================
 *  COMPILER SETTINGS
 *====================*/
#define LV_BIG_ENDIAN_SYSTEM 0

#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_FAST_MEM
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning
#define LV_ATTRIBUTE_EXTERN_DATA

#define LV_USE_FLOAT            0
#define LV_USE_MATRIX           0

#ifndef LV_USE_PRIVATE_API
    #define LV_USE_PRIVATE_API  0
#endif

/*==================
 *   FONT USAGE
 *===================*/
//Montserrat数字/字母字形漂亮, 按界面字号按需启用(DDR不在乎体积, 在乎编译时间)
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_MONTSERRAT_28_COMPRESSED    0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW    0
#define LV_FONT_SOURCE_HAN_SANS_SC_14_CJK   0
#define LV_FONT_SOURCE_HAN_SANS_SC_16_CJK   0

#define LV_FONT_UNSCII_8  0
#define LV_FONT_UNSCII_16 0

//自制中文子集字体(fonts/font_cn_16.c、font_cn_20.c, 由lv_font_conv从黑体生成,
//字符集=全部界面文案+富余, 约230个汉字+全ASCII); 数字大字号仍用Montserrat
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(font_cn_16) \
    LV_FONT_DECLARE(font_cn_20)

#define LV_FONT_DEFAULT &font_cn_16

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_PLACEHOLDER 1

/*=================
 *  TEXT SETTINGS
 *=================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_)]}"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN 3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3

#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0
#define LV_TXT_COLOR_CMD "#"

/*==================
 * WIDGETS
 *================*/
#define LV_WIDGETS_HAS_DEFAULT_VALUE  1

#define LV_USE_ANIMIMG    0
#define LV_USE_ARC        1   //仪表盘圆弧
#define LV_USE_ARCLABEL   0
#define LV_USE_BAR        1   //进度条(滑杆依赖)
#define LV_USE_BUTTON     1
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR   0
#define LV_USE_CANVAS     0
#define LV_USE_CHART      1   //实时曲线
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   0
#define LV_USE_IMAGE      1
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD   0
#define LV_USE_LABEL      1
#define LV_USE_LED        1   //状态指示灯
#define LV_USE_LINE       1
#define LV_USE_LIST       0
#define LV_USE_LOTTIE     0
#define LV_USE_MENU       0
#define LV_USE_MSGBOX     1
#define LV_USE_ROLLER     0
#define LV_USE_SCALE      1   //刻度尺(炫酷仪表元素)
#define LV_USE_SLIDER     1
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1   //加载动画
#define LV_USE_SWITCH     1
#define LV_USE_TABLE      0
#define LV_USE_TABVIEW    0
#define LV_USE_TEXTAREA   0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_3DTEXTURE  0

/*==================
 * THEMES
 *==================*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
#define LV_THEME_DEFAULT_DARK 1              //深色主题
#define LV_THEME_DEFAULT_GROW 1              //按下时放大反馈
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif

#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO   0

/*==================
 * LAYOUTS
 *==================*/
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*====================
 * 3RD PARTS LIBRARIES
 *====================*/
//文件系统/图片解码/矢量/字体库全部关闭(源码保留, 按需再开)
#define LV_FS_DEFAULT_DRIVER_LETTER '\0'
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_FS_MEMFS 0
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_FS_ARDUINO_ESP_LITTLEFS 0
#define LV_USE_FS_ARDUINO_SD 0
#define LV_USE_FS_UEFI 0
#define LV_USE_FS_FROGFS 0

#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_BMP 0
#define LV_USE_TJPGD 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_GIF 0
#define LV_USE_GSTREAMER 0
#define LV_USE_BIN_DECODER_RAM_LOAD 0
#define LV_USE_RLE 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_RLOTTIE 0
#define LV_USE_GLTF  0
#define LV_USE_VECTOR_GRAPHIC  0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG_EXTERNAL 0
#define LV_USE_LZ4_INTERNAL  0
#define LV_USE_LZ4_EXTERNAL  0
#define LV_USE_SVG 0
#define LV_USE_SVG_ANIMATION 0
#define LV_USE_SVG_DEBUG 0
#define LV_USE_FFMPEG 0

/*==================
 * OTHERS
 *==================*/
#define LV_USE_SNAPSHOT 0
#define LV_USE_SYSMON   0
#define LV_USE_PROFILER 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_OBSERVER 0
#define LV_USE_IME_PINYIN 0
#define LV_USE_FILE_EXPLORER 0
#define LV_USE_FONT_MANAGER 0
#define LV_USE_TEST 0
#define LV_USE_XML    0
#define LV_USE_TRANSLATION 0
#define LV_USE_COLOR_FILTER     0

/*==================
 * DEVICES
 *==================*/
//显示/输入走自有移植层(sw/gui), LVGL自带设备后端全关
#define LV_USE_SDL              0
#define LV_USE_X11              0
#define LV_USE_WAYLAND          0
#define LV_USE_LINUX_FBDEV      0
#define LV_USE_NUTTX            0
#define LV_USE_LINUX_DRM        0
#define LV_USE_TFT_ESPI         0
#define LV_USE_LOVYAN_GFX       0
#define LV_USE_EVDEV            0
#define LV_USE_LIBINPUT         0
#define LV_USE_ST7735        0
#define LV_USE_ST7789        0
#define LV_USE_ST7796        0
#define LV_USE_ILI9341       0
#define LV_USE_FT81X         0
#define LV_USE_NV3007        0
#define LV_USE_GENERIC_MIPI  0
#define LV_USE_RENESAS_GLCDC 0
#define LV_USE_ST_LTDC       0
#define LV_USE_NXP_ELCDIF    0
#define LV_USE_WINDOWS       0
#define LV_USE_UEFI          0
#define LV_USE_OPENGLES      0
#define LV_USE_GLFW          0
#define LV_USE_QNX           0

/*=====================
* BUILD OPTIONS
 *======================*/
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

/*--END OF LV_CONF_H--*/
#endif /*LV_CONF_H*/
