// =============================================================================
// cam —— 摄像头帧访问(实现层, 板卡)
//
// 链路启停归hw/chain/cam_chain(可选子系统, 应用控制);
// 本模块在链路运行期间提供帧边界抓取/最近帧快照/检测框叠加
//
// 帧语义: VDMA1三帧环自由跑, 写指针翻转即帧边界; 刚写完的帧在
// 被重新覆盖前有约两帧时间(~66ms@30fps)供读取
// =============================================================================

#ifndef SW_IMPL_CAM_CAM_H
#define SW_IMPL_CAM_CAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//检测框描述(坐标为帧内像素, RGB888)
typedef struct {
    int16_t x0, y0, x1, y1;
    uint8_t r, g, b;
} CamOverlayRect;

//同步等帧边界, 把刚写完的完整帧拷到dst(含cache失效), 返回帧环索引/-1失败
//idle: 等待帧边界期间的回调(可空, 如让出CPU)
int cam_grab_frame(uint8_t* dst, void (*idle)(void));

//零拷贝取"最近写完"的帧地址(不失效cache, 调用方自行按行失效并在
//~66ms内读完); 返回帧环索引/-1未启动
int cam_snap_latest(const uint8_t** frame);

//在下一个帧边界把检测框描到该帧上(2px描边, 先失效再改再冲刷),
//timeout_ms超时放弃; 返回0成功/-1失败或超时
int cam_draw_overlays(const CamOverlayRect* rects, int n, unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* SW_IMPL_CAM_CAM_H */
