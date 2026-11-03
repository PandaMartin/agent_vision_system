// =============================================================================
// gui_model —— 业务数据桥实现
// =============================================================================

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "sw/app/bridge.h"

//内部可变状态(发布方写, 快照时整体拷出)
static struct {
    SemaphoreHandle_t mtx;
    GuiSnapshot   s;                       //对外快照的原件
    u32           ms_sum;                  //耗时环累加和(算均值)
    u16           ms_ring[GUI_HIST_N];     //耗时环(与s.hist同步, 方便滚动)
    int           ring_pos;
    int           ring_n;
} g;

//字符串安全拷贝(截断补0)
static void copy_str(char* dst, const char* src)
{
    if (src == 0) src = "";
    strncpy(dst, src, GUI_LBL_MAX - 1);
    dst[GUI_LBL_MAX - 1] = 0;
}

int gui_model_init(void)
{
    memset(&g, 0, sizeof(g));
    g.s.model_idx = -1;      //未激活≠索引0(否则与第一个模型撞车, 界面误判)
    g.mtx = xSemaphoreCreateMutex();
    return g.mtx != 0 ? 0 : -1;
}

//---- 发布侧(app_task) ----

void gui_model_pub_boot(int sd_ok, int n_models,
                        int scr_id, int scr_w, int scr_h, int touch_id)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.sd_ok = sd_ok;
    g.s.scr_id = scr_id; g.s.scr_w = scr_w; g.s.scr_h = scr_h;
    g.s.touch_id = touch_id;
    g.s.n_models = n_models;
    g.s.gen_boot++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_cam(int on, int hdmi)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.cam_on = on;
    g.s.cam_hdmi = hdmi;
    g.s.gen_cam++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_run(int running, unsigned cycle)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.running = running;
    g.s.cycle = cycle;
    g.s.gen_run++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_infer(unsigned ms)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.infer_ms = ms;
    if (g.ring_n < GUI_HIST_N)
        g.ring_n++;                            //环未满: 直接累计
    else
        g.ms_sum -= g.ms_ring[g.ring_pos];     //环已满: 先减掉被覆盖的旧值
    g.ms_ring[g.ring_pos] = (u16)ms;
    g.ms_sum += ms;
    g.ring_pos = (g.ring_pos + 1) % GUI_HIST_N;

    //展开成快照里的"旧->新"直序, 页面直接整段喂图表
    int n = g.ring_n;
    g.s.hist_n = n;
    for (int i = 0; i < n; i++)
        g.s.hist[i] = g.ms_ring[(g.ring_pos - n + i + GUI_HIST_N) % GUI_HIST_N];

    g.s.avg_ms = g.ms_sum / (unsigned)n;
    g.s.peak_ms = 0;
    for (int i = 0; i < n; i++)
        if (g.ms_ring[i] > g.s.peak_ms)
            g.s.peak_ms = g.ms_ring[i];
    g.s.gen_infer++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_classify(int n, const GuiTopKItem* items)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.post_type = 0;
    if (n > GUI_TOPK_MAX) n = GUI_TOPK_MAX;
    g.s.topk_n = n;
    for (int i = 0; i < n; i++)
    {
        copy_str(g.s.topk[i].name, items[i].name);
        g.s.topk[i].score = items[i].score;
    }
    g.s.gen_res++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_detect(int n_cls, int total, const GuiDetCls* cls)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.post_type = 1;
    if (n_cls > GUI_DET_CLS_MAX) n_cls = GUI_DET_CLS_MAX;
    g.s.det_n_cls = n_cls;
    g.s.det_total = total;
    for (int i = 0; i < n_cls; i++)
    {
        copy_str(g.s.det_cls[i].name, cls[i].name);
        g.s.det_cls[i].count = cls[i].count;
        g.s.det_cls[i].best = cls[i].best;
    }
    g.s.gen_res++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_model(int idx, const char* name, const char* type)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.model_idx = idx;
    copy_str(g.s.model_name, name);
    copy_str(g.s.model_type, type);
    g.s.gen_model++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_capture(int ok, const char* file)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.cap_ok = ok;
    copy_str(g.s.cap_file, file);
    g.s.gen_cap++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_photo(void* pix, int w, int h, const char* name)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.photo_pix = pix;
    g.s.photo_w = w;
    g.s.photo_h = h;
    copy_str(g.s.photo_name, name);
    g.s.gen_photo++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_fs(const char* path, int err, int truncated,
                      const GuiFsEntry* entries, int n)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    if (path == 0) path = "";
    strncpy(g.s.fs_path, path, sizeof(g.s.fs_path) - 1);
    g.s.fs_path[sizeof(g.s.fs_path) - 1] = 0;
    g.s.fs_err = err;
    g.s.fs_truncated = truncated;
    if (n > GUI_FS_MAX) n = GUI_FS_MAX;
    g.s.fs_n = n;
    if (entries != 0 && err == 0)
        memcpy(g.s.fs, entries, (size_t)n * sizeof(GuiFsEntry));
    g.s.gen_fs++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_fsop(int ok)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.fsop_seq++;
    if (ok)
        g.s.fsop_ok++;
    else
        g.s.fsop_err++;
    g.s.gen_fsop++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_sdapps(const char (*names)[16], const char (*files)[32], int n)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    if (n > GUI_SDAPP_MAX) n = GUI_SDAPP_MAX;
    g.s.sdapp_n = n;
    for (int i = 0; i < n; i++)
    {
        strncpy(g.s.sdapp_name[i], names[i], sizeof(g.s.sdapp_name[i]) - 1);
        g.s.sdapp_name[i][sizeof(g.s.sdapp_name[i]) - 1] = 0;
        strncpy(g.s.sdapp_file[i], files[i], sizeof(g.s.sdapp_file[i]) - 1);
        g.s.sdapp_file[i][sizeof(g.s.sdapp_file[i]) - 1] = 0;
    }
    g.s.gen_sdapp++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_app_state(int st, const char* err)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.app_state = st;
    if (err != 0)
    {
        strncpy(g.s.app_err, err, sizeof(g.s.app_err) - 1);
        g.s.app_err[sizeof(g.s.app_err) - 1] = 0;
    }
    else
    {
        g.s.app_err[0] = 0;
    }
    g.s.gen_app++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_vid(unsigned fps, unsigned us)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.vid_fps = fps;
    g.s.vid_us = us;
    g.s.gen_vid++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_models(const char (*names)[GUI_LBL_MAX],
                          const char (*tags)[16], int n)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    if (n > GUI_MODEL_MAX) n = GUI_MODEL_MAX;
    g.s.n_models = n;
    for (int i = 0; i < n; i++)
    {
        strncpy(g.s.models_name[i], names[i], GUI_LBL_MAX - 1);
        g.s.models_name[i][GUI_LBL_MAX - 1] = 0;
        if (tags != 0)
        {
            strncpy(g.s.models_tag[i], tags[i], sizeof(g.s.models_tag[i]) - 1);
            g.s.models_tag[i][sizeof(g.s.models_tag[i]) - 1] = 0;
        }
        else
        {
            g.s.models_tag[i][0] = 0;
        }
    }
    g.s.gen_models++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_det(int src_w, int src_h,
                       const GuiDetBox* boxes, int n)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.det_src_w = src_w;
    g.s.det_src_h = src_h;
    if (n < 0) n = 0;
    if (n > GUI_DET_BOX_MAX) n = GUI_DET_BOX_MAX;
    g.s.det_box_n = n;
    if (boxes != 0 && n > 0)
        memcpy(g.s.det_box, boxes, (size_t)n * sizeof(GuiDetBox));
    g.s.gen_det++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_net(int up, const char* ip)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.net_up = up;
    if (ip == 0) ip = "";
    strncpy(g.s.net_ip, ip, sizeof(g.s.net_ip) - 1);
    g.s.net_ip[sizeof(g.s.net_ip) - 1] = 0;
    g.s.gen_net++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_wallpaper(void* pix)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.wall_pix = pix;                    //所有权过户: gui常驻引用, 不释放
    g.s.gen_wall++;
    xSemaphoreGive(g.mtx);
}

void gui_model_pub_xfer(int state, const char* file,
                        uint32_t done, uint32_t total, const char* msg)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    g.s.xfer_state = state;
    if (file == 0) file = "";
    strncpy(g.s.xfer_file, file, sizeof(g.s.xfer_file) - 1);
    g.s.xfer_file[sizeof(g.s.xfer_file) - 1] = 0;
    if (state == XFER_CONN)             //对端地址: 记一次, 整个会话(含RX)保留
    {
        strncpy(g.s.xfer_peer, file, sizeof(g.s.xfer_peer) - 1);
        g.s.xfer_peer[sizeof(g.s.xfer_peer) - 1] = 0;
    }
    if (state == XFER_DONE || state == XFER_ERR)   //结果区: 不随后续事件清除
    {
        g.s.xfer_res = (state == XFER_DONE) ? 1 : 2;
        g.s.xfer_res_gen++;                //结果代数: UI据此不漏任何一次结果
        strncpy(g.s.xfer_res_name, file, sizeof(g.s.xfer_res_name) - 1);
        g.s.xfer_res_name[sizeof(g.s.xfer_res_name) - 1] = 0;
        if (msg == 0) msg = "";
        strncpy(g.s.xfer_res_msg, msg, sizeof(g.s.xfer_res_msg) - 1);
        g.s.xfer_res_msg[sizeof(g.s.xfer_res_msg) - 1] = 0;
    }
    g.s.xfer_done = done;
    g.s.xfer_total = total;
    if (msg == 0) msg = "";
    strncpy(g.s.xfer_msg, msg, sizeof(g.s.xfer_msg) - 1);
    g.s.xfer_msg[sizeof(g.s.xfer_msg) - 1] = 0;
    g.s.gen_xfer++;
    xSemaphoreGive(g.mtx);
}

//---- 消费侧(gui任务) ----

void gui_model_snapshot(GuiSnapshot* out)
{
    xSemaphoreTake(g.mtx, portMAX_DELAY);
    *out = g.s;
    xSemaphoreGive(g.mtx);
}

uint32_t gui_model_det_gen_fast(void)
{
    return g.s.gen_det;      //单写者(app_task)下的对齐u32读, 原子可见
}
