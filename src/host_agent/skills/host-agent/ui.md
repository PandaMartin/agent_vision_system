# 板端界面

屏幕 800×480。和板子桌面同一套：深色渐变底、一张主卡片、一种青色强调。层次靠字号、留白和下面这些控件，不靠网格、角标和光晕。

写界面之前读这一页。每个要上屏的汉字先用 `query_symbols` 的 name 核对。控件函数都在固件符号表里，直接调用。拿不准函数名时，用 `query_symbols`，name 填 `lv_chart_` 这样的前缀。

一屏只做一个主视觉：大数字、刻度盘、圆弧或曲线四选一。再配 1 到 3 个辅助控件（进度条、滑杆、开关、复选框、指示灯）。不要把所有控件堆在同一屏。

## 字号

声明用到的字体，没用到的不要声明：

```c
extern const lv_font_t font_cn_16;
extern const lv_font_t font_cn_20;
extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_48;
```

- 屏幕标题：`font_cn_20`，白色。
- 说明、按钮、单位里的汉字：`font_cn_16`，次要信息用 `0x8A94B8`。
- 主数字（时间、计数、分数）：`lv_font_montserrat_48`，只放数字和 `:` `.` `-`。汉字不能放进英文字体。
- 刻度数字、单位：`lv_font_montserrat_14`，颜色 `0x8A94B8`。
- `font_cn_16` 和 `font_cn_20` 字库相同。

## 颜色

- 背景自上而下 `0x0A0E1A` 到 `0x05070D`
- 卡片 `0x141B2E`，边 `0x263050`，轨道 `0x1A2440`
- 正文 `0xFFFFFF`，次要文字 `0x8A94B8`
- 强调色只有 `0x00E5FF`。主按钮填充可用 `0x0F5E6E`
- 正常指示灯 `0x76FF03`，异常 `0xFF8A80`。不要拿来铺大面积底色

不要同时使用品红和青色。不要用 LVGL 主题默认的亮蓝按钮。卡片和按钮用 `lv_obj_remove_style_all` 后自己画。圆弧、进度条、滑杆、开关、曲线、刻度不要清空样式，只改轨道、指示和旋钮的颜色。

## 版面

边距 24。控件用整数坐标，不用 `lv_pct`，不读控件宽高。点数组必须是 `static`，不能 `malloc`。

- 标题在 `(24, 18)`。
- 退出在右上角 `(688, 16)`，宽 88，高 40，文字是「退出」。
- 主卡片 `(24, 80)`，宽 752。只有大数字时高 280；里面放刻度或曲线时高 340。
- 并列：左卡 `(24, 80, 460, 340)`，右卡 `(500, 80, 276, 340)`。
- 底部操作从 `(24, 424)` 起，按钮高 44，间距 16。主操作只有一个。
- 设置行：说明在左，开关或滑杆在右，行距 56。
- 根对象和卡片去掉滚动：`lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);`

背景：

```c
lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E1A), 0);
lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x05070D), 0);
lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
```

主卡片可以加一道轻阴影，不要每张卡都加：

```c
lv_obj_set_style_shadow_width(card, 18, 0);
lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
lv_obj_set_style_shadow_offset_y(card, 6, 0);
```

## 卡片和按钮

```c
static lv_obj_t* make_card(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141B2E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263050), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t* make_btn(lv_obj_t* parent, int x, int y, int w, int h,
                          const char* text, int accent, lv_event_cb_t cb)
{
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(accent ? 0x0F5E6E : 0x141B2E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(accent ? 0x00E5FF : 0x263050), 0);
    lv_obj_set_style_border_width(btn, accent ? 2 : 1, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* lab = lv_label_create(btn);
    lv_obj_set_style_text_font(lab, &font_cn_16, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lab, text);
    lv_obj_center(lab);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, 0);
    return btn;
}
```

`accent` 为 1 的按钮整屏只用一颗。

## 可用控件

按任务选，不要每种都放。

指示灯。状态用颜色区分，旁边配一句 `font_cn_16`：

```c
lv_obj_t* led = lv_led_create(card);
lv_obj_set_pos(led, 28, 28);
lv_obj_set_size(led, 16, 16);
lv_led_set_color(led, lv_color_hex(0x76FF03));
lv_led_on(led);
```

熄灭用 `lv_led_off`。异常色 `0xFF8A80`，进行中用 `0x00E5FF`。

进度条：

```c
lv_obj_t* bar = lv_bar_create(card);
lv_obj_set_pos(bar, 28, 220);
lv_obj_set_size(bar, 480, 12);
lv_bar_set_range(bar, 0, 100);
lv_bar_set_value(bar, 40, LV_ANIM_OFF);
lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A2440), LV_PART_MAIN);
lv_obj_set_style_bg_color(bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
```

圆弧。适合百分比、秒数。中间另放一枚 `montserrat_48` 数字，圆弧本身不写字：

```c
lv_obj_t* arc = lv_arc_create(card);
lv_obj_set_pos(arc, 40, 36);
lv_obj_set_size(arc, 200, 200);
lv_arc_set_bg_angles(arc, 135, 45);
lv_arc_set_range(arc, 0, 100);
lv_arc_set_value(arc, 30);
lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
lv_obj_set_style_arc_color(arc, lv_color_hex(0x1A2440), LV_PART_MAIN);
lv_obj_set_style_arc_color(arc, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
lv_obj_set_style_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
```

圆形刻度加指针。适合仪表。刻度文字只用英文数字字体。指针的点由刻度控件管理，不要再给这根线 `lv_line_set_points`：

```c
lv_obj_t* scale = lv_scale_create(card);
lv_obj_set_pos(scale, 36, 24);
lv_obj_set_size(scale, 240, 240);
lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
lv_scale_set_range(scale, 0, 100);
lv_scale_set_angle_range(scale, 270);
lv_scale_set_rotation(scale, 135);
lv_scale_set_total_tick_count(scale, 21);
lv_scale_set_major_tick_every(scale, 5);
lv_scale_set_label_show(scale, 1);
lv_obj_set_style_text_font(scale, &lv_font_montserrat_14, 0);
lv_obj_set_style_text_color(scale, lv_color_hex(0x8A94B8), 0);
lv_obj_set_style_line_color(scale, lv_color_hex(0x8A94B8), LV_PART_INDICATOR);
lv_obj_t* needle = lv_line_create(scale);
lv_obj_set_style_line_width(needle, 3, 0);
lv_obj_set_style_line_color(needle, lv_color_hex(0x00E5FF), 0);
lv_scale_set_line_needle_value(scale, needle, 78, 40);
```

改数值时再次调用 `lv_scale_set_line_needle_value`。

曲线。点数不要超过 40。序列指针存成 `static`，定时器里追加：

```c
static lv_chart_series_t* s_ser;
lv_obj_t* chart = lv_chart_create(card);
lv_obj_set_pos(chart, 24, 56);
lv_obj_set_size(chart, 700, 200);
lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
lv_chart_set_point_count(chart, 32);
lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
lv_chart_set_div_line_count(chart, 3, 4);
lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
lv_obj_set_style_bg_color(chart, lv_color_hex(0x0A0E1A), 0);
lv_obj_set_style_border_color(chart, lv_color_hex(0x263050), 0);
lv_obj_set_style_line_color(chart, lv_color_hex(0x263050), LV_PART_MAIN);
s_ser = lv_chart_add_series(chart, lv_color_hex(0x00E5FF), LV_CHART_AXIS_PRIMARY_Y);
lv_chart_set_next_value(chart, s_ser, 20);
```

滑杆。事件用 `LV_EVENT_VALUE_CHANGED`，读值用 `lv_slider_get_value`：

```c
lv_obj_t* slider = lv_slider_create(card);
lv_obj_set_pos(slider, 160, 48);
lv_obj_set_size(slider, 420, 14);
lv_slider_set_range(slider, 0, 100);
lv_slider_set_value(slider, 30, LV_ANIM_OFF);
lv_obj_set_style_bg_color(slider, lv_color_hex(0x1A2440), LV_PART_MAIN);
lv_obj_set_style_bg_color(slider, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
```

开关。查询用 `lv_obj_has_state(sw, LV_STATE_CHECKED)`：

```c
lv_obj_t* sw = lv_switch_create(card);
lv_obj_set_pos(sw, 640, 40);
lv_obj_set_size(sw, 64, 32);
lv_obj_set_style_bg_color(sw, lv_color_hex(0x263050), LV_PART_MAIN);
lv_obj_set_style_bg_color(sw, lv_color_hex(0x00E5FF), LV_PART_INDICATOR | LV_STATE_CHECKED);
```

默认打开再加 `lv_obj_add_state(sw, LV_STATE_CHECKED)`。

复选框。文字用静态字符串，汉字必须在字库里：

```c
lv_obj_t* box = lv_checkbox_create(card);
lv_obj_set_pos(box, 28, 48);
lv_checkbox_set_text_static(box, "静音");
lv_obj_set_style_text_font(box, &font_cn_16, 0);
lv_obj_set_style_text_color(box, lv_color_hex(0xFFFFFF), 0);
```

等待。只在确实要等待时放一个，不要一直转：

```c
lv_obj_t* spin = lv_spinner_create(card);
lv_obj_set_pos(spin, 352, 120);
lv_obj_set_size(spin, 48, 48);
lv_spinner_set_anim_params(spin, 1000, 60);
lv_obj_set_style_arc_color(spin, lv_color_hex(0x1A2440), LV_PART_MAIN);
lv_obj_set_style_arc_color(spin, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
```

一根分隔线可以。点必须是静态数组，不要用循环铺满屏幕：

```c
static const lv_point_precise_t s_rule[] = { {24, 0}, {728, 0} };
lv_obj_t* rule = lv_line_create(card);
lv_obj_set_pos(rule, 0, 64);
lv_line_set_points(rule, s_rule, 2);
lv_obj_set_style_line_width(rule, 1, 0);
lv_obj_set_style_line_color(rule, lv_color_hex(0x263050), 0);
```

## 怎么搭配

- 秒表、计数：主卡片里一枚 48 像素数字，下面一颗进度条或圆弧，底部两个按钮。
- 仪表：左侧圆形刻度，右侧大数字和一句说明，角上一个指示灯。
- 趋势：标题下一条曲线，曲线上用 14 像素数字标当前值。
- 设置：每行左边汉字说明，右边开关或滑杆。

## 不要做的

- 不要把标题、数字、按钮都设成 `font_cn_16`。
- 不要画全屏网格、四角折线、多层描边光晕。
- 不要用 `lv_button_create` 的默认外观。
- 不要使用 `lv_image` 和 `lv_msgbox`。应用里没有图片资源，弹窗会挡住整屏。
- 不要在对话里描述完界面就算完成，控件要写进 `app_main.c`。
