// =============================================================================
// app_files —— 文件应用(内置): 浏览SD卡目录, 支持批量复制/剪切/粘贴/删除
//
// FatFs单线程纪律下的标准范式(与M2应用访问SD同原语):
//   GUI: 点目录行 -> appctl_post(APP_CMD_FS_LIST, path)
//   app_task: sd_list_dir代读 -> gui_model_pub_fs快照回传
//   GUI: refresh比对gen_fs -> 重建列表
// 批量操作走"操作泵": 多选后复制/剪切进剪贴板, 粘贴/删除把条目排进本地
// 操作环, refresh节拍里逐条投递FS_COPY/FS_DELETE(appctl队列16深, 泵限流
// 水6); 结果经gen_fsop累计计数对账, 全部完成后重列目录并toast汇总。
// 剪切粘贴串行对账(逐条确认复制成功才补发删源, 不会误删未复制走的源);
// 复制/删除批内在流水深度内并行。列表超出上限(GUI_FS_MAX)时提示截断。
// =============================================================================

#include <string.h>

#include "lvgl.h"

#include "sw/app/launcher.h"
#include "sw/app/bridge.h"
#include "sw/app/appctl.h"
#include "sw/impl/lcd/lvgl_port.h"

#define COL_CARD      0x141B2E
#define COL_CARD_EDGE 0x263050
#define COL_TEXT_DIM  0x8A94B8
#define COL_CYAN      0x00E5FF
#define COL_ACCENT    0x4FA8FF
#define COL_ROW_PRES  0x1C2946     //行按压底色
#define COL_ROW_SEL   0x16324F     //行选中底色
#define COL_RED       0xB3261E     //确认删除按钮
#define COL_RED_DIM   0x402028     //行内垃圾桶底色
#define COL_AMBER     0xFFC107     //警告图标/目录图标

#define ROOT_PATH     "0:/"

//操作批类型(s_batch)
enum { BATCH_NONE = 0, BATCH_DELETE, BATCH_PASTE };

//控件引用
static lv_obj_t* s_lbl_path;        //当前路径(超长DOT省略)
static lv_obj_t* s_lbl_state;       //读取中/空目录/错误提示
static lv_obj_t* s_list;            //滚动列表容器
static lv_obj_t* s_lbl_cnt;         //底部条数/截断提示行
static lv_obj_t* s_btn_up;          //上一级
static lv_obj_t* s_btn_sel;         //多选开关
static lv_obj_t* s_btn_paste;       //粘贴(剪贴板非空时显示)
static lv_obj_t* s_paste_ic;        //粘贴按钮图标(复制/剪切)
static lv_obj_t* s_paste_lbl;       //粘贴按钮文字(带条数)
static lv_obj_t* s_actionbar;       //多选浮动操作条
static lv_obj_t* s_lbl_selcnt;      //已选条数
static GuiSnapshot s_snap;          //最近一次快照(行点击时查条目)
static uint32_t   s_last_gen;       //已渲染的fs代数(0=从未渲染)
static char       s_cur[GUI_FS_PATH_MAX];
static int        s_cw;             //内容区宽(on_open算好, 惰性布局不可读坐标)

//---- 多选(按名对账, 重列/翻页不丢) ----
static int  s_sel_mode;
static char s_sel[GUI_FS_MAX][GUI_FS_NAME_MAX];
static int  s_sel_n;

//---- 剪贴板(应用会话内有效, 退出清空) ----
static char s_clip[GUI_FS_MAX][GUI_FS_PATH_MAX];
static int  s_clip_n;
static int  s_clip_cut;             //1=剪切(粘贴成功后删源)

//---- 行控件缓存(重列时填, 选中态原地更新用) ----
static lv_obj_t* s_row[GUI_FS_MAX];
static lv_obj_t* s_row_chk[GUI_FS_MAX];
static lv_obj_t* s_row_chkic[GUI_FS_MAX];

//---- 操作泵(refresh节拍驱动, 见文件头注释) ----
typedef struct {
    uint8_t is_copy;                //1=FS_COPY(src->dst) 0=FS_DELETE(src)
    uint8_t rm;                     //复制成功后补发删源(剪切粘贴)
    char    src[GUI_FS_PATH_MAX];
    char    dst[GUI_FS_PATH_MAX];
} FsOp;
#define OP_RING         (2 * GUI_FS_MAX)   //最坏: 全部复制+全部删源
#define OP_INFLIGHT_MAX 6                  //流水深度(appctl队列16留余量)
static FsOp     s_ops[OP_RING];
static int      s_op_head, s_op_cnt;
static int      s_inflight;
static FsOp     s_last_posted;      //串行模式下当前在途命令(结果归属)
static uint32_t s_seq_seen;         //已对账的fsop_seq
static uint32_t s_ok_seen, s_err_seen;
static int      s_res_ok, s_res_err;//本批成功/失败累计
static uint8_t  s_batch;            //BATCH_xxx
static int      s_batch_serial;     //1=逐条对账(剪切粘贴)
static int      s_src_cnt;          //本批条数(汇总文案用)

static lv_obj_t* s_confirm;         //确认框根(全屏遮罩, 一次一个)
static int       s_confirm_batch;   //1=批量删除确认 0=单条
static char      s_del_path[GUI_FS_PATH_MAX];   //单条删除待确认路径

// -----------------------------------------------------------------------------
// 大小格式化: B/KB/MB 自适应, buf>=16字节
static void fmt_size(char* buf, unsigned cap, uint32_t sz)
{
    if (sz >= 1024u * 1024u)
        lv_snprintf(buf, cap, "%u.%u MB", sz >> 20,
                    (unsigned)((sz & 0xFFFFFu) * 10u >> 20));
    else if (sz >= 1024u)
        lv_snprintf(buf, cap, "%u KB", (sz + 1023u) >> 10);
    else
        lv_snprintf(buf, cap, "%u B", sz);
}

// -----------------------------------------------------------------------------
// 拼子路径(root"0:/"不加分隔符): 返回0成功(缓冲不足-1)
static int path_join(const char* name, char* out, unsigned cap)
{
    unsigned len = (unsigned)strlen(s_cur);
    if (len + 1 >= cap)
        return -1;
    memcpy(out, s_cur, len);
    if (len > 0 && out[len - 1] != '/')
        out[len++] = '/';
    const unsigned rest = cap - 1 - len;
    const unsigned nl = strnlen(name, 28);
    if (nl > rest)
        return -1;
    memcpy(out + len, name, nl);
    out[len + nl] = 0;
    return 0;
}

//取路径末级名("0:/a/b.txt"->"b.txt")
static const char* path_name(const char* p)
{
    const char* s = strrchr(p, '/');
    return (s != 0) ? s + 1 : p;
}

//扩展名比较(不区分大小写, dot以'.'开头)
static int ext_eq(const char* dot, const char* ext)
{
    while (*dot != 0 && *ext != 0)
    {
        char a = *dot, b = *ext;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b)
            return 0;
        dot++;
        ext++;
    }
    return *dot == 0 && *ext == 0;
}

//文件类型图标(目录由调用方单独处理)
static const char* file_icon(const char* name, uint32_t* color)
{
    const char* dot = strrchr(name, '.');
    if (dot != 0)
    {
        if (ext_eq(dot, ".bmp") || ext_eq(dot, ".jpg") ||
            ext_eq(dot, ".jpeg"))
        {
            *color = 0x64B5F6;
            return LV_SYMBOL_IMAGE;
        }
        if (ext_eq(dot, ".mp3") || ext_eq(dot, ".wav"))
        {
            *color = 0xBA68C8;
            return LV_SYMBOL_AUDIO;
        }
        if (ext_eq(dot, ".mp4") || ext_eq(dot, ".avi"))
        {
            *color = 0x4DB6AC;
            return LV_SYMBOL_VIDEO;
        }
        if (ext_eq(dot, ".bin"))
        {
            *color = 0xFFB74D;
            return LV_SYMBOL_SAVE;
        }
    }
    *color = COL_TEXT_DIM;
    return LV_SYMBOL_FILE;
}

// -----------------------------------------------------------------------------
// 导航: 记录当前路径 + 命令进总线(结果由refresh经快照回来渲染)
static void sel_mode_set(int on);        //前置声明(navigate退出多选用)

static void navigate(const char* path)
{
    //离开本目录: 选中集合作废(选中名只对本目录有效, 否则会拼错路径误删)
    if (s_sel_mode)
        sel_mode_set(0);
    else
        s_sel_n = 0;

    strncpy(s_cur, path, sizeof(s_cur) - 1);
    s_cur[sizeof(s_cur) - 1] = 0;
    lv_label_set_text(s_lbl_path, s_cur);
    lv_obj_remove_flag(s_lbl_state, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_lbl_state, "读取中…");

    //根目录再上一级无意义: 上一级按钮置灰(与顶栏返回键拉开距离防误触)
    if (strcmp(s_cur, ROOT_PATH) == 0)
        lv_obj_add_state(s_btn_up, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(s_btn_up, LV_STATE_DISABLED);

    AppCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = APP_CMD_FS_LIST;
    strncpy(cmd.path, path, sizeof(cmd.path) - 1);
    appctl_post(&cmd);
}

// -----------------------------------------------------------------------------
// 多选集合(名字数组, 线性查找; 上限=GUI_FS_MAX)
static int sel_find(const char* name)
{
    for (int i = 0; i < s_sel_n; i++)
        if (strcmp(s_sel[i], name) == 0)
            return i;
    return -1;
}

static int sel_has(const char* name)
{
    return sel_find(name) >= 0;
}

static void sel_put(const char* name)
{
    if (s_sel_n >= GUI_FS_MAX || sel_find(name) >= 0)
        return;
    strncpy(s_sel[s_sel_n], name, sizeof(s_sel[0]) - 1);
    s_sel[s_sel_n][sizeof(s_sel[0]) - 1] = 0;
    s_sel_n++;
}

static void sel_del(const char* name)
{
    const int i = sel_find(name);
    if (i < 0)
        return;
    if (i != s_sel_n - 1)
        memcpy(s_sel[i], s_sel[s_sel_n - 1], sizeof(s_sel[0]));
    s_sel_n--;
}

static void sel_toggle(const char* name)
{
    if (sel_find(name) >= 0)
        sel_del(name);
    else
        sel_put(name);
}

//行选中态外观(复选框填充+行底色/描边)
static void row_apply_sel(int i)
{
    lv_obj_t* row = s_row[i];
    if (row == 0)
        return;
    const int on = sel_has(s_snap.fs[i].name);
    if (s_row_chk[i] != 0)
    {
        lv_obj_set_style_bg_opa(s_row_chk[i],
                                on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (s_row_chkic[i] != 0)
        {
            if (on)
                lv_obj_remove_flag(s_row_chkic[i], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(s_row_chkic[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_bg_color(row,
        lv_color_hex(on ? COL_ROW_SEL : COL_CARD), 0);
    lv_obj_set_style_border_color(row,
        lv_color_hex(on ? COL_ACCENT : COL_CARD_EDGE), 0);
}

static void sel_sync(void)
{
    lv_label_set_text_fmt(s_lbl_selcnt, "已选 %d 项", s_sel_n);
}

// -----------------------------------------------------------------------------
// 多选模式进出(重建列表: 复选框有无随模式变化)
static void sel_mode_set(int on)
{
    if (s_sel_mode == on)
        return;
    s_sel_mode = on;
    s_sel_n = 0;
    if (on)
        sel_sync();
    if (s_btn_sel != 0)
    {
        if (on)
            lv_obj_add_state(s_btn_sel, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(s_btn_sel, LV_STATE_CHECKED);
    }
    if (s_actionbar != 0)
    {
        if (on)
            lv_obj_remove_flag(s_actionbar, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_actionbar, LV_OBJ_FLAG_HIDDEN);
    }
    s_last_gen--;                    //强制重建(借用gen比对通道)
}

// -----------------------------------------------------------------------------
// 剪贴板/粘贴按钮
static void paste_btn_update(void)
{
    if (s_clip_n == 0)
    {
        lv_obj_add_flag(s_btn_paste, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_paste_ic,
                      s_clip_cut ? LV_SYMBOL_CUT : LV_SYMBOL_COPY);
    lv_label_set_text_fmt(s_paste_lbl, "粘贴 %d", s_clip_n);
    lv_obj_remove_flag(s_btn_paste, LV_OBJ_FLAG_HIDDEN);
}

// -----------------------------------------------------------------------------
// 行点击: 多选模式切换选中; 否则进目录/报文件大小
static void row_click_cb(lv_event_t* e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_snap.fs_n)
        return;
    const GuiFsEntry* en = &s_snap.fs[idx];

    if (s_sel_mode)
    {
        sel_toggle(en->name);
        row_apply_sel(idx);
        sel_sync();
        return;
    }

    if (en->is_dir)
    {
        char path[GUI_FS_PATH_MAX];
        if (path_join(en->name, path, sizeof(path)) == 0)
            navigate(path);
    }
    else
    {
        char sz[16];
        fmt_size(sz, sizeof(sz), en->size);
        char msg[GUI_FS_NAME_MAX + 24];
        lv_snprintf(msg, sizeof(msg), "%s  %s", en->name, sz);
        launcher_toast(msg);
    }
}

// -----------------------------------------------------------------------------
// 操作泵: 环形队列 -> appctl命令; 结果经gen_fsop计数对账
static void op_enqueue(int is_copy, int rm,
                       const char* src, const char* dst)
{
    if (s_op_cnt >= OP_RING)
        return;
    FsOp* op = &s_ops[(s_op_head + s_op_cnt) % OP_RING];
    op->is_copy = (uint8_t)is_copy;
    op->rm = (uint8_t)rm;
    strncpy(op->src, src, sizeof(op->src) - 1);
    op->src[sizeof(op->src) - 1] = 0;
    if (dst != 0)
    {
        strncpy(op->dst, dst, sizeof(op->dst) - 1);
        op->dst[sizeof(op->dst) - 1] = 0;
    }
    else
        op->dst[0] = 0;
    s_op_cnt++;
}

static void op_start(int batch_kind, int src_cnt, int serial,
                     const char* doing)
{
    s_batch = (uint8_t)batch_kind;
    s_batch_serial = serial;
    s_src_cnt = src_cnt;
    s_seq_seen = s_snap.fsop_seq;    //app_refresh已取本拍快照
    s_ok_seen = s_snap.fsop_ok;
    s_err_seen = s_snap.fsop_err;
    s_res_ok = 0;
    s_res_err = 0;
    s_inflight = 0;
    launcher_toast(doing);
}

//对账: fsop_seq推进量=完成条数; 剪切粘贴逐条确认后补发删源
static void op_account(void)
{
    const uint32_t d_seq = s_snap.fsop_seq - s_seq_seen;
    if (d_seq == 0)
        return;
    const int d_ok = (int)(s_snap.fsop_ok - s_ok_seen);
    s_res_ok += d_ok;
    s_res_err += (int)(s_snap.fsop_err - s_err_seen);
    s_seq_seen = s_snap.fsop_seq;
    s_ok_seen = s_snap.fsop_ok;
    s_err_seen = s_snap.fsop_err;
    s_inflight -= (int)d_seq;
    if (s_inflight < 0)
        s_inflight = 0;

    //剪切粘贴(串行): 刚完成的复制成功才补发删源
    if (s_batch == BATCH_PASTE && s_batch_serial && d_ok > 0 &&
        s_last_posted.is_copy && s_last_posted.rm)
        op_enqueue(0, 0, s_last_posted.src, 0);
}

static void op_pump(void)
{
    if (s_batch == BATCH_NONE)
        return;
    op_account();

    //投递(剪切粘贴串行=在途1; 其余流水至多6; 队列满下拍再发)
    const int limit = s_batch_serial ? 1 : OP_INFLIGHT_MAX;
    while (s_op_cnt > 0 && s_inflight < limit)
    {
        FsOp* op = &s_ops[s_op_head];
        AppCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        if (op->is_copy)
        {
            cmd.type = APP_CMD_FS_COPY;
            strncpy(cmd.path, op->src, sizeof(cmd.path) - 1);
            strncpy(cmd.path2, op->dst, sizeof(cmd.path2) - 1);
        }
        else
        {
            cmd.type = APP_CMD_FS_DELETE;
            strncpy(cmd.path, op->src, sizeof(cmd.path) - 1);
        }
        if (appctl_post(&cmd) != 0)
            break;                   //队列满: 等下一拍
        s_last_posted = *op;
        s_op_head = (s_op_head + 1) % OP_RING;
        s_op_cnt--;
        s_inflight++;
    }

    //收尾: 全部投递且全部对账 -> 汇总toast(长停留) + 重列当前目录
    if (s_op_cnt == 0 && s_inflight == 0)
    {
        char msg[40];
        lv_snprintf(msg, sizeof(msg), "%s 成功%d 失败%d",
                    s_batch == BATCH_PASTE ? "粘贴完成" : "删除完成",
                    s_res_ok, s_res_err);
        launcher_toast_long(msg);
        if (s_batch == BATCH_PASTE && s_clip_cut)
        {
            s_clip_n = 0;            //剪切语义: 粘贴后剪贴板清空
            paste_btn_update();
        }
        s_batch = BATCH_NONE;

        AppCmd lst;
        memset(&lst, 0, sizeof(lst));
        lst.type = APP_CMD_FS_LIST;
        strncpy(lst.path, s_cur, sizeof(lst.path) - 1);
        appctl_post(&lst);
    }
}

// -----------------------------------------------------------------------------
// 确认框(全屏遮罩+卡片): 文案由调用方给; 确认后按s_confirm_batch执行
static void del_ok_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (s_confirm_batch)
    {
        for (int i = 0; i < s_snap.fs_n && s_op_cnt < OP_RING; i++)
        {
            if (!sel_has(s_snap.fs[i].name))
                continue;
            char p[GUI_FS_PATH_MAX];
            if (path_join(s_snap.fs[i].name, p, sizeof(p)) == 0)
                op_enqueue(0, 0, p, 0);
        }
        op_start(BATCH_DELETE, s_sel_n, 0, "删除中…");
        sel_mode_set(0);
    }
    else
    {
        op_enqueue(0, 0, s_del_path, 0);
        op_start(BATCH_DELETE, 1, 0, "删除中…");
    }
    lv_obj_delete(s_confirm);
    s_confirm = 0;
}

static void confirm_close_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (s_confirm != 0)
    {
        lv_obj_delete(s_confirm);
        s_confirm = 0;
    }
}

//text=完整问句(一行或两行, 超宽自动换行, 按钮区独立不重叠)
static void confirm_open(const char* text)
{
    if (s_confirm != 0)
        return;
    lv_display_t* disp = lv_display_get_default();
    s_confirm = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_size(s_confirm, lv_display_get_horizontal_resolution(disp),
                    lv_display_get_vertical_resolution(disp));
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_60, 0);
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_confirm, confirm_close_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t* card = lv_obj_create(s_confirm);
    lv_obj_set_size(card, 460, 150);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    //问句区: 定宽+换行, 最多两行(16px字号, 388px宽), 底部按钮区固定不重叠
    lv_obj_t* ic = lv_label_create(card);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(COL_AMBER), 0);
    lv_label_set_text(ic, LV_SYMBOL_WARNING);
    lv_obj_set_pos(ic, 20, 18);

    lv_obj_t* q = lv_label_create(card);
    lv_obj_set_pos(q, 52, 18);
    lv_obj_set_width(q, 388);
    lv_obj_set_style_text_font(q, &font_cn_16, 0);
    lv_obj_set_style_text_color(q, lv_color_white(), 0);
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
    lv_label_set_text(q, text);

    lv_obj_t* ok = lv_button_create(card);
    lv_obj_set_size(ok, 132, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, -78, -16);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COL_RED), 0);
    lv_obj_t* ok_t = lv_label_create(ok);
    lv_obj_set_style_text_font(ok_t, &font_cn_16, 0);
    lv_label_set_text(ok_t, "删除");
    lv_obj_center(ok_t);
    lv_obj_add_event_cb(ok, del_ok_cb, LV_EVENT_CLICKED, 0);

    lv_obj_t* cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 132, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 78, -16);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x2A3450), 0);
    lv_obj_t* cc_t = lv_label_create(cancel);
    lv_obj_set_style_text_font(cc_t, &font_cn_16, 0);
    lv_label_set_text(cc_t, "取消");
    lv_obj_center(cc_t);
    lv_obj_add_event_cb(cancel, confirm_close_cb, LV_EVENT_CLICKED, 0);
}

//行内垃圾桶(普通模式): 单条删除确认
static void del_click_cb(lv_event_t* e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_snap.fs_n || s_confirm != 0)
        return;
    if (s_batch != BATCH_NONE)
    {
        launcher_toast("等待完成");
        return;
    }
    if (path_join(s_snap.fs[idx].name, s_del_path,
                  sizeof(s_del_path)) != 0)
        return;
    s_confirm_batch = 0;

    char txt[GUI_FS_NAME_MAX + 48];
    if (s_snap.fs[idx].is_dir)
        lv_snprintf(txt, sizeof(txt), "删除目录 %s ?(文件随目录删除)",
                    s_snap.fs[idx].name);
    else
        lv_snprintf(txt, sizeof(txt), "删除 %s ?",
                    s_snap.fs[idx].name);
    confirm_open(txt);
}

//操作条删除(多选): 批量删除确认
static void bat_del_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (s_confirm != 0 || s_sel_n == 0)
        return;
    if (s_batch != BATCH_NONE)
    {
        launcher_toast("等待完成");
        return;
    }
    s_confirm_batch = 1;
    char txt[24];
    lv_snprintf(txt, sizeof(txt), "删除 %d 项 ?", s_sel_n);
    confirm_open(txt);
}

// -----------------------------------------------------------------------------
// 上一级/多选/粘贴
static void btn_up_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    //上一级: 去掉末级目录("0:/"为根, 按钮已置灰双保险)
    char path[GUI_FS_PATH_MAX];
    strncpy(path, s_cur, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    unsigned len = (unsigned)strlen(path);
    while (len > 0 && path[len - 1] == '/')
        path[--len] = 0;                     //去尾部'/'
    char* slash = strchr(path, '/');
    if (len == 0 || slash == 0)
    {
        navigate(ROOT_PATH);
        return;
    }
    if (strchr(slash + 1, '/') == 0)
        strcpy(path, ROOT_PATH);             //只剩一级 => 根
    else
    {
        char* last = strrchr(path, '/');
        *last = 0;
        strcat(path, "/");                   //保留为目录路径
    }
    navigate(path);
}

static void btn_sel_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    sel_mode_set(s_sel_mode ? 0 : 1);
}

//复制/剪切进剪贴板(只收文件; 目录计数跳过并提示)
static void clip_stash(int cut)
{
    if (s_sel_n == 0)
        return;
    if (s_batch != BATCH_NONE)
    {
        launcher_toast("等待完成");
        return;
    }
    int files = 0, dirs = 0;
    s_clip_n = 0;
    s_clip_cut = cut;
    for (int i = 0; i < s_snap.fs_n && s_clip_n < GUI_FS_MAX; i++)
    {
        if (!sel_has(s_snap.fs[i].name))
            continue;
        if (s_snap.fs[i].is_dir)
        {
            dirs++;                          //目录不支持批量搬运, 跳过
            continue;
        }
        char p[GUI_FS_PATH_MAX];
        if (path_join(s_snap.fs[i].name, p, sizeof(p)) != 0)
            continue;
        strncpy(s_clip[s_clip_n], p, sizeof(s_clip[0]) - 1);
        s_clip[s_clip_n][sizeof(s_clip[0]) - 1] = 0;
        s_clip_n++;
        files++;
    }
    sel_mode_set(0);
    paste_btn_update();

    char msg[32];
    lv_snprintf(msg, sizeof(msg), "已%s %d 项",
                cut ? "剪切" : "复制", files);
    if (files > 0)
        launcher_toast_long(msg);
    if (dirs > 0)
    {
        lv_snprintf(msg, sizeof(msg), "%d个目录未%s",
                    dirs, cut ? "剪切" : "复制");
        launcher_toast_long(msg);
    }
}

static void clip_copy_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    clip_stash(0);
}

static void clip_cut_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    clip_stash(1);
}

static void paste_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    if (s_clip_n == 0)
        return;
    if (s_batch != BATCH_NONE)
    {
        launcher_toast("等待完成");
        return;
    }
    int enq = 0;
    for (int i = 0; i < s_clip_n; i++)
    {
        char dst[GUI_FS_PATH_MAX];
        //同目录剪切: 复制会被后端拒绝(自拷), 计入失败, 源不受影响
        if (path_join(path_name(s_clip[i]), dst, sizeof(dst)) != 0)
            continue;
        op_enqueue(1, s_clip_cut, s_clip[i], dst);
        enq++;
    }
    if (enq == 0)
        return;
    op_start(BATCH_PASTE, enq, s_clip_cut, "粘贴中…");
}

//操作条: 全选/反选/取消
static void sel_all_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    s_sel_n = 0;
    for (int i = 0; i < s_snap.fs_n; i++)
        sel_put(s_snap.fs[i].name);
    for (int i = 0; i < s_snap.fs_n; i++)
        row_apply_sel(i);
    sel_sync();
}

static void sel_inv_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    for (int i = 0; i < s_snap.fs_n; i++)
        sel_toggle(s_snap.fs[i].name);
    for (int i = 0; i < s_snap.fs_n; i++)
        row_apply_sel(i);
    sel_sync();
}

static void sel_cancel_cb(lv_event_t* e)
{
    LV_UNUSED(e);
    sel_mode_set(0);
}

// -----------------------------------------------------------------------------
static lv_obj_t* place(lv_obj_t* parent, int x, int y, int w, int h)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

//工具栏按钮(卡片底+描边, 图标+文字; ic_out/lbl_out可取回子标签, 不需要传0)
static lv_obj_t* tool_btn(lv_obj_t* parent, int x, int w, int h,
                          const char* icon, const char* text,
                          lv_event_cb_t cb, intptr_t ud,
                          lv_obj_t** ic_out, lv_obj_t** lbl_out)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, 2);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 5, 0);
    if (icon != 0)
    {
        lv_obj_t* ic = lv_label_create(b);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(COL_ACCENT), 0);
        lv_label_set_text(ic, icon);
        if (ic_out != 0)
            *ic_out = ic;
    }
    lv_obj_t* lb = lv_label_create(b);
    lv_obj_set_style_text_font(lb, &font_cn_16, 0);
    lv_label_set_text(lb, text);
    if (lbl_out != 0)
        *lbl_out = lb;
    if (cb != 0)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)ud);
    return b;
}

// -----------------------------------------------------------------------------
// gen_fs变化/多选进出 => 重建列表(清空滚动容器, 按快照建行)
static void build_rows(void)
{
    lv_obj_clean(s_list);

    if (s_snap.fs_err != 0)
    {
        lv_label_set_text(s_lbl_state,
                          s_snap.fs_err == -1 ? "SD卡未挂载" : "目录打开失败");
        lv_obj_remove_flag(s_lbl_state, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_cnt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (s_snap.fs_n == 0)
    {
        lv_label_set_text(s_lbl_state, "空目录");
        lv_obj_remove_flag(s_lbl_state, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_cnt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_lbl_state, LV_OBJ_FLAG_HIDDEN);

    const int cw = s_cw;                 //与on_open同源(惰性布局不可读坐标)
    for (int i = 0; i < s_snap.fs_n; i++)
    {
        const GuiFsEntry* en = &s_snap.fs[i];

        lv_obj_t* row = lv_button_create(s_list);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_CARD_EDGE), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_ROW_PRES),
                                  LV_STATE_PRESSED);
        s_row[i] = row;
        s_row_chk[i] = 0;
        s_row_chkic[i] = 0;
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);

        //复选框(仅多选模式): 点击落在行上统一处理
        int tx = 12;
        if (s_sel_mode)
        {
            lv_obj_t* chk = lv_obj_create(row);
            lv_obj_remove_style_all(chk);
            lv_obj_set_size(chk, 22, 22);
            lv_obj_align(chk, LV_ALIGN_LEFT_MID, 8, 0);
            lv_obj_set_style_radius(chk, 6, 0);
            lv_obj_set_style_bg_color(chk, lv_color_hex(COL_CYAN), 0);
            lv_obj_set_style_bg_opa(chk, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(chk, lv_color_hex(0x55628A), 0);
            lv_obj_set_style_border_width(chk, 2, 0);
            lv_obj_remove_flag(chk, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* ok = lv_label_create(chk);
            lv_obj_set_style_text_font(ok, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(ok, lv_color_hex(0x082038), 0);
            lv_label_set_text(ok, LV_SYMBOL_OK);
            lv_obj_center(ok);
            lv_obj_add_flag(ok, LV_OBJ_FLAG_HIDDEN);
            s_row_chk[i] = chk;
            s_row_chkic[i] = ok;
            tx = 42;
        }

        //类型图标: 目录琥珀/图片/音视频/可执行/普通文件
        uint32_t icol;
        const char* isym = en->is_dir ? LV_SYMBOL_DIRECTORY
                                      : file_icon(en->name, &icol);
        if (en->is_dir)
            icol = COL_AMBER;
        lv_obj_t* ic = lv_label_create(row);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(icol), 0);
        lv_label_set_text(ic, isym);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, tx, 0);

        //文件名(右侧预留: 大小84+垃圾桶38+间距)
        lv_obj_t* nm = lv_label_create(row);
        lv_obj_set_pos(nm, tx + 26, 0);
        lv_obj_set_height(nm, lv_pct(100));
        lv_obj_set_width(nm, cw - (tx + 26) - 138);
        lv_obj_set_style_text_font(nm, &font_cn_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_white(), 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, en->name);

        //大小
        lv_obj_t* sz = lv_label_create(row);
        lv_obj_set_width(sz, 84);
        lv_obj_set_style_text_align(sz, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(sz, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sz, lv_color_hex(COL_TEXT_DIM), 0);
        if (en->is_dir)
            lv_label_set_text(sz, "<dir>");
        else
        {
            char buf[16];
            fmt_size(buf, sizeof(buf), en->size);
            lv_label_set_text(sz, buf);
        }
        lv_obj_align(sz, LV_ALIGN_RIGHT_MID, -48, 0);

        //垃圾桶(普通模式; 多选下由操作条统一删除)
        if (!s_sel_mode)
        {
            lv_obj_t* del = lv_button_create(row);
            lv_obj_set_size(del, 34, 34);
            lv_obj_align(del, LV_ALIGN_RIGHT_MID, -4, 0);
            lv_obj_set_style_bg_color(del, lv_color_hex(COL_RED_DIM), 0);
            lv_obj_set_style_border_width(del, 0, 0);
            lv_obj_set_style_radius(del, 8, 0);
            lv_obj_set_style_shadow_width(del, 0, 0);
            lv_obj_t* di = lv_label_create(del);
            lv_obj_set_style_text_font(di, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(di, lv_color_hex(0xFF8A80), 0);
            lv_label_set_text(di, LV_SYMBOL_TRASH);
            lv_obj_center(di);
            lv_obj_add_event_cb(del, del_click_cb, LV_EVENT_CLICKED,
                                (void*)(intptr_t)i);
        }

        row_apply_sel(i);                    //恢复选中态(重列不丢)
    }

    //底部条数/截断提示
    if (s_snap.fs_truncated)
        lv_label_set_text(s_lbl_cnt, "只列前64个");
    else
        lv_label_set_text_fmt(s_lbl_cnt, "%d 项", s_snap.fs_n);
    lv_obj_remove_flag(s_lbl_cnt, LV_OBJ_FLAG_HIDDEN);
}

static void app_on_open(lv_obj_t* parent)
{
    //根节点: 内容区几何(launcher应用屏: 顶栏40+左右边距8), 800x480=>784x424
    //尺寸取显示分辨率计算(不可读parent坐标: LVGL惰性布局, 此刻为0)
    const int cw = lv_display_get_horizontal_resolution(
                       lv_display_get_default()) - 16;
    const int ch = lv_display_get_vertical_resolution(
                       lv_display_get_default()) - 40 - 16;
    s_cw = cw;
    lv_obj_t* root = place(parent, 0, 0, cw, ch);

    //---- 状态复位(静态量跨会话存续, 进应用一律清) ----
    s_sel_mode = 0;
    s_sel_n = 0;
    s_clip_n = 0;
    s_clip_cut = 0;
    s_op_head = s_op_cnt = s_inflight = 0;
    s_batch = BATCH_NONE;
    s_confirm = 0;

    //---- 顶栏工具行: 路径居左, 操作钮居右(与顶栏"返回"拉开距离防误触) ----
    //几何自右向左排布; 粘贴按钮隐藏时占位不变, 邻钮位置稳定
    const int x_paste = cw - 100;
    const int x_selbt = x_paste - 8 - 84;
    const int x_up    = x_selbt - 8 - 92;

    lv_obj_t* drv = lv_label_create(root);
    lv_obj_set_style_text_font(drv, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(drv, lv_color_hex(COL_ACCENT), 0);
    lv_label_set_text(drv, LV_SYMBOL_DRIVE);
    lv_obj_set_pos(drv, 8, 11);

    s_lbl_path = lv_label_create(root);
    lv_obj_set_pos(s_lbl_path, 30, 12);
    lv_obj_set_width(s_lbl_path, x_up - 8 - 30);
    lv_obj_set_style_text_font(s_lbl_path, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_path, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_long_mode(s_lbl_path, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_path, ROOT_PATH);

    s_btn_up = tool_btn(root, x_up, 92, 34, LV_SYMBOL_LEFT, "上一级",
                        btn_up_cb, 0, 0, 0);
    s_btn_sel = tool_btn(root, x_selbt, 84, 34, LV_SYMBOL_LIST, "选择",
                         btn_sel_cb, 0, 0, 0);
    //多选进行中的按钮态(蓝底深字)
    lv_obj_set_style_bg_color(s_btn_sel, lv_color_hex(COL_ACCENT),
                              LV_STATE_CHECKED);
    s_btn_paste = tool_btn(root, x_paste, 100, 34, LV_SYMBOL_COPY, "粘贴",
                           paste_cb, 0, &s_paste_ic, &s_paste_lbl);
    lv_obj_add_flag(s_btn_paste, LV_OBJ_FLAG_HIDDEN);

    //---- 状态行(读取中/空/错误): 列表区居中 ----
    s_lbl_state = lv_label_create(root);
    lv_obj_set_style_text_font(s_lbl_state, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_state, "读取中…");
    lv_obj_align(s_lbl_state, LV_ALIGN_CENTER, 0, 10);

    //---- 滚动列表(工具栏之下, 底部提示行之上) ----
    s_list = lv_obj_create(root);
    lv_obj_set_pos(s_list, 0, 42);
    lv_obj_set_size(s_list, cw, ch - 42 - 22);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    //---- 底部条数/截断提示 ----
    s_lbl_cnt = lv_label_create(root);
    lv_obj_set_pos(s_lbl_cnt, 8, ch - 20);
    lv_obj_set_style_text_font(s_lbl_cnt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_cnt, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(s_lbl_cnt, "");

    //---- 多选浮动操作条(默认隐藏) ----
    s_actionbar = lv_obj_create(root);
    lv_obj_remove_style_all(s_actionbar);
    lv_obj_set_size(s_actionbar, 640, 52);
    lv_obj_align(s_actionbar, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(s_actionbar, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_actionbar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_actionbar, 14, 0);
    lv_obj_set_style_border_color(s_actionbar, lv_color_hex(COL_CARD_EDGE), 0);
    lv_obj_set_style_border_width(s_actionbar, 1, 0);
    lv_obj_set_style_shadow_color(s_actionbar, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(s_actionbar, 30, 0);
    lv_obj_set_style_shadow_opa(s_actionbar, LV_OPA_50, 0);
    lv_obj_clear_flag(s_actionbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_actionbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_actionbar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_actionbar, 8, 0);

    s_lbl_selcnt = lv_label_create(s_actionbar);
    lv_obj_set_style_text_font(s_lbl_selcnt, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_lbl_selcnt, lv_color_hex(COL_CYAN), 0);
    lv_label_set_text(s_lbl_selcnt, "已选 0 项");

    tool_btn(s_actionbar, 0, 72, 36, 0, "全选", sel_all_cb, 0, 0, 0);
    tool_btn(s_actionbar, 0, 72, 36, 0, "反选", sel_inv_cb, 0, 0, 0);
    tool_btn(s_actionbar, 0, 72, 36, LV_SYMBOL_COPY, "复制", clip_copy_cb,
             0, 0, 0);
    tool_btn(s_actionbar, 0, 72, 36, LV_SYMBOL_CUT, "剪切", clip_cut_cb,
             0, 0, 0);
    tool_btn(s_actionbar, 0, 88, 36, LV_SYMBOL_TRASH, "删除", bat_del_cb,
             0, 0, 0);
    tool_btn(s_actionbar, 0, 76, 36, 0, "取消", sel_cancel_cb, 0, 0, 0);
    lv_obj_add_flag(s_actionbar, LV_OBJ_FLAG_HIDDEN);

    s_last_gen = 0;
    s_cur[0] = 0;
    navigate(ROOT_PATH);
}

static void app_on_close(void)
{
    //确认框挂在顶层layer(不属于应用屏, 框架退场不删): 必须自行清理,
    //否则会留一块遮罩挡住桌面
    if (s_confirm)
    {
        lv_obj_delete(s_confirm);
        s_confirm = 0;
    }
    //批量操作态一并作废(已入队命令由app_task照常执行完, 只是无人toast)
    s_sel_mode = 0;
    s_sel_n = 0;
    s_clip_n = 0;
    s_op_head = s_op_cnt = s_inflight = 0;
    s_batch = BATCH_NONE;
}

// -----------------------------------------------------------------------------
// gen_fs变化 => 重建列表; 操作泵每拍先对账推进
static void app_refresh(void)
{
    gui_model_snapshot(&s_snap);
    op_pump();
    if (s_snap.gen_fs == s_last_gen)
        return;
    s_last_gen = s_snap.gen_fs;

    //路径校验: 丢弃过期回包(快速连点时旧目录结果晚到)
    if (strcmp(s_snap.fs_path, s_cur) != 0)
        return;

    build_rows();
}

// -----------------------------------------------------------------------------
const LauncherApp app_files = {
    .name       = "文件",
    .icon       = LV_SYMBOL_DRIVE,
    .on_open    = app_on_open,
    .on_close   = app_on_close,
    .refresh    = app_refresh,
    .refresh_ms = 150,
    .accent    = 0x4FA8FF,
};

// =============================================================================
// 后端(app_task上下文): 代GUI读SD目录/复制/删除 —— FatFs纪律执行侧,
// 上方UI经appctl命令进入app_files_dispatch, 结果经bridge的fs组回传,
// 操作成败经gui_model_pub_fsop累加(gen_fsop对账)。
// 相册(相机应用)也复用FS_LIST管道, 路径区分。
// =============================================================================

#include "xil_printf.h"
#include "sw/impl/sd/sd.h"
#include "sw/app/int_app/app_files.h"

void app_files_dispatch(const AppCmd* cmd)
{
    switch (cmd->type)
    {
    case APP_CMD_FS_LIST:
    {
        //代GUI读目录(唯一FatFs使用者)
        //sd层条目转bridge条目(两层类型独立, 不做硬投射)
        static SdDirEntry sd_entries[GUI_FS_MAX];
        int n = 0, trunc = 0;
        const int err = sd_list_dir(cmd->path, sd_entries, GUI_FS_MAX,
                                    &n, &trunc);
        static GuiFsEntry entries[GUI_FS_MAX];
        for (int i = 0; i < n; i++)
        {
            //名字已被sd层截断补0(<=27字节), 直接整串拷
            const size_t len = strlen(sd_entries[i].name) + 1;
            memcpy(entries[i].name, sd_entries[i].name, len);
            entries[i].is_dir = sd_entries[i].is_dir;
            entries[i].size = sd_entries[i].size;
        }
        gui_model_pub_fs(cmd->path, err, trunc, entries, n);
        break;
    }
    case APP_CMD_FS_DELETE:
    {
        //删除带防呆: 必须在SD内且至少一级名字(禁止根/空)
        int ok = 0;
        if (strncmp(cmd->path, "0:/", 3) == 0 && strlen(cmd->path) > 3)
            ok = (sd_delete(cmd->path) == 0);
        else
            xil_printf("[fs] delete rejected: %s\r\n", cmd->path);
        xil_printf("[fs] delete %s: %s\r\n", cmd->path,
                   ok ? "ok" : "failed");
        gui_model_pub_fsop(ok);
        break;
    }
    case APP_CMD_FS_COPY:
    {
        //复制带防呆: 源/目标都必须在SD内且至少一级名字
        int ok = 0;
        if (strncmp(cmd->path, "0:/", 3) == 0 && strlen(cmd->path) > 3 &&
            strncmp(cmd->path2, "0:/", 3) == 0 && strlen(cmd->path2) > 3)
            ok = (sd_copy_file(cmd->path, cmd->path2) == 0);
        else
            xil_printf("[fs] copy rejected: %s -> %s\r\n",
                       cmd->path, cmd->path2);
        gui_model_pub_fsop(ok);
        break;
    }
    default:
        break;
    }
}
