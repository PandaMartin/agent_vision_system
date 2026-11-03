// =============================================================================
// 贪吃蛇 —— 第一个SD卡加载应用(M2示范)
//
// 展示完整应用契约(见sw/app/ext_app/loader.h):
//   app_boot(.app_entry_first, 恒定入口) -> app_main建界面并快速返回;
//   逻辑全部在app_timer回调(gui任务上下文); 退出按钮调fw_app_exit()。
// 状态放static(.bss): 每次启动由加载器清零, 天然复位。
// 链接: tools/sdapp/sdapp.py build(裸机+fw_exports.ld直接调固件函数, 无libc无包装)
// =============================================================================

#include "lvgl.h"

#include "sw/app/ext_app/loader.h"

//---- 布局(内容区784x424, 坐标系同内置应用) ----
//格子缩到25列(600px), 给右侧方向键十字让位(634..766)
#define GRID_X      8
#define GRID_Y      60
#define COLS        25
#define ROWS        14
#define CELL        24

//---- 配色 ----
#define COL_EMPTY   0x0E1422
#define COL_BODY    0x76FF03
#define COL_HEAD    0xFF1744
#define COL_FOOD    0xFFEA00
#define COL_DIM     0x8A94B8

//---- 方向 ----
enum { DIR_RIGHT = 0, DIR_DOWN, DIR_LEFT, DIR_UP };

static lv_obj_t* s_cells[ROWS][COLS];
static lv_obj_t* s_lbl_score;
static lv_obj_t* s_lbl_over;
static lv_timer_t* s_timer;

static struct {
    int8_t body_x[COLS * ROWS];      //环形队列(头在下标head)
    int8_t body_y[COLS * ROWS];
    int head, tail;                  //队列首尾(尾=最老, 吃食物时尾不弹)
    int len;
    int dir, dir_next;
    int food_x, food_y;
    int dead;
    unsigned score;
    unsigned seed;                   //LCG随机(无libc)
} S;

//LCG伪随机(0..n-1)
static int rnd(int n)
{
    S.seed = S.seed * 1664525u + 1013904223u;
    return (int)((S.seed >> 8) % (unsigned)n);
}

static void cell_draw(int x, int y, uint32_t color)
{
    lv_obj_set_style_bg_color(s_cells[y][x], lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s_cells[y][x], LV_OPA_COVER, 0);
}

static void cell_clear(int x, int y)
{
    lv_obj_set_style_bg_opa(s_cells[y][x], LV_OPA_TRANSP, 0);
}

static void food_spawn(void)
{
    for (;;)
    {
        int x = rnd(COLS), y = rnd(ROWS);
        int hit = 0;
        for (int i = S.tail; i != S.head; i = (i + 1) % (COLS * ROWS))
            if (S.body_x[i] == x && S.body_y[i] == y) { hit = 1; break; }
        if (!hit)
        {
            S.food_x = x;
            S.food_y = y;
            cell_draw(x, y, COL_FOOD);
            return;
        }
    }
}

static void game_reset(void)
{
    while (S.tail != S.head)
    {
        cell_clear(S.body_x[S.tail], S.body_y[S.tail]);
        S.tail = (S.tail + 1) % (COLS * ROWS);
    }
    S.head = S.tail = 0;
    S.len = 0;
    S.dead = 0;
    S.score = 0;
    S.dir = S.dir_next = DIR_RIGHT;
    S.seed = (unsigned)lv_tick_get() | 1u;

    static const int sx[3] = { 4, 5, 6 };   //队列序=4尾->6头: 头朝右,向右走
    for (int i = 0; i < 3; i++)
    {
        S.body_x[S.head] = (int8_t)sx[i];
        S.body_y[S.head] = ROWS / 2;
        S.head = (S.head + 1) % (COLS * ROWS);
        S.len++;
        cell_draw(sx[i], ROWS / 2, COL_BODY);
    }
    cell_draw(sx[2], ROWS / 2, COL_HEAD);   //队尾写入的是头
    lv_label_set_text_fmt(s_lbl_score, "得分 %u", S.score);
    lv_obj_add_flag(s_lbl_over, LV_OBJ_FLAG_HIDDEN);
    food_spawn();
}

static void game_over(void)
{
    S.dead = 1;
    lv_obj_remove_flag(s_lbl_over, LV_OBJ_FLAG_HIDDEN);
}

//定时器回调: 走一步(gui任务上下文)
static void tick_cb(lv_timer_t* t)
{
    (void)t;
    if (S.dead)
        return;

    S.dir = S.dir_next;
    static const int dx[4] = { 1, 0, -1, 0 };
    static const int dy[4] = { 0, 1, 0, -1 };
    int nx = S.body_x[(S.head + COLS * ROWS - 1) % (COLS * ROWS)] + dx[S.dir];
    int ny = S.body_y[(S.head + COLS * ROWS - 1) % (COLS * ROWS)] + dy[S.dir];

    //撞墙/撞身
    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS)
    {
        game_over();
        return;
    }
    for (int i = S.tail; i != S.head; i = (i + 1) % (COLS * ROWS))
        if (S.body_x[i] == nx && S.body_y[i] == ny)
        {
            game_over();
            return;
        }

    //新头入队
    S.body_x[S.head] = (int8_t)nx;
    S.body_y[S.head] = (int8_t)ny;
    S.head = (S.head + 1) % (COLS * ROWS);
    S.len++;

    if (nx == S.food_x && ny == S.food_y)
    {
        S.score += 10;
        lv_label_set_text_fmt(s_lbl_score, "得分 %u", S.score);
        food_spawn();
    }
    else
    {
        //没吃到: 弹出尾巴
        cell_clear(S.body_x[S.tail], S.body_y[S.tail]);
        S.tail = (S.tail + 1) % (COLS * ROWS);
        S.len--;
    }
    cell_draw(nx, ny, COL_HEAD);
    //旧头降级为身体
    int old_head = (S.head + COLS * ROWS - 2) % (COLS * ROWS);
    cell_draw(S.body_x[old_head], S.body_y[old_head], COL_BODY);
}

//---- 输入 ----
static void dpad_cb(lv_event_t* e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    static const int opp[4] = { DIR_LEFT, DIR_UP, DIR_RIGHT, DIR_DOWN };
    if (!S.dead && d != opp[S.dir])
        S.dir_next = d;
}

static void grid_click_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (S.dead)
        game_reset();
}

static void btn_exit_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    fw_app_exit();                       //之后不得再碰任何LVGL对象
}

//---- 界面 ----
static lv_obj_t* label(lv_obj_t* parent, int x, int y, const char* text,
                       const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t* dpad_btn(lv_obj_t* parent, int x, int y, const char* sym,
                          int dir)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, 44, 44);
    lv_obj_t* ic = lv_label_create(b);
    //箭头符号在Montserrat符号区: 必须显式指定(默认字体font_cn_16无符号字形)
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
    lv_label_set_text(ic, sym);
    lv_obj_center(ic);
    lv_obj_add_event_cb(b, dpad_cb, LV_EVENT_CLICKED, (void*)(intptr_t)dir);
    return b;
}

void app_main(void)
{
    lv_obj_t* scr = lv_obj_create(0);    //应用独占screen
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E1A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    label(scr, 8, 8, "SNAKE", &lv_font_montserrat_20, 0x00E5FF);
    s_lbl_score = label(scr, 340, 10, "得分 0", &font_cn_16, 0xFFFFFF);

    lv_obj_t* btn_exit = lv_button_create(scr);
    lv_obj_set_pos(btn_exit, 708, 6);
    lv_obj_set_size(btn_exit, 68, 30);
    lv_obj_t* ex = lv_label_create(btn_exit);
    lv_obj_set_style_text_font(ex, &font_cn_16, 0);
    lv_obj_set_style_text_color(ex, lv_color_white(), 0);
    lv_label_set_text(ex, "退出");      //中文字库无符号字形, 不用LV_SYMBOL
    lv_obj_center(ex);
    lv_obj_add_event_cb(btn_exit, btn_exit_cb, LV_EVENT_CLICKED, 0);

    //格子(392个纯色块, 只在变化时重绘)
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
        {
            lv_obj_t* c = lv_obj_create(scr);
            //不可点击: 点击穿透到scr(触格重开); 不可滚动
            lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(c, GRID_X + x * CELL, GRID_Y + y * CELL);
            lv_obj_set_size(c, CELL - 2, CELL - 2);
            lv_obj_set_style_radius(c, 4, 0);
            lv_obj_set_style_border_width(c, 0, 0);
            lv_obj_set_style_bg_color(c, lv_color_hex(COL_EMPTY), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
            s_cells[y][x] = c;
        }

    //GAME OVER + 触格重开
    s_lbl_over = label(scr, 0, 0, "GAME OVER", &lv_font_montserrat_32,
                       0xFF1744);
    lv_obj_align(s_lbl_over, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_add_flag(s_lbl_over, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(scr, grid_click_cb, LV_EVENT_CLICKED, 0);

    //方向键(右侧十字, 中心约(700,254); 格子止于608不重叠)
    dpad_btn(scr, 634, 232, LV_SYMBOL_LEFT, DIR_LEFT);
    dpad_btn(scr, 722, 232, LV_SYMBOL_RIGHT, DIR_RIGHT);
    dpad_btn(scr, 678, 188, LV_SYMBOL_UP, DIR_UP);
    dpad_btn(scr, 678, 276, LV_SYMBOL_DOWN, DIR_DOWN);

    game_reset();

    lv_screen_load(scr);                 //应用自加载其screen(契约)

    //应用定时器(框架记账; 勿用lv_timer_create)
    s_timer = app_timer_create(tick_cb, 200, 0);
}

//恒定入口(APP_LOAD_BASE, 见tools/sdapp/app.ld)
__attribute__((section(".app_entry_first")))
void app_boot(void)
{
    app_main();
}
