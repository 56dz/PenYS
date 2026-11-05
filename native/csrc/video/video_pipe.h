/*
 * PenYS video pipe: appsrc -> h264parse -> mppvideodec -> kmssink
 *                    appsrc -> rtpopusdepay/opusdec -> audioconvert -> alsasink
 *
 * 契约面向 session.c（唯一调用方），全部函数在 worker 线程调用，模块自身
 * 不创建任何线程。GStreamer 内部的 streaming 线程由 GStreamer 拥有，不算在内。
 *
 * 真机已验证的约束（改代码前务必读一遍，每条都踩过坑）：
 *   1. kmssink 必须显式 driver-name=rockchip + plane-id=76。本机有两张 DRM
 *      card，card1 是 RKNPU 不是显示设备；不指定时 kmssink 可能挑错卡，
 *      管线永远进不了 PLAYING，push 会静默卡死在 appsrc 反压里。
 *   2. rtph264depay 和 h264parse 之间不要插 capsfilter。depay 输出
 *      stream-format=avc + codec_data，h264parse 需要从 codec_data 里取
 *      SPS/PPS 才能补出 width/height；改成 byte-stream 会丢 codec_data，
 *      解码器收到满量 buffer 但输出 0 帧。
 *   3. kmssink 的 max-lateness 单位是**纳秒**。直接传 40 等于 40ns，
 *      所有帧都被判 late 丢弃，sink 收到 0 个 buffer。
 *   4. render-rectangle 是只写属性，且单向棘轮：缩到很小之后无法放大，
 *      只能重建管线。yset_video_set_rect 因此按「只允许缩小」实现。
 *   5. /sys/kernel/debug/dri/0/summary 是 debugfs seq_file，单次 read 只回
 *      576 字节，必须循环 read 到 EOF。判 ACTIVE 要两条同时成立：窗口头行
 *      含 ACTIVE，且该段内 buf[0]: addr: 非 0。
 *   6. 熄屏（hal-screen off）会让内核 disable CRTC，平面停止提交，这是
 *      “屏幕关了”不是“管线坏了”。plane_active 会因此变假阴性。
 */
#ifndef YS_VIDEO_PIPE_H
#define YS_VIDEO_PIPE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ys_video ys_video_t;

/* 状态快照。所有 int 用 -1 表示“还没测到”。 */
typedef struct {
    double fps;               /* sink pad buffer 计数，1s 滑动窗口 */
    int    plane_active;      /* KMS overlay 平面 ACTIVE 且 framebuffer 非空 */
    int    audio_ok;          /* 音频支路已 PLAYING 且推过至少一帧 */
    int    game_w, game_h;    /* 解码器协商出的真实分辨率（0 = 未知） */
    int    video_w, video_h;  /* 上屏后的显示尺寸（面板像素） */
    int    decode_ms;         /* 首帧 push -> 解码器 src pad 出 buffer，-1 未测 */
    int    plane_ms;          /* 首帧 push -> summary 里平面新地址，-1 未测 */
    int    audio_frames;
    int    gst_error;         /* 0 = 无；非 0 = 管线进 ERROR，值是枚举 */
    int    dropped;           /* appsrc 满导致的丢帧数 */
    char   error_msg[160];
} ys_video_status;

/* canvas_w/h = 逻辑画布尺寸（960x266），用于 set_rect 的坐标换算。 */
ys_video_t *ys_video_create(int canvas_w, int canvas_h);
void        ys_video_destroy(ys_video_t *v);

/* 两段式起播：NULL -> PAUSED，轮询到视频支路出首 buffer（且音频支路就绪，
 * 若已 enable_audio），再 PAUSED -> PLAYING。
 * 返回 0 = 已 PLAYING 且首帧上屏；-1 = 超时/出错（写 error_msg）。
 * 最长阻塞 preroll_timeout_ms（建议 3000）。 */
int ys_video_start(ys_video_t *v, int preroll_timeout_ms);

/* 逻辑画布坐标 (x,y,w,h)，内部换算成面板坐标：
 *   panel_x = y, panel_y = PANEL_H - (x + w) - PANEL_X_DEADZONE, panel_w = h, panel_h = w
 * 传非法值（w<=0 / h<=0 / 非有限）返回 -1 且不改当前 rect。
 * 只允许缩小，放大返回 -1（render-rectangle 单向棘轮）。 */
int ys_video_set_rect(ys_video_t *v, int x, int y, int w, int h);

int ys_video_pause(ys_video_t *v);
int ys_video_resume(ys_video_t *v);
/* 幂等。会 set_state(NULL) 并销毁整条管线，保证 KMS 平面空出
 * （summary 里 Video Port0 回到 DISABLED）。之后 plane_active 返回 0。 */
int ys_video_stop(ys_video_t *v);

/* 推一帧完整 Annex-B 访问单元（每 NAL 前 4 字节 00 00 00 01）。
 * 不阻塞：appsrc 队列满则丢弃并累加 dropped。返回 0 = 已接受。 */
int ys_video_push_video(ys_video_t *v, const uint8_t *annexb, size_t len, uint32_t now_ms);

/* 推一个 Opus 裸包（不含 RTP 头，payload type 由上游决定）。 */
int ys_video_push_audio(ys_video_t *v, const uint8_t *opus, size_t len);

/* 音频开关（必须在 ys_video_start 之前调用）。默认关闭：服务端音频编码
 * 未在真机上验证过，session.c 先按「不接音频」发布。 */
void ys_video_enable_audio(ys_video_t *v, int on);

/* 每次调用把 GStreamer bus 泵干净，并刷新 fps 滑动窗口与首帧计时。
 * session.c 请在 10ms 循环里调。不阻塞（bus 用 0 超时轮询）。 */
void ys_video_tick(ys_video_t *v, uint32_t now_ms);

void ys_video_get_status(ys_video_t *v, ys_video_status *out);
int  ys_video_plane_active(ys_video_t *v);

/* 面板几何常量，供 session.c 做 UI 布局换算。 */
#define YS_PANEL_W        480
#define YS_PANEL_H        960
#define YS_PANEL_X_DEADZONE 107
#define YS_KMS_PLANE_ID   76

#ifdef __cplusplus
}
#endif
#endif
