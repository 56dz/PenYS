/*
 * rtp_reorder.c -- RTP 重排 + H.264 拆包 + Annex-B 组帧。
 *
 * 数据流（读代码时按这个顺序走）：
 *
 *   rtp_depay_push
 *     ├─ rtp_parse_header            校验版本/长度
 *     ├─ warmup 判断                 流启动时先攒 2 个包，好让乱序的起点归位
 *     ├─ 太旧判断                    已经交付过的 seq 直接丢
 *     ├─ slot_put                    按 seq 放进重排环
 *     └─ drain(now)                  按序推进 -> 拆包 -> 凑帧 -> 输出
 *          └─ handle_payload         单 NAL / STAP-A / FU-A
 *
 *   rtp_depay_tick(now_ms)           **必须**由会话循环每 10~20ms 调一次：
 *                                    重排超时后才能强制推进，否则画面会冻住
 *
 * 五个必须理解的实现决定（改之前先读完，每条都是踩过坑的结论）：
 *
 * 【1】重排环是定长数组 + 占用位图，不是链表也不是堆。64 槽覆盖约 1 秒
 *      @60fps。WebRTC 实测乱序很少超过 3~5 个包，64 远远够用。
 *
 * 【2】**重排超时必须有，光靠环满不够**。环满（64 个包）在 60fps 下要
 *     1 秒才触发；而丢一个包之后如果只是干等，画面会冻 1 秒——肉眼可见
 *     的卡顿。所以 rtp_depay_tick() 在**没有进展超过 RTP_REORDER_TIMEOUT_MS
 *     (60ms)** 且环里还有包时，强制把 expecting 跳到最老的未收 seq。
 *     这是本模块唯一需要外部时钟的地方，所以时钟由调用方传入
 *     （可测试性：测试里喂假时间戳就能精确控制超时）。
 *
 * 【3】丢一帧之后**进入等 IDR 状态**，但不停止解析包。
 *     这是 H.264 的解码器特性：缺参考帧之后的 P 帧解出来是绿的/花的，
 *     而且会污染参考帧列表，直到下一个 IDR 才恢复。所以：
 *       - 检测到缺片 -> 丢弃当前未完成帧 + need_keyframe=1
 *       - need_keyframe 期间**仍然解析**（为了统计和找 IDR），
 *         但只交付 IDR 帧，中间的 P 帧一律丢
 *     恢复时间 = 到下一个 IDR 为止（云游戏通常 1~2 秒一个 IDR）。
 *
 * 【4】Annex-B 而不是 AVCC。每个 NAL 前写 4 字节 00 00 00 01。
 *     mppvideodec 接在 h264parse 上两种都吃，但 Annex-B 不需要维护
 *     SPS/PPS 的长度表。SPS/PPS 本身作为普通 NAL 一起塞进帧里
 *     （服务端会把它们放在 IDR 前的 STAP-A 里），h264parse 自动提取。
 *
 * 【5】**待交付的帧绝不能导致丢包**。frame 缓冲只有一份，所以
 *     「凑出一帧就立刻返回、剩下的包留在环里下次处理」。曾经写过
 *     「入口发现 frame_ready 就直接返回它、把本次进来的包丢掉」——
 *     结果是只要上层慢一拍就静默丢包（自检 test 7 抓到 3/200 丢失）。
 *     现在的约定：每次都先把已就绪的帧交出去，环里的包等下次 push。
 */

#include "rtp_reorder.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 内部结构 ---------------- */

struct rtp_depay {
    rtp_codec codec;
    uint32_t  max_frame;

    /* --- 重排环 --- */
    struct {
        uint8_t  data[RTP_PACKET_MAX];
        uint16_t len;
        uint16_t seq;
    } ring[RTP_REORDER_SLOTS];
    uint8_t  ring_used[RTP_REORDER_SLOTS];
    int      expecting_valid;
    uint16_t expecting;        /* 下一个期望的 seq */

    /* --- 启动/超时 --- */
    int      delivered_any;    /* 是否已经按序交付过至少一个包 */
    uint32_t last_progress_ms; /* 上次成功按序推进的时间 */

    /* --- 重组缓冲 --- */
    uint8_t *frame;
    uint32_t frame_len;
    int      frame_open;
    uint32_t frame_ts;
    int      frame_is_key;

    /* FU-A 状态 */
    int      fu_active;
    uint8_t  fu_nal_type;
    uint32_t fu_ts;

    /* 丢包后等 IDR */
    int      need_keyframe;

    /* 已凑好但还没被上层取走的帧 */
    int      frame_ready;

    rtp_stats stats;
    char      err[128];
};

static const char *g_err = "";

static void set_err(rtp_depay *d, const char *msg) {
    if (!d) { g_err = msg; return; }
    size_t n = strlen(msg);
    if (n >= sizeof(d->err)) n = sizeof(d->err) - 1;
    memcpy(d->err, msg, n);
    d->err[n] = 0;
    g_err = d->err;
}

const char *rtp_last_error(void) { return g_err ? g_err : ""; }

/* ---------------- RTP 头解析 ---------------- */

int rtp_header_len(const uint8_t *pkt, size_t len) {
    if (!pkt || len < 12) return -1;
    if ((pkt[0] >> 6) != 2) return -1;
    size_t h = 12 + 4u * (size_t)(pkt[0] & 0x0F);
    if (len < h) return -1;
    if (pkt[0] & 0x10) {
        if (len < h + 4) return -1;
        size_t ext_words = ((size_t)pkt[h + 2] << 8) | pkt[h + 3];
        h += 4 + 4 * ext_words;
        if (len < h) return -1;
    }
    return (int)h;
}

rtp_status rtp_parse_header(const uint8_t *pkt, size_t len,
                            uint16_t *seq, uint32_t *ts, uint32_t *ssrc,
                            int *marker, int *payload_type) {
    if (!pkt || len < 12) return RTP_ERR_SHORT;
    if ((pkt[0] >> 6) != 2) return RTP_ERR_VERSION;
    if (seq)  *seq  = (uint16_t)(((uint16_t)pkt[2] << 8) | pkt[3]);
    if (ts)   *ts   = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                      ((uint32_t)pkt[6] << 8)  |  (uint32_t)pkt[7];
    if (ssrc) *ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
                      ((uint32_t)pkt[10] << 8) |  (uint32_t)pkt[11];
    if (marker)       *marker = (pkt[1] >> 7) & 1;
    if (payload_type) *payload_type = pkt[1] & 0x7F;
    return RTP_OK;
}

/* ---------------- 生命周期 ---------------- */

rtp_depay *rtp_depay_init(rtp_codec codec, uint32_t max_frame_bytes) {
    if (codec != RTP_DEPAY_H264) {
        /* H.265 的拆包规则（RFC 7798）与 H.264 差别不小（AP/DSC/FU 三种，
         * FU 头 2 字节），没有真机流量可对照时写出来只是假装支持。
         * 明确拒绝比悄悄解错好。云原神实测下发的就是 H.264（codec_type=1）。 */
        g_err = (codec == RTP_DEPAY_H265)
            ? "H.265 depacketisation not implemented (RFC 7798 AP/DSC/FU differ from H.264); "
              "mihoyo serves H.264 (codec_type=1)"
            : "unsupported codec";
        return NULL;
    }
    uint32_t mf = max_frame_bytes ? max_frame_bytes : RTP_FRAME_MAX;
    if (mf > RTP_FRAME_MAX) mf = RTP_FRAME_MAX;
    if (mf < 4096) mf = 4096;

    rtp_depay *d = (rtp_depay *)calloc(1, sizeof(*d));
    if (!d) { g_err = "out of memory (context)"; return NULL; }
    d->frame = (uint8_t *)malloc(mf);
    if (!d->frame) { free(d); g_err = "out of memory (frame buffer)"; return NULL; }
    d->codec = codec;
    d->max_frame = mf;
    rtp_depay_reset(d);
    g_err = "";
    return d;
}

void rtp_depay_destroy(rtp_depay *d) {
    if (!d) return;
    free(d->frame);
    free(d);
}

void rtp_depay_reset(rtp_depay *d) {
    if (!d) return;
    memset(d->ring_used, 0, sizeof(d->ring_used));
    d->expecting_valid = 0;
    d->expecting = 0;
    d->delivered_any = 0;
    d->last_progress_ms = 0;
    d->frame_len = 0;
    d->frame_open = 0;
    d->frame_ts = 0;
    d->frame_is_key = 0;
    d->fu_active = 0;
    d->fu_nal_type = 0;
    d->fu_ts = 0;
    d->need_keyframe = 0;
    d->frame_ready = 0;
    memset(&d->stats, 0, sizeof(d->stats));
    d->err[0] = 0;
    g_err = "";
}

const rtp_stats *rtp_depay_stats(const rtp_depay *d) {
    static const rtp_stats zero;
    return d ? &d->stats : &zero;
}

int rtp_depay_waiting_keyframe(const rtp_depay *d) {
    return d ? d->need_keyframe : 0;
}

/* ---------------- 帧缓冲 ---------------- */

static int frame_append_nal(rtp_depay *d, const uint8_t *nal, uint32_t len) {
    if (len == 0) return 0;
    if (d->frame_len + RTP_ANNEXB_STARTCODE_LEN + len > d->max_frame) {
        set_err(d, "frame overflow (single NAL too large)");
        return RTP_ERR_OVERFLOW;
    }
    if (!d->frame_open) {
        d->frame_open = 1;
        d->frame_len = 0;
    }
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    memcpy(d->frame + d->frame_len, sc, 4);
    d->frame_len += 4;
    memcpy(d->frame + d->frame_len, nal, len);
    d->frame_len += len;

    int t = nal[0] & 0x1F;
    if (t == 5 || t == 7) d->frame_is_key = 1;
    return 0;
}

/* 丢弃当前未完成的帧（丢包造成的，要计入 frames_dropped）。 */
static void frame_abort(rtp_depay *d) {
    if (d->frame_open && d->frame_len) d->stats.frames_dropped++;
    d->frame_open = 0;
    d->frame_len = 0;
    d->frame_is_key = 0;
    d->fu_active = 0;
}

/* 把当前帧标为就绪，供上层取走。
 * need_keyframe 期间：P 帧丢弃，IDR 帧解禁。 */
static void frame_close(rtp_depay *d) {
    if (!d->frame_open || d->frame_len == 0) { d->frame_open = 0; return; }

    if (d->need_keyframe && !d->frame_is_key) {
        d->frame_open = 0;
        d->frame_len = 0;
        d->frame_is_key = 0;
        d->stats.frames_dropped++;
        return;
    }
    if (d->need_keyframe && d->frame_is_key) d->need_keyframe = 0;

    d->frame_ready = 1;
    d->stats.frames_out++;
    if (d->frame_is_key) d->stats.keyframes_out++;
    d->stats.last_ts = d->frame_ts;
}

/* ---------------- H.264 负载分派 ---------------- */

static int handle_payload(rtp_depay *d, const uint8_t *p, uint32_t n,
                          uint32_t ts, int marker) {
    (void)marker;
    if (n < 1) return RTP_ERR_PAYLOAD;
    uint8_t nal_type = p[0] & 0x1F;

    /* 时间戳变了说明上一帧已经结束（有些服务端不发 marker，或者 marker
     * 丢在了丢失的包里）。先结帧，再开始新帧——不能在这里直接返回，
     * 否则这个包就丢了。 */
    if (d->frame_open && ts != d->frame_ts) {
        frame_close(d);
        d->frame_open = 0;
        d->frame_len = 0;
        d->frame_is_key = 0;
    }

    if (nal_type == 24) {
        /* STAP-A：一个包里若干 NAL，每个前面 2 字节长度。
         * 服务端用它把 SPS+PPS(+SEI) 塞在一个包里，紧挨着 IDR。 */
        uint32_t off = 1;
        while (off + 2 <= n) {
            uint32_t sz = ((uint32_t)p[off] << 8) | p[off + 1];
            off += 2;
            if (sz == 0 || off + sz > n) {
                set_err(d, "STAP-A truncated");
                return RTP_ERR_PAYLOAD;
            }
            if (!d->frame_open) { d->frame_open = 1; d->frame_len = 0; d->frame_ts = ts; }
            int rc = frame_append_nal(d, p + off, sz);
            if (rc < 0) return rc;
            off += sz;
        }
        return RTP_OK;
    }

    if (nal_type == 28) {
        /* FU-A：2 字节头
         *   p[0] = FU indicator（F|NRI|type=28）
         *   p[1] = FU header（S|E|R|真实 nal_type） */
        if (n < 3) return RTP_ERR_SHORT;
        int start = (p[1] >> 7) & 1;
        int end   = (p[1] >> 6) & 1;
        uint8_t real_type = p[1] & 0x1F;
        const uint8_t *frag = p + 2;
        uint32_t frag_len = n - 2;

        /* 重建 NAL 头：保留原包的 F|NRI，type 换成真实类型 */
        uint8_t nal_hdr = (uint8_t)((p[0] & 0xE0) | real_type);

        if (start) {
            if (d->fu_active) {
                /* 上一个 FU 还没收到结尾就来了新起始 -> 上一帧废了 */
                set_err(d, "FU-A restarted before end fragment");
                frame_abort(d);
                d->stats.frames_dropped++;
                d->need_keyframe = 1;
            }
            if (!d->frame_open) { d->frame_open = 1; d->frame_len = 0; d->frame_ts = ts; }
            d->fu_active = 1;
            d->fu_nal_type = real_type;
            d->fu_ts = ts;

            if (d->frame_len + 4 + 1 + frag_len > d->max_frame) {
                set_err(d, "frame overflow (FU-A start)");
                frame_abort(d);
                return RTP_ERR_OVERFLOW;
            }
            static const uint8_t sc[4] = { 0, 0, 0, 1 };
            memcpy(d->frame + d->frame_len, sc, 4);
            d->frame_len += 4;
            d->frame[d->frame_len] = nal_hdr;
            d->frame_len += 1;
            memcpy(d->frame + d->frame_len, frag, frag_len);
            d->frame_len += frag_len;
            if (real_type == 5 || real_type == 7) d->frame_is_key = 1;
            return RTP_OK;
        }

        if (!d->fu_active) {
            /* 中间片但起始片没来（丢包）。整个 NAL 不可恢复 -> 整帧作废等 IDR。 */
            set_err(d, "FU-A middle fragment without start (packet loss)");
            d->stats.packets_bad++;
            if (d->frame_open) {
                frame_abort(d);
                d->need_keyframe = 1;
            }
            return RTP_OK;   /* 网络丢包不算格式错误 */
        }

        if (d->fu_nal_type != real_type || d->fu_ts != ts) {
            set_err(d, "FU-A fragment mismatch (type/ts changed mid-fragment)");
            frame_abort(d);
            d->stats.frames_dropped++;
            d->need_keyframe = 1;
            return RTP_OK;
        }

        if (d->frame_len + frag_len > d->max_frame) {
            set_err(d, "frame overflow (FU-A middle)");
            frame_abort(d);
            d->need_keyframe = 1;
            return RTP_ERR_OVERFLOW;
        }
        memcpy(d->frame + d->frame_len, frag, frag_len);
        d->frame_len += frag_len;

        if (end) d->fu_active = 0;
        return RTP_OK;
    }

    if (nal_type == 25 || nal_type == 26 || nal_type == 27 || nal_type == 29) {
        /* STAP-B / MTAP16 / MTAP24 / FU-B：都依赖 RTP 包的 DON 字段，
         * 云游戏不用。明确拒绝，不要静默当单 NAL 处理。 */
        set_err(d, "interleaved/MTAP H.264 packetisation not supported");
        d->stats.packets_bad++;
        return RTP_ERR_UNSUPPORTED;
    }

    if (nal_type >= 1 && nal_type <= 23) {
        if (!d->frame_open) { d->frame_open = 1; d->frame_len = 0; d->frame_ts = ts; }
        return frame_append_nal(d, p, n);
    }

    set_err(d, "unknown H.264 RTP payload type");
    d->stats.packets_bad++;
    return RTP_ERR_PAYLOAD;
}

/* ---------------- 重排环 ---------------- */

static int seq_newer(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

static int slot_index(uint16_t seq) {
    return (int)(seq % RTP_REORDER_SLOTS);
}

static int slot_put(rtp_depay *d, uint16_t seq, const uint8_t *pkt, uint32_t len) {
    int i = slot_index(seq);
    if (d->ring_used[i]) {
        if (d->ring[i].seq == seq) {
            d->stats.packets_dup++;
            return 0;
        }
        /* 槽被别的 seq 占了（seq 差 64 的整数倍，或环回收慢）。
         * 保留新的、丢掉旧的：旧的那个大概率已经超时被放弃了。 */
        if (seq_newer(d->ring[i].seq, seq)) {
            d->stats.packets_late++;
            return 0;
        }
        d->stats.packets_late++;
    }
    d->ring[i].seq = seq;
    d->ring[i].len = (uint16_t)len;
    memcpy(d->ring[i].data, pkt, len);
    d->ring_used[i] = 1;
    return 1;
}

static int slot_take_in_order(rtp_depay *d, uint16_t *seq,
                             const uint8_t **pkt, uint32_t *len) {
    if (!d->expecting_valid) return 0;
    int i = slot_index(d->expecting);
    if (!d->ring_used[i] || d->ring[i].seq != d->expecting) return 0;
    *seq = d->ring[i].seq;
    *pkt = d->ring[i].data;
    *len = d->ring[i].len;
    d->ring_used[i] = 0;
    d->expecting = (uint16_t)(d->expecting + 1);
    return 1;
}

static int slot_oldest(rtp_depay *d, uint16_t *out) {
    int found = 0;
    uint16_t best = 0;
    for (int i = 0; i < RTP_REORDER_SLOTS; i++) {
        if (!d->ring_used[i]) continue;
        if (!found || seq_newer(best, d->ring[i].seq)) {
            best = d->ring[i].seq;
            found = 1;
        }
    }
    if (found) *out = best;
    return found;
}

static int ring_count(rtp_depay *d) {
    int n = 0;
    for (int i = 0; i < RTP_REORDER_SLOTS; i++) if (d->ring_used[i]) n++;
    return n;
}

/* ---------------- 主路径 ---------------- */

/* 按序推进，最多产出一帧。
 * 返回时 *emitted 表示 out/out_len 是否有效。 */
static void drain(rtp_depay *d, uint32_t now_ms,
                  const uint8_t **out, size_t *out_len) {
    /* 启动阶段：先攒两个包再决定 expecting，这样第一个包乱序时
     * （比如先收到中间片）不会把窗口起点定错。
     * marker 包例外——它本身就是一帧的收尾，等下去只会白等。 */
    if (!d->delivered_any && ring_count(d) < 2) {
        uint16_t s; int is_marker = 0;
        if (slot_oldest(d, &s)) {
            int i = slot_index(s);
            const uint8_t *p = d->ring[i].data;
            uint16_t n = d->ring[i].len;
            if (n >= 2) is_marker = (p[1] >> 7) & 1;
        }
        if (!is_marker) return;
    }

    if (!d->delivered_any) {
        /* 第一次真正开工：把起点定在环里最老的未收 seq */
        uint16_t oldest;
        if (slot_oldest(d, &oldest)) {
            d->expecting = oldest;
            d->expecting_valid = 1;
        }
    }

    for (;;) {
        /* 先交付已就绪的帧：frame 缓冲只有一份，交完就 break，
         * 剩下的包留在环里等下次 push。
         * 注意是 break 不是 return——return 会跳过下面的 *out 赋值，
         * 帧就静默消失了（自检抓到的第 6 个坑）。 */
        if (d->frame_ready) break;

        uint16_t s;
        const uint8_t *p;
        uint32_t n;
        if (!slot_take_in_order(d, &s, &p, &n)) break;

        d->stats.last_seq = s;
        d->delivered_any = 1;
        d->last_progress_ms = now_ms;

        int hl = rtp_header_len(p, n);
        if (hl < 0) { d->stats.packets_bad++; continue; }

        int mk = (p[1] >> 7) & 1;
        uint32_t pts = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                       ((uint32_t)p[6] << 8) | (uint32_t)p[7];

        if (handle_payload(d, p + hl, n - (uint32_t)hl, pts, mk) < 0) {
            set_err(d, "payload rejected");
            /* 单包格式错误不该毁掉整帧，继续后面的包 */
        }

        if (mk) frame_close(d);
    }

    if (d->frame_ready) {
        *out = d->frame;
        *out_len = d->frame_len;
        d->frame_ready = 0;
        /* 必须清空重组状态，否则下一帧的 ts 判断会因为 frame_open 残留
         * 而把旧内容重新 mark 成 ready —— 同一帧被交付两次
         * （自检抓到的第 7 个坑：fu_frames 期望 1 实际 2）。 */
        d->frame_open = 0;
        d->frame_len = 0;
        d->frame_is_key = 0;
    }
}

rtp_status rtp_depay_push(rtp_depay *d, const uint8_t *pkt, size_t len,
                          uint32_t now_ms, const uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!d || !pkt || !out || !out_len) { set_err(d, "null argument"); return RTP_ERR_ARG; }
    if (len < 12) {
        d->stats.packets_bad++;
        set_err(d, "packet shorter than RTP header");
        return RTP_ERR_SHORT;
    }

    uint16_t seq = 0;
    uint32_t ts = 0;
    int marker = 0;
    rtp_status rc = rtp_parse_header(pkt, len, &seq, &ts, NULL, &marker, NULL);
    if (rc != RTP_OK) { d->stats.packets_bad++; set_err(d, "bad RTP header"); return rc; }

    int hl = rtp_header_len(pkt, len);
    if (hl < 0) { d->stats.packets_bad++; set_err(d, "bad RTP header length"); return RTP_ERR_SHORT; }

    /* 上一帧还没被取走：丢掉它（上层没接，责任在上层），
     * 但**不能因此丢掉本次进来的包**——这是自检抓到的第 5 个坑。 */
    if (d->frame_ready) {
        d->stats.frames_dropped++;
        d->frame_ready = 0;
    }

    d->stats.packets_rx++;

    if (!d->expecting_valid) {
        d->expecting = seq;
        d->expecting_valid = 1;
    } else if (d->delivered_any) {
        /* 已经交付过：比 expecting-1 还旧的包直接丢 */
        if (!seq_newer(seq, (uint16_t)(d->expecting - 1)) && seq != d->expecting) {
            d->stats.packets_late++;
            drain(d, now_ms, out, out_len);
            return RTP_OK;
        }
    }

    slot_put(d, seq, pkt, (uint32_t)len);

    /* 环满 -> 强制推进（死等会让画面冻住） */
    if (ring_count(d) >= RTP_REORDER_SLOTS) {
        uint16_t oldest;
        if (slot_oldest(d, &oldest) && seq_newer(oldest, d->expecting)) {
            d->stats.packets_lost += (uint16_t)(oldest - d->expecting);
            d->expecting = oldest;
            if (d->frame_open) {
                frame_abort(d);
                d->need_keyframe = 1;
            }
        }
    }

    drain(d, now_ms, out, out_len);
    return RTP_OK;
}

void rtp_depay_tick(rtp_depay *d, uint32_t now_ms,
                    const uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!d || !out || !out_len) return;

    if (d->frame_ready) {
        *out = d->frame;
        *out_len = d->frame_len;
        d->frame_ready = 0;
        return;
    }
    if (!d->expecting_valid || ring_count(d) == 0) return;

    /* 有包压着但超过 60ms 没有按序推进 -> 那个包不会来了，跳过去。
     * 60ms ≈ 3~4 帧 @60fps：比单帧间隔长得多（不会误判正常抖动），
     * 又比人眼能察觉的卡顿门槛（~100ms）短。 */
    if ((uint32_t)(now_ms - d->last_progress_ms) < RTP_REORDER_TIMEOUT_MS) return;

    uint16_t oldest;
    if (!slot_oldest(d, &oldest)) return;
    if (!seq_newer(oldest, d->expecting) && oldest != d->expecting) return;

    if (oldest != d->expecting) {
        d->stats.packets_lost += (uint16_t)(oldest - d->expecting);
        d->expecting = oldest;
        if (d->frame_open) {
            frame_abort(d);
            d->need_keyframe = 1;
        }
    }
    d->last_progress_ms = now_ms;
    drain(d, now_ms, out, out_len);
}

rtp_status rtp_depay_poll(rtp_depay *d, const uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!d || !out || !out_len) return RTP_ERR_ARG;
    if (d->frame_ready) {
        *out = d->frame;
        *out_len = d->frame_len;
        d->frame_ready = 0;
    }
    return RTP_OK;
}
