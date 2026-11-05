/*
 * rtp_reorder.h -- RTP 接收侧：序号重排 + H.264/H.265 拆包。
 *
 * 为什么需要它：SRTP 解密之后拿到的是一堆 RTP 包，而 mppvideodec 要的是
 * **完整的 NAL 单元**（或者至少是能自己切的 Annex-B 字节流）。中间这一步
 * 没有任何现成组件可用——设备上没有 gst 的 rtpbin/rtph264depay 依赖链里
 * 我们想避开的东西（rtph264depay 本身有，但它要接在 rtpbin 上，而 rtpbin
 * 需要 ICE/DTLS 那一整套；我们自己是 UDP socket 直收，所以拆包逻辑自己写
 * 反而更短、更可控）。
 *
 * 设计约束（都是真机/协议层面的事实，不是偏好）：
 *
 *   1) **零热路径 malloc**。每个视频包 30~60 个/秒，加上音频能到 100/s 量级，
 *      持续几十分钟。malloc/free 抖动在 4xA35 上会被放大成可见的卡顿。
 *      所以：重排环、重组缓冲、输出缓冲全部在 ctx 里预分配。
 *
 *   2) **交付是「整帧」不是「整包」**。H.264 的一帧可能跨几十个 RTP 包
 *      （1080p 关键帧 ~100KB / 1200 字节每包 ≈ 85 个包）。凑不齐就叫
 *      mppvideodec 解，会解出花屏或者干脆解不出来。所以这里必须按
 *      marker 位 + 时间戳判断帧边界。
 *
 *   3) **AV1/VP9 不支持打包模式**。设备硬解支持 h264/h265/av1/vp8/vp9，
 *      但云原神服务端实测下发的是 H.264（codec_type=1），所以只做
 *      H.264 + H.265 的 RFC 6184 / RFC 7798 拆包。别的编码直接拒绝，
 *      不要留半吊子实现。
 *
 *   4) **丢包必须能让上层知道**。丢一个 FU-A 的中间片，整个 NAL 就废了，
 *      必须整帧丢弃而不是交半帧给解码器（半帧会让解码器进入错误状态，
 *      后续正常帧也解不出来，直到下一个 IDR）。所以这里统计
 *      `frames_dropped` 并在丢帧后通知上层「等 IDR」。
 *
 * H.264 RTP 负载格式（RFC 6184）：
 *   nal_type = payload[0] & 0x1F
 *   1..23  单 NAL 包          -> 直接是一个完整 NAL
 *   24     STAP-A             -> 一个包里装多个 NAL，每个前面 2 字节长度
 *   25-27  STAP-B / MTAP      -> 云游戏不用，拒绝
 *   28     FU-A               -> 分片：payload[0]=FU indicator,
 *                               payload[1]=FU header(bit7=S起始, bit6=E结束,
 *                               bit0-4=真实 nal_type)
 *   29     FU-B               -> 不用，拒绝
 *
 * H.265 RTP 负载格式（RFC 7798）：暂未实现，`rtp_depay_init` 传
 * RTP_DEPAY_H265 会返回错误并说明原因——留接口不留坑。
 */
#ifndef PENYS_RTP_REORDER_H
#define PENYS_RTP_REORDER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 参数 ---- */
#define RTP_REORDER_SLOTS     64    /* 重排窗口深度（包数）。64 = 约 1 秒 @60fps */
#define RTP_REORDER_TIMEOUT_MS 60   /* 无进展超过此值即跳过缺失包（见 rtp_depay_tick）*/
#define RTP_FRAME_MAX         (512u * 1024u)  /* 单帧上限 512KB（1080p IDR 足够）*/
#define RTP_PACKET_MAX        1500u /* 单个 RTP 包（含头，不含 SRTP tag） */
#define RTP_NAL_MAX           64    /* 一帧内最多 NAL 数（STAP-A 可能塞好几个）*/

typedef enum {
    RTP_DEPAY_H264 = 1,
    RTP_DEPAY_H265 = 2
} rtp_codec;

typedef enum {
    RTP_OK              = 0,
    RTP_ERR_ARG         = -1,
    RTP_ERR_NOMEM       = -2,
    RTP_ERR_SHORT       = -3,   /* 包太短，连 RTP 头都不够 */
    RTP_ERR_VERSION     = -4,   /* RTP version != 2 */
    RTP_ERR_PAYLOAD     = -5,   /* 负载格式不认识 */
    RTP_ERR_OVERFLOW    = -6,   /* 帧超过 RTP_FRAME_MAX */
    RTP_ERR_UNSUPPORTED = -7    /* 编码/打包模式不支持 */
} rtp_status;

/* ---- 统计。全部只读，用于状态上报和看门狗 ---- */
typedef struct {
    uint32_t packets_rx;        /* 收到的 RTP 包（进了重排窗口的） */
    uint32_t packets_dup;       /* 重复包（已收过的 seq） */
    uint32_t packets_late;      /* 太旧，已滑出窗口 */
    uint32_t packets_bad;       /* 头非法/负载不认识 */
    uint32_t packets_lost;      /* 序号空洞推算出来的丢失数 */
    uint32_t frames_out;        /* 交给解码器的完整帧 */
    uint32_t frames_dropped;    /* 因缺片/溢出而丢掉的帧 */
    uint32_t keyframes_out;
    uint32_t last_seq;          /* 最后交付的 seq（诊断用） */
    uint32_t last_ts;           /* 最后交付的 RTP 时间戳 */
} rtp_stats;

/*
 * 重排 + 重组上下文。**很大**（约 520KB），必须放堆上或 static，
 * 不要放栈上。构造用 rtp_depay_init / rtp_depay_destroy。
 */
typedef struct rtp_depay rtp_depay;

/* codec: RTP_DEPAY_H264（唯一支持的）。
 * max_frame_bytes: 单帧上限，<= RTP_FRAME_MAX。传 0 用默认值。
 * 返回 NULL 表示失败（rtp_last_error() 说明原因）。 */
rtp_depay *rtp_depay_init(rtp_codec codec, uint32_t max_frame_bytes);
void rtp_depay_destroy(rtp_depay *d);

/* 复位（换流/重连时用）。保留配置，清空缓冲与序号状态。 */
void rtp_depay_reset(rtp_depay *d);

/*
 * 喂一个 RTP 包（**已由 SRTP 解密**，即标准 RTP：12 字节头 + 负载）。
 *
 * 内部会：校验版本/头长度 → 按 seq 放进重排窗口 → 按序推进 → 拆包 →
 * 凑帧。凑到完整一帧时，通过 `out` 返回该帧的 Annex-B 字节流
 * （每个 NAL 前加 00 00 00 01 起始码），并把 `*out_len` 设为长度。
 *
 * 返回值：
 *   RTP_OK 且 *out_len > 0  —— 有一帧可以交给解码器了（指针只在下次调用前有效）
 *   RTP_OK 且 *out_len == 0 —— 包已收下，但还没凑成完整帧
 *   < 0                     —— 这个包被拒绝了（不影响已缓冲的数据）
 *
 * 注意：一次调用**最多**产出一帧。如果一次 push 同时补齐了两帧（乱序恢复
 * 时可能发生），第二帧留在内部缓冲，下次调用 push 任意包时再吐出。
 * 用 rtp_depay_poll(d, out, &len) 主动取出已就绪的帧。
 */
rtp_status rtp_depay_push(rtp_depay *d, const uint8_t *pkt, size_t len,
                          uint32_t now_ms, const uint8_t **out, size_t *out_len);

/* 主动取一帧（如果内部已就绪）。用法同 push 的输出约定。 */
rtp_status rtp_depay_poll(rtp_depay *d, const uint8_t **out, size_t *out_len);

/*
 * **必须**由会话循环每 10~20ms 调一次。它做两件事：
 *   1. 取出已就绪但还没被取走的帧（如果 push 交出来时上层没接住）
 *   2. 重排超时：环里有包压着超过 RTP_REORDER_TIMEOUT_MS 没有按序推进时，
 *      跳过那个永远不来的包
 *
 * 为什么必须调用：只靠"环满 64 包"来兜底，在 60fps 下要等 1 秒才触发，
 * 丢一个包就冻 1 秒画面。60ms 超时把最坏情况压到单帧量级。
 *
 * now_ms：调用方的单调毫秒时钟（单调、可任意起点，只比较差值）。
 * 传到 push 里的那个时刻也来自同一个时钟（push 内部用 last_progress_ms
 * 记录进展时刻，所以 push 不需要额外传时间参数——见 push 的实现说明）。
 */
void rtp_depay_tick(rtp_depay *d, uint32_t now_ms,
                    const uint8_t **out, size_t *out_len);

/* 只读统计。 */
const rtp_stats *rtp_depay_stats(const rtp_depay *d);

/* 在丢包导致丢帧之后，是否处于「等下一个 IDR」的状态。
 * 上层可以用它决定是否给用户打「网络抖动」提示。 */
int rtp_depay_waiting_keyframe(const rtp_depay *d);

/* 最近一次失败的可读说明。永不返回 NULL。 */
const char *rtp_last_error(void);

/* ---- 给测试用的底层原语（生产代码不用）---- */

/* 解析 RTP 固定头，返回头长度（含 CSRC 与扩展），失败返回 -1。 */
int rtp_header_len(const uint8_t *pkt, size_t len);

/* 取 RTP 头里的字段。任何指针可为 NULL。失败返回 RTP_ERR_SHORT。 */
rtp_status rtp_parse_header(const uint8_t *pkt, size_t len,
                            uint16_t *seq, uint32_t *ts, uint32_t *ssrc,
                            int *marker, int *payload_type);

/* Annex-B 起始码长度（输出缓冲里每个 NAL 前面固定写 4 字节）。 */
#define RTP_ANNEXB_STARTCODE_LEN 4

#ifdef __cplusplus
}
#endif
#endif /* PENYS_RTP_REORDER_H */
