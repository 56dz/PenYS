/*
 * video_pipe.c -- PenYS 视频通路: appsrc -> h264parse -> mppvideodec -> queue -> kmssink
 *                 （音频支路按契约"先不接音频"，只留接口不建管线）
 *
 * 契约见 video/video_pipe.h（已冻结，本文件只实现不改声明）。
 *
 * 真机（X6 Pro / RK3562, GStreamer 1.22）踩过的坑，每条都对应下面一段代码：
 *   1. kmssink 必须显式 driver-name=rockchip + plane-id=76。本机 card1 是
 *      RKNPU 不是显示设备，挑错卡 -> 管线永远 ASYNC，push 静默卡死。
 *   2. kmssink max-lateness 单位是纳秒：40ms 要传 "40000000"。传 40 = 40ns
 *      -> 全部帧判 late 丢弃 -> sink 收 0 个 buffer -> 全黑且不报错。
 *   3. rtph264depay 与 h264parse 之间不许插 capsfilter（depay 的 codec_data
 *      会被丢掉 -> 解码器 in 有量 out 为 0）。本模块不走 RTP，appsrc 直接
 *      以 video/x-h264,stream-format=byte-stream,alignment=au 喂 Annex-B AU，
 *      这是 rtpprobe --raw 模式真机跑通的 caps。
 *   4. render-rectangle 是面板绝对坐标 480x960，Falcon 合成面占面板列
 *      107..372，所以 107px 死区加在 x 轴：
 *          panel_x = YS_PANEL_X_DEADZONE + y
 *          panel_y = YS_PANEL_H - (x + w)
 *          panel_w = h,  panel_h = w
 *      全画布 (0,0,960,266) 实测 dst = [107,0, 266x960]。
 *   5. KMS 视频平面 zpos 2 会盖住 UI（UI 平面 zpos 0）。rect 为 0 尺寸时
 *      set_rect 直接拒绝；出界 rect 做 clamp（session 的初始 set_rect 失败
 *      会 w_fail 中止会话，所以只拒 w<=0/h<=0，坐标垃圾值收编不拒绝）。
 *   6. /sys/kernel/debug/dri/0/summary 是 seq_file，单次 read 只回 576 字节，
 *      必须循环 read 到 EOF。判 ACTIVE 要两条同时成立：窗口头行含 ACTIVE，
 *      且该段内 buf[0]: addr: 非 0。
 *   7. 两段式起播：is-live 的 appsrc 在 PAUSED 需要 preroll 一个 buffer 才能
 *      完成切换，而 start 时一个包都没推 -> gst_element_get_state() 带超时
 *      会一直等到超时（rtpprobe 实测"永远阻塞"）。所以 start 只发状态请求
 *      （PAUSED -> PLAYING，pending=PLAYING），用 0 超时轮询探针计数，靠
 *      调用方返回后继续推流把 preroll 喂满，pending=PLAYING 自动生效。
 *
 * 线程模型：所有公开函数只在 session.c 的唯一 worker 线程上调用；另外只有
 * GStreamer streaming 线程会在 pad 探针里改 relaxed 原子计数器。无锁、不自建
 * 线程。RTP 推送路径除 GStreamer buffer 外零 malloc。
 */
#include "video/video_pipe.h"

#include "core/log.h"
#include "core/thread.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ====================================================================== */
/* 手写最小 GStreamer 声明（设备无 gst dev 头；对照设备 .so .dynsym 核对过） */
/* ====================================================================== */

typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;
typedef struct _GstMessage GstMessage;
typedef struct _GstPad GstPad;
typedef struct _GstPadProbeInfo GstPadProbeInfo;
typedef struct _GstCaps GstCaps;
typedef struct _GstStructure GstStructure;
typedef struct _GstBin GstBin;
typedef struct _GstBuffer GstBuffer;

typedef int gboolean;
typedef int GstState;
typedef int GstStateChangeReturn;
typedef int GstPadProbeReturn;
typedef int GstMessageType;
typedef int GstFlowReturn;
typedef unsigned long long GstClockTime;
typedef unsigned long gulong;

typedef struct _GError {
    unsigned int domain;
    int code;
    char *message;
} GError;

#define GST_STATE_VOID_PENDING 0
#define GST_STATE_NULL         1
#define GST_STATE_READY        2
#define GST_STATE_PAUSED       3
#define GST_STATE_PLAYING      4

#define GST_STATE_CHANGE_FAILURE 0
#define GST_STATE_CHANGE_SUCCESS 1
#define GST_STATE_CHANGE_ASYNC   2

#define GST_FLOW_OK 0

#define GST_PAD_PROBE_OK         1
#define GST_PAD_PROBE_TYPE_BUFFER 16

#define GST_MESSAGE_EOS     (1 << 0)
#define GST_MESSAGE_ERROR   (1 << 1)
#define GST_MESSAGE_WARNING (1 << 2)

/* Core / Element */
extern void gst_init(int *argc, char ***argv);
extern GstElement *gst_pipeline_new(const char *name);
extern GstElement *gst_element_factory_make(const char *factoryname, const char *name);
extern gboolean gst_bin_add(GstBin *bin, GstElement *element);
extern gboolean gst_element_link(GstElement *src, GstElement *dest);
extern GstStateChangeReturn gst_element_set_state(GstElement *element, GstState state);
extern GstStateChangeReturn gst_element_get_state(GstElement *element, GstState *state,
                                                  GstState *pending, GstClockTime timeout);
extern GstBus *gst_element_get_bus(GstElement *element);

/* Bus / Message（gst_message_type 是读结构体的宏，这里不可用，改用结构名判断） */
extern GstMessage *gst_bus_timed_pop_filtered(GstBus *bus, GstClockTime timeout,
                                              GstMessageType types);
extern void gst_message_parse_error(GstMessage *message, GError **gerror, char **debug);
extern GstStructure *gst_message_get_structure(GstMessage *message);
extern void gst_message_unref(GstMessage *message);
extern const char *gst_structure_get_name(const GstStructure *structure);
extern gboolean gst_structure_get_int(const GstStructure *structure,
                                      const char *fieldname, int *value);

/* Pad / Caps */
extern GstPad *gst_element_get_static_pad(GstElement *element, const char *name);
extern gulong gst_pad_add_probe(GstPad *pad, int mask,
                                GstPadProbeReturn (*callback)(GstPad *, GstPadProbeInfo *, void *),
                                void *user_data, void (*destroy_data)(void *));
extern GstCaps *gst_pad_get_current_caps(GstPad *pad);
extern GstCaps *gst_caps_from_string(const char *string);
extern GstStructure *gst_caps_get_structure(const GstCaps *caps, unsigned int index);
extern void gst_caps_unref(GstCaps *caps);

/* Buffer（RTP 路径零 malloc 之外唯一的分配点） */
extern GstBuffer *gst_buffer_new_allocate(void *allocator, size_t size, void *params);
extern size_t gst_buffer_fill(GstBuffer *buffer, size_t offset, const void *src, size_t size);
extern void gst_buffer_unref(GstBuffer *buffer);

/* GObject / GLib */
extern void gst_object_unref(void *object);
extern void gst_util_set_object_arg(void *object, const char *name, const char *value);
extern void g_object_set(void *object, const char *first_property_name, ...);
extern void g_object_get(void *object, const char *first_property_name, ...);
extern void g_error_free(GError *error);
extern void g_free(void *mem);
/* appsrc 的 push-buffer action signal：libgstapp 不在链接行里，
 * gst_app_src_push_buffer 不可用，用信号发射等价（文档明示两条路径等价）。 */
extern void g_signal_emit_by_name(void *instance, const char *signal_name, ...);

/* ====================================================================== */
/* 内部常量与状态                                                          */
/* ====================================================================== */

#define VP_SUMMARY_PATH  "/sys/kernel/debug/dri/0/summary"
#define VP_SUMMARY_CAP   65536
#define VP_SUMMARY_MS    100    /* summary 读取节流：至少 100ms 一次 */
#define VP_GRACE_MS      250    /* start 无帧时的最短等待，超过即放行给调用方 */
#define VP_QUEUE_SET     1048576 /* appsrc max-bytes 目标值 */
#define VP_QUEUE_FALLBACK 200000 /* appsrc max-bytes 读回失败时的保守值 */

struct ys_video {
    /* 配置 */
    int  canvas_w, canvas_h;
    int  audio_on;              /* 契约保留：音频支路未实现，恒不建 */

    /* 当前逻辑矩形（面板换算的输入）+ 单向棘轮上限 */
    int  rx, ry, rw, rh;
    int  lim_w, lim_h;

    /* 管线（所有权：pipeline 拥有全部元素；appsrc/sink 是借用指针） */
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *sink;
    int         started;        /* start() 已成功返回 */

    /* 探针计数：streaming 线程写，worker 线程读 -> relaxed 原子 */
    unsigned    dec_out;        /* 解码器 src pad 出 buffer 数 */
    unsigned    sink_cnt;       /* kmssink sink pad 收 buffer 数（fps + preroll） */
    unsigned    dropped;        /* appsrc 满导致的丢帧数 */
    uint32_t    first_push_ms;  /* 首次接受 push 的时刻（度量起点） */
    int         decode_ms;      /* 首帧 push -> 解码器出帧，-1 未测 */
    int         game_w, game_h; /* 解码器协商分辨率，0 未知 */

    /* 平面/显示度量（worker 线程） */
    int     plane_ms;           /* 首帧 push -> summary 平面新地址，-1 未测 */
    int     video_w, video_h;   /* 上屏显示尺寸（面板像素，即 render-rectangle） */
    int     plane_cache;        /* summary 最近一次判定 0/1 */

    /* fps 1s 滑动窗口（worker 线程） */
    double  fps;
    uint32_t fps_ms;
    unsigned fps_cnt;

    /* summary 缓冲（worker 线程读写；64KB 放堆上，栈不背这个包袱） */
    char    sbuf[VP_SUMMARY_CAP];
    uint32_t last_sum_ms;

    /* appsrc 队列上限（build 时从属性读回） */
    unsigned long long queue_max;

    /* 错误 */
    int     gst_error;          /* 0 无；非 0 = 管线 ERROR（GError code） */
    char    error_msg[160];
};

/* ====================================================================== */
/* DRM summary 读取与解析（seq_file 必须循环 read 到 EOF）                  */
/* ====================================================================== */

static int vp_read_summary(ys_video_t *v) {
    int fd = open(VP_SUMMARY_PATH, O_RDONLY | O_CLOEXEC);
    size_t total = 0;

    if (fd < 0) return -1;
    for (;;) {
        ssize_t n;
        if (total >= sizeof(v->sbuf) - 1) break;
        n = read(fd, v->sbuf + total, sizeof(v->sbuf) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    v->sbuf[total] = '\0';
    return (int)total;
}

/*
 * 从窗口头位置往后找这一段的结束（下一个窗口头或缩进归零的行）。
 * 真机 summary 结构（视频上屏时约 974 字节）：
 *     Video Port0: ACTIVE
 *         Connector: DSI-1
 *         Esmart0-win0: ACTIVE        <- UI 平面 zpos 0
 *             win_id: 0
 *             buf[0]: addr: 0x0000000000000000 ...
 *         Esmart1-win0: ACTIVE        <- 视频平面 plane-id 76
 *             win_id: 1
 *             dst: pos[x, y] rect[w x h]
 *             buf[0]: addr: 0x0000000002534000 ...
 * 段尾陷阱：win_id 行紧跟窗口头，不能拿它当段尾，否则 buf[0] 行永远落在段外。
 */
static const char *vp_block_end(const char *p) {
    const char *q = p;
    while (*q && *q != '\n') q++;      /* 跳过窗口头这一行 */
    while (*q) {
        const char *line = q + 1;      /* 下一行行首 */
        const char *s = line;
        const char *win;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') break;
        if ((s - line) == 0) return line;   /* 缩进归零 => 不再属于该窗口 */
        win = strstr(s, "-win");
        if (win) {
            const char *r = win + 4;
            int digits = 0;
            while (*r >= '0' && *r <= '9') { r++; digits++; }
            if (digits > 0 && *r == ':') return line;   /* 下一个窗口头 */
        }
        q = line;
        while (*q && *q != '\n') q++;
    }
    return q;
}

/* ACTIVE 且 framebuffer 地址非 0 才算有画面。 */
static int vp_summary_active(const char *buf) {
    const char *p, *end, *nl, *a, *val;
    int active = 0;

    p = strstr(buf, "Esmart1-win0");
    if (!p) return 0;

    nl = p;
    while (*nl && *nl != '\n') nl++;
    {
        const char *q;
        for (q = p; q + 6 <= nl; q++) {
            if (strncmp(q, "ACTIVE", 6) == 0) { active = 1; break; }
        }
    }
    if (!active) return 0;

    end = vp_block_end(p);
    if (!end) end = buf + strlen(buf);

    a = strstr(p, "buf[0]: addr:");
    if (!a || a >= end) return 0;

    val = a + strlen("buf[0]: addr:");
    while (*val == ' ') val++;
    return strtoull(val, NULL, 0) != 0ULL;
}

/* 刷新 plane_active 缓存，并在首帧之后补记 plane_ms。 */
static void vp_refresh_summary(ys_video_t *v, uint32_t now) {
    v->last_sum_ms = now;
    if (vp_read_summary(v) <= 0) {
        v->plane_cache = 0;
        return;
    }
    v->plane_cache = vp_summary_active(v->sbuf);
    if (v->plane_cache && v->plane_ms < 0) {
        uint32_t fp = __atomic_load_n(&v->first_push_ms, __ATOMIC_RELAXED);
        if (fp) v->plane_ms = (int)(now - fp);
    }
}

/* ====================================================================== */
/* bus 抽干（0 超时，永不阻塞）                                             */
/* ====================================================================== */

static void vp_pump_bus(ys_video_t *v) {
    GstBus *bus;
    GstMessage *m;

    if (!v->pipeline) return;
    bus = gst_element_get_bus(v->pipeline);   /* 新引用，下面要还 */
    if (!bus) return;

    while ((m = gst_bus_timed_pop_filtered(bus, 0,
             GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS)) != NULL) {
        GstStructure *s = gst_message_get_structure(m);
        const char *nm = s ? gst_structure_get_name(s) : NULL;

        if (nm && strcmp(nm, "GstMessageError") == 0) {
            GError *e = NULL;
            char *dbg = NULL;
            gst_message_parse_error(m, &e, &dbg);
            if (v->gst_error == 0) {
                int n;
                v->gst_error = (e && e->code != 0) ? e->code : 1;
                n = snprintf(v->error_msg, sizeof v->error_msg, "GStreamer ERROR: %s",
                             (e && e->message) ? e->message : "unknown");
                if (n > 0 && n < (int)sizeof v->error_msg) {
                    /* 把 debug 细节（哪个元素、什么原因）也带上，截断由 snprintf 兜底 */
                    snprintf(v->error_msg + n, sizeof v->error_msg - (size_t)n,
                             " | %s", dbg ? dbg : "");
                }
                ys_logf("video: bus error: %s", v->error_msg);
            }
            if (e) g_error_free(e);
            if (dbg) g_free(dbg);
        } else if (nm && strcmp(nm, "GstMessageWarning") == 0) {
            GError *e = NULL;
            char *dbg = NULL;
            gst_message_parse_error(m, &e, &dbg);
            ys_logf("video: bus warning: %s | %s",
                    (e && e->message) ? e->message : "?", dbg ? dbg : "-");
            if (e) g_error_free(e);
            if (dbg) g_free(dbg);
        } else if (nm && strcmp(nm, "GstMessageEOS") == 0) {
            ys_logf("video: bus EOS");
        } else {
            /* 结构缺失时退回 parse_error 探测（pop 过滤器保证只可能是这三类） */
            GError *e = NULL;
            char *dbg = NULL;
            gst_message_parse_error(m, &e, &dbg);
            if (e) {
                if (v->gst_error == 0) {
                    int n;
                    v->gst_error = (e->code != 0) ? e->code : 1;
                    n = snprintf(v->error_msg, sizeof v->error_msg,
                                 "GStreamer ERROR: %s",
                                 e->message ? e->message : "unknown");
                    if (n > 0 && n < (int)sizeof v->error_msg) {
                        snprintf(v->error_msg + n, sizeof v->error_msg - (size_t)n,
                                 " | %s", dbg ? dbg : "");
                    }
                    ys_logf("video: bus error: %s", v->error_msg);
                }
                g_error_free(e);
            }
            if (dbg) g_free(dbg);
        }
        gst_message_unref(m);
    }
    gst_object_unref(bus);
}

/* ====================================================================== */
/* render-rectangle：逻辑画布 -> 面板绝对坐标（107px 死区在 x 轴）          */
/* ====================================================================== */

static void vp_apply_rect(ys_video_t *v, GstElement *sink) {
    char rect[64];
    int px, py, pw, ph;

    if (!sink) return;

    px = YS_PANEL_X_DEADZONE + v->ry;
    py = YS_PANEL_H - (v->rx + v->rw);
    pw = v->rh;
    ph = v->rw;

    /* 出界收编：垃圾坐标 clamp 进 480x960，保证 kmssink 拿到的永远合法 */
    if (px < 0) px = 0;
    if (px >= YS_PANEL_W) px = YS_PANEL_W - 1;
    if (py < 0) py = 0;
    if (py >= YS_PANEL_H) py = YS_PANEL_H - 1;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    if (px + pw > YS_PANEL_W) pw = YS_PANEL_W - px;
    if (py + ph > YS_PANEL_H) ph = YS_PANEL_H - py;

    snprintf(rect, sizeof rect, "<%d,%d,%d,%d>", px, py, pw, ph);
    gst_util_set_object_arg(sink, "render-rectangle", rect);
    v->video_w = pw;
    v->video_h = ph;
    ys_logf("video: rect (%d,%d,%d,%d) -> panel %s", v->rx, v->ry, v->rw, v->rh, rect);
}

/* ====================================================================== */
/* pad 探针（streaming 线程回调，只碰 relaxed 原子计数）                     */
/* ====================================================================== */

static GstPadProbeReturn vp_on_dec_src(GstPad *pad, GstPadProbeInfo *info, void *ud) {
    ys_video_t *v = (ys_video_t *)ud;
    unsigned n;

    (void)pad;
    (void)info;

    n = __atomic_add_fetch(&v->dec_out, 1, __ATOMIC_RELAXED);
    if (n == 1) {
        uint32_t fp = __atomic_load_n(&v->first_push_ms, __ATOMIC_RELAXED);
        if (fp) {
            __atomic_store_n(&v->decode_ms, (int)(ys_mono_ms() - fp), __ATOMIC_RELAXED);
        }
        /* 解码器协商出的真实分辨率（首个输出 buffer 时 caps 已定） */
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (caps) {
            GstStructure *st = gst_caps_get_structure(caps, 0);
            int w = 0, h = 0;
            if (st && gst_structure_get_int(st, "width", &w) &&
                gst_structure_get_int(st, "height", &h) && w > 0 && h > 0) {
                __atomic_store_n(&v->game_w, w, __ATOMIC_RELAXED);
                __atomic_store_n(&v->game_h, h, __ATOMIC_RELAXED);
            }
            gst_caps_unref(caps);
        }
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn vp_on_sink_buf(GstPad *pad, GstPadProbeInfo *info, void *ud) {
    ys_video_t *v = (ys_video_t *)ud;
    (void)pad;
    (void)info;
    __atomic_add_fetch(&v->sink_cnt, 1, __ATOMIC_RELAXED);
    return GST_PAD_PROBE_OK;
}

/* ====================================================================== */
/* 管线构建                                                                */
/* ====================================================================== */

static int vp_build(ys_video_t *v) {
    static int gst_ready = 0;
    GstElement *p = NULL, *src = NULL, *parse = NULL, *dec = NULL, *q = NULL, *sink = NULL;
    GstElement *e[5];
    GstPad *pad;
    GstCaps *caps;
    unsigned long long mb = 0;
    int k = 0;

    if (!gst_ready) {
        gst_init(NULL, NULL);
        gst_ready = 1;
    }

    p = gst_pipeline_new("ys-video");
    src = gst_element_factory_make("appsrc", "vsrc");
    parse = gst_element_factory_make("h264parse", "vparse");
    dec = gst_element_factory_make("mppvideodec", "vdec");
    q = gst_element_factory_make("queue", "vq");
    sink = gst_element_factory_make("kmssink", "vsink");
    e[0] = src; e[1] = parse; e[2] = dec; e[3] = q; e[4] = sink;

    if (!p || !src || !parse || !dec || !q || !sink) {
        snprintf(v->error_msg, sizeof v->error_msg,
                 "build: missing element (appsrc=%d h264parse=%d mppvideodec=%d "
                 "queue=%d kmssink=%d)", !!src, !!parse, !!dec, !!q, !!sink);
        goto fail;   /* k=0：5 个元素都还没入 bin，由 fail 路径逐个释放一次 */
    }

    if (!gst_element_link(src, parse) || !gst_element_link(parse, dec) ||
        !gst_element_link(dec, q) || !gst_element_link(q, sink)) {
        snprintf(v->error_msg, sizeof v->error_msg, "build: gst_element_link failed");
        goto fail;   /* 尚未入 bin：fail 路径把 5 个元素全部释放 */
    }
    for (k = 0; k < 5; k++) {
        if (!gst_bin_add((GstBin *)p, e[k])) {
            snprintf(v->error_msg, sizeof v->error_msg, "build: gst_bin_add failed");
            goto fail;   /* e[0..k) 归 p，e[k..4] 归本函数 */
        }
    }

    /* ---- appsrc：Annex-B byte-stream AU；block=false 保证 push 永不阻塞 ---- */
    caps = gst_caps_from_string("video/x-h264,stream-format=(string)byte-stream,"
                                "alignment=(string)au");
    if (caps) {
        g_object_set(src, "caps", caps, NULL);   /* g_object_set 自己 take ref */
        gst_caps_unref(caps);
    }
    gst_util_set_object_arg(src, "is-live", "true");
    gst_util_set_object_arg(src, "format", "time");
    gst_util_set_object_arg(src, "stream-type", "stream");
    gst_util_set_object_arg(src, "do-timestamp", "true");
    gst_util_set_object_arg(src, "block", "false");
    gst_util_set_object_arg(src, "emit-signals", "false");
    gst_util_set_object_arg(src, "max-bytes", "1048576");
    /* 读回真实上限：set 若被属性拒绝，push 侧按真实值预判，永远不会撞上反压 */
    g_object_get(src, "max-bytes", &mb, NULL);
    v->queue_max = mb ? mb : VP_QUEUE_FALLBACK;

    /* ---- 解码器（rtpprobe 真机配方） ---- */
    g_object_set(dec, "fast-mode", 1, "ignore-error", 0, "max-errors", 16, NULL);

    /* ---- kmssink：两个必填 + 纳秒 max-lateness + render-rectangle ---- */
    g_object_set(sink, "sync", 0, "qos", 1, "can-scale", 1, "force-aspect-ratio", 0,
                 "skip-vsync", 0, "plane-id", YS_KMS_PLANE_ID, NULL);
    /* 必须显式指定 DRM 卡：card1 是 RKNPU，挑错卡 -> 永远不进 PLAYING。 */
    g_object_set(sink, "driver-name", "rockchip", NULL);
    /* max-lateness 单位是纳秒：40ms 必须写成 40000000，写 40 = 全帧丢弃全黑 */
    gst_util_set_object_arg(sink, "max-lateness", "40000000");
    vp_apply_rect(v, sink);   /* 默认整画布 -> <107,0,266,960>（此时 v->sink 还没赋值，用局部 sink） */

    /* ---- 探针：解码器出帧（decode_ms/game_w/h）+ sink 收帧（fps/preroll） ---- */
    pad = gst_element_get_static_pad(dec, "src");
    if (pad) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, vp_on_dec_src, v, NULL);
        gst_object_unref(pad);
    }
    pad = gst_element_get_static_pad(sink, "sink");
    if (pad) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, vp_on_sink_buf, v, NULL);
        gst_object_unref(pad);
    }

    v->pipeline = p;
    v->appsrc = src;
    v->sink = sink;
    ys_logf("video: pipeline built (canvas %dx%d, queue_max=%llu)",
            v->canvas_w, v->canvas_h, v->queue_max);
    return 0;

fail:
    /* k = 已成功入 bin 的元素个数：e[0..k) 的所有权归 p（随 p 释放），
     * e[k..4] 还在本函数手里，逐个释放恰好一次；p 为空管线无害。 */
    for (; k < 5; k++) {
        if (e[k]) gst_object_unref(e[k]);
    }
    if (p) gst_object_unref(p);
    ys_logf("video: build failed: %s", v->error_msg);
    return -1;
}

/* ====================================================================== */
/* 公开 API                                                                */
/* ====================================================================== */

ys_video_t *ys_video_create(int canvas_w, int canvas_h) {
    ys_video_t *v = (ys_video_t *)calloc(1, sizeof *v);
    if (!v) return NULL;

    v->canvas_w = canvas_w > 0 ? canvas_w : 960;
    v->canvas_h = canvas_h > 0 ? canvas_h : 266;
    /* 默认矩形 = 整个画布（全画布实测 dst [107,0,266x960]） */
    v->rx = 0;
    v->ry = 0;
    v->rw = v->canvas_w;
    v->rh = v->canvas_h;
    /* 棘轮上限取 jsapi valid_rect 的硬顶 4096：首调 set_rect 必过
     * （session.c 初始 set_rect 失败会 w_fail 中止会话）。之后只许缩小。 */
    v->lim_w = 4096;
    v->lim_h = 4096;
    v->decode_ms = -1;
    v->plane_ms = -1;
    v->queue_max = VP_QUEUE_FALLBACK;

    /* 管线在这里就建好：start 前后到达的帧都能进 appsrc 队列，
     * preroll 可以在 start 的轮询窗口内完成。建失败不致命，start 会重试。 */
    if (vp_build(v) != 0) v->pipeline = NULL;
    return v;
}

void ys_video_destroy(ys_video_t *v) {
    if (!v) return;
    ys_video_stop(v);
    free(v);
}

int ys_video_start(ys_video_t *v, int preroll_timeout_ms) {
    GstStateChangeReturn r;
    uint32_t t0;
    int budget, grace;

    if (!v) return -1;
    if (v->started) return 0;
    if (!v->pipeline) {
        if (vp_build(v) != 0) return -1;
    }

    /* 度量窗口从 start 起算 */
    __atomic_store_n(&v->first_push_ms, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&v->decode_ms, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&v->dec_out, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&v->sink_cnt, 0, __ATOMIC_RELAXED);
    v->plane_ms = -1;
    v->plane_cache = 0;
    v->last_sum_ms = 0;
    v->fps = 0;
    v->fps_ms = 0;
    v->fps_cnt = 0;

    /* 两段式：只发请求，绝不用带超时的 get_state 等待
     * （PAUSED 需要 preroll 一个 buffer，而此刻一个包都没推）。 */
    r = gst_element_set_state(v->pipeline, GST_STATE_PAUSED);
    if (r == GST_STATE_CHANGE_FAILURE) {
        snprintf(v->error_msg, sizeof v->error_msg, "set_state(PAUSED) -> FAILURE");
        ys_logf("video: %s", v->error_msg);
        return -1;
    }
    r = gst_element_set_state(v->pipeline, GST_STATE_PLAYING);
    if (r == GST_STATE_CHANGE_FAILURE) {
        snprintf(v->error_msg, sizeof v->error_msg, "set_state(PLAYING) -> FAILURE");
        ys_logf("video: %s", v->error_msg);
        return -1;
    }

    if (preroll_timeout_ms <= 0) {
        v->started = 1;
        return 0;
    }

    budget = preroll_timeout_ms;
    grace = budget < VP_GRACE_MS ? budget : VP_GRACE_MS;
    t0 = ys_mono_ms();

    for (;;) {
        uint32_t el;
        vp_pump_bus(v);
        if (v->gst_error) return -1;   /* error_msg 已在 pump 里写好 */

        el = ys_mono_ms() - t0;
        if (__atomic_load_n(&v->sink_cnt, __ATOMIC_RELAXED) > 0) {
            /* 首 buffer 已到：0 超时确认状态（绝不会阻塞） */
            GstState st = GST_STATE_VOID_PENDING, pend = GST_STATE_VOID_PENDING;
            GstStateChangeReturn g = gst_element_get_state(v->pipeline, &st, &pend, 0);
            if (g == GST_STATE_CHANGE_SUCCESS && st == GST_STATE_PLAYING) {
                v->started = 1;
                ys_logf("video: PLAYING in %u ms (sink=%u dec=%u)",
                        (unsigned)el,
                        __atomic_load_n(&v->sink_cnt, __ATOMIC_RELAXED),
                        __atomic_load_n(&v->dec_out, __ATOMIC_RELAXED));
                return 0;
            }
            if (el >= (uint32_t)budget) {
                /* 有 buffer 却进不了 PLAYING = 卡在 preroll（典型：挑错 DRM 卡） */
                snprintf(v->error_msg, sizeof v->error_msg,
                         "preroll timeout: first buffer arrived but not PLAYING "
                         "in %d ms (state=%d pending=%d)", budget, (int)st, (int)pend);
                ys_logf("video: %s", v->error_msg);
                return -1;
            }
        } else if (el >= (uint32_t)grace) {
            /* 无帧可 preroll：状态请求已被接受，交给调用方返回后推流喂满，
             * pending=PLAYING 会在 preroll 完成时自动生效（两段式契约）。 */
            v->started = 1;
            ys_logf("video: start requested, no frames yet after %u ms "
                    "(caller must keep pushing)", (unsigned)el);
            return 0;
        }
        ys_sleep_ms(10);
    }
}

int ys_video_set_rect(ys_video_t *v, int x, int y, int w, int h) {
    if (!v) return -1;
    if (w <= 0 || h <= 0) return -1;             /* 0 尺寸拒绝，不改当前 rect */
    if (w > v->lim_w || h > v->lim_h) return -1; /* 放大：render-rectangle 单向棘轮 */
    v->rx = x;
    v->ry = y;
    v->rw = w;
    v->rh = h;
    v->lim_w = w;
    v->lim_h = h;
    vp_apply_rect(v, v->sink);
    return 0;
}

int ys_video_pause(ys_video_t *v) {
    GstStateChangeReturn r;
    if (!v || !v->pipeline) return -1;
    r = gst_element_set_state(v->pipeline, GST_STATE_PAUSED);
    return (r == GST_STATE_CHANGE_FAILURE) ? -1 : 0;
}

int ys_video_resume(ys_video_t *v) {
    GstStateChangeReturn r;
    if (!v || !v->pipeline) return -1;
    r = gst_element_set_state(v->pipeline, GST_STATE_PLAYING);
    return (r == GST_STATE_CHANGE_FAILURE) ? -1 : 0;
}

int ys_video_stop(ys_video_t *v) {
    if (!v) return -1;
    if (v->pipeline) {
        ys_logf("video: stop (sink=%u dec=%u dropped=%u decode_ms=%d plane_ms=%d)",
                __atomic_load_n(&v->sink_cnt, __ATOMIC_RELAXED),
                __atomic_load_n(&v->dec_out, __ATOMIC_RELAXED),
                __atomic_load_n(&v->dropped, __ATOMIC_RELAXED),
                __atomic_load_n(&v->decode_ms, __ATOMIC_RELAXED), v->plane_ms);
        /* set_state(NULL) 同步回收 KMS 平面（真机 12~13ms 内释放 plane 76），
         * 随后销毁整条管线 —— 否则进程崩了平面会冻帧盖住桌面。 */
        gst_element_set_state(v->pipeline, GST_STATE_NULL);
        gst_object_unref(v->pipeline);
        v->pipeline = NULL;
        v->appsrc = NULL;   /* 借用指针随 pipeline 一起失效 */
        v->sink = NULL;
    }
    v->started = 0;
    v->plane_cache = 0;     /* plane_active 之后恒 0（契约） */
    v->last_sum_ms = 0;
    v->fps = 0;
    v->fps_ms = 0;
    v->fps_cnt = 0;
    return 0;               /* 幂等 */
}

int ys_video_push_video(ys_video_t *v, const uint8_t *annexb, size_t len, uint32_t now_ms) {
    GstBuffer *buf;
    GstFlowReturn flow = GST_FLOW_OK;
    unsigned long long lvl = 0;

    (void)now_ms;   /* 度量统一用 ys_mono_ms()，避免两套时钟对不齐 */

    if (!v || !annexb || len == 0) return -1;
    if (!v->pipeline || !v->appsrc) return -1;

    /* 队列满预判：单生产者，level 只会被消费端减小，预判通过即永不撞反压 */
    g_object_get(v->appsrc, "current-level-bytes", &lvl, NULL);
    if (lvl + (unsigned long long)len > v->queue_max) {
        __atomic_add_fetch(&v->dropped, 1, __ATOMIC_RELAXED);
        return -1;
    }

    buf = gst_buffer_new_allocate(NULL, len, NULL);
    if (!buf) {
        __atomic_add_fetch(&v->dropped, 1, __ATOMIC_RELAXED);
        return -1;
    }
    if (gst_buffer_fill(buf, 0, annexb, len) != len) {
        gst_buffer_unref(buf);
        __atomic_add_fetch(&v->dropped, 1, __ATOMIC_RELAXED);
        return -1;
    }

    /* 信号发射等价于 gst_app_src_push_buffer（后者不在链接行里）。
     * 信号路径只 take 一个引用，发射完必须自己 unref。 */
    g_signal_emit_by_name(v->appsrc, "push-buffer", buf, &flow);
    gst_buffer_unref(buf);

    if (flow != GST_FLOW_OK) {
        __atomic_add_fetch(&v->dropped, 1, __ATOMIC_RELAXED);
        return -1;
    }
    {
        uint32_t zero = 0;
        if (__atomic_compare_exchange_n(&v->first_push_ms, &zero, ys_mono_ms(),
                                        0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            /* 首帧 push 时刻已记录（decode_ms / plane_ms 的度量起点） */
        }
    }
    return 0;
}

int ys_video_push_audio(ys_video_t *v, const uint8_t *opus, size_t len) {
    /* 音频支路未实现（编码未验证，契约：先不接音频）→ 不接受 */
    (void)v;
    (void)opus;
    (void)len;
    return -1;
}

void ys_video_enable_audio(ys_video_t *v, int on) {
    if (!v) return;
    v->audio_on = on ? 1 : 0;   /* 仅留接口：管线不建音频支路，audio_ok 恒 0 */
}

void ys_video_tick(ys_video_t *v, uint32_t now_ms) {
    if (!v) return;
    vp_pump_bus(v);
    if (!v->pipeline) return;

    /* fps 1s 滑动窗口（sink pad buffer 计数） */
    {
        unsigned c = __atomic_load_n(&v->sink_cnt, __ATOMIC_RELAXED);
        if (v->fps_ms == 0) {
            v->fps_ms = now_ms;
            v->fps_cnt = c;
        } else {
            uint32_t el = now_ms - v->fps_ms;   /* uint32 差值，回绕安全 */
            if (el >= 1000) {
                v->fps = (double)(c - v->fps_cnt) * 1000.0 / (double)el;
                v->fps_ms = now_ms;
                v->fps_cnt = c;
            }
        }
    }

    /* summary 节流刷新：plane_active 缓存 + 首帧后的 plane_ms */
    if ((uint32_t)(now_ms - v->last_sum_ms) >= VP_SUMMARY_MS) {
        vp_refresh_summary(v, now_ms);
    }
}

void ys_video_get_status(ys_video_t *v, ys_video_status *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->decode_ms = -1;
    out->plane_ms = -1;
    if (!v) return;

    /* 自足刷新：即使调用方没在跑 tick，状态里的平面信息也不超过 100ms 旧 */
    if (v->pipeline) {
        uint32_t now = ys_mono_ms();
        if ((uint32_t)(now - v->last_sum_ms) >= VP_SUMMARY_MS) {
            vp_refresh_summary(v, now);
        }
    }

    out->fps = v->fps;
    out->plane_active = v->pipeline ? v->plane_cache : 0;
    out->audio_ok = 0;                        /* 音频支路未建，恒 0 */
    out->game_w = __atomic_load_n(&v->game_w, __ATOMIC_RELAXED);
    out->game_h = __atomic_load_n(&v->game_h, __ATOMIC_RELAXED);
    out->video_w = v->video_w;
    out->video_h = v->video_h;
    out->decode_ms = __atomic_load_n(&v->decode_ms, __ATOMIC_RELAXED);
    out->plane_ms = v->plane_ms;
    out->audio_frames = 0;
    out->gst_error = v->gst_error;
    out->dropped = (int)__atomic_load_n(&v->dropped, __ATOMIC_RELAXED);
    memcpy(out->error_msg, v->error_msg, sizeof out->error_msg);
}

int ys_video_plane_active(ys_video_t *v) {
    if (!v || !v->pipeline) return 0;
    {
        uint32_t now = ys_mono_ms();
        if ((uint32_t)(now - v->last_sum_ms) >= VP_SUMMARY_MS) {
            vp_refresh_summary(v, now);
        }
    }
    return v->plane_cache;
}
