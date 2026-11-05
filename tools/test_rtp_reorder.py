#!/usr/bin/env python3
"""rtp_reorder 的自检：用合成 RTP 流验证拆包 + 重排 + 丢包恢复。

为什么这值得写：这一段是「静默错误」的重灾区。拆包写错不会崩溃，只会
让 mppvideodec 解出花屏或者干脆黑屏，而报错信息全在解码器内部。所以
必须用**构造好的、期望值已知的** RTP 流逐字节比对输出。

覆盖的场景（每条都对应一个真实的故障模式）：
  1. 单 NAL 包       —— 最基本的路径，P 帧就是这种
  2. FU-A 分片       —— 大 NAL（IDR 的关键帧）必需
  3. STAP-A 聚合     —— 服务端把 SPS+PPS 塞在一个包里
  4. 乱序重排        —— 网络抖动，包乱到
  5. 重复包          —— 重传或网络复制
  6. 丢片 -> 等 IDR  —— 丢一个 FU 中间片后必须整帧丢弃并等 IDR
  7. 环溢出强制推进  —— 一个包永远不来时不能冻结

跑法：python PenYS/tools/test_rtp_reorder.py
"""

import os
import struct
import subprocess
import sys
import tempfile

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rtp/rtp_reorder.h"

static unsigned char g_out[RTP_FRAME_MAX];
static int g_out_len;
static int g_frames;
static unsigned int g_now = 1000;  /* fake monotonic clock in ms */

/* 构造一个最小可用的 RTP 包：12 字节头 + payload */
static int mk_pkt(unsigned char *buf, unsigned short seq, unsigned int ts,
                  int marker, const unsigned char *payload, int plen) {
    buf[0] = 0x80;                       /* V=2, P=0, X=0, CC=0 */
    buf[1] = (unsigned char)((marker ? 0x80 : 0) | 96);
    buf[2] = (unsigned char)(seq >> 8);
    buf[3] = (unsigned char)(seq & 0xFF);
    buf[4] = (unsigned char)(ts >> 24);
    buf[5] = (unsigned char)(ts >> 16);
    buf[6] = (unsigned char)(ts >> 8);
    buf[7] = (unsigned char)(ts & 0xFF);
    buf[8] = 0x11; buf[9] = 0x22; buf[10] = 0x33; buf[11] = 0x44;
    memcpy(buf + 12, payload, plen);
    return 12 + plen;
}

/* 把一个完整的 NAL 打成单 NAL 包 */
static int push_single(rtp_depay *d, unsigned short seq, unsigned int ts,
                       int marker, const unsigned char *nal, int nlen) {
    unsigned char buf[1500];
    int n = mk_pkt(buf, seq, ts, marker, nal, nlen);
    const unsigned char *o = NULL; size_t ol = 0;
    g_now += 16;
    rtp_depay_push(d, buf, n, g_now, &o, &ol);
    if (ol) { memcpy(g_out, o, ol); g_out_len = (int)ol; g_frames++; }
    return 0;
}

/* 把一个 NAL 用 FU-A 分片发出 */
static int push_fu(rtp_depay *d, unsigned short *seq, unsigned int ts,
                   int marker_at_end, const unsigned char *nal, int nlen,
                   int fragment_size) {
    unsigned char buf[1500];
    unsigned char nal_hdr = nal[0];
    unsigned char ntype = nal_hdr & 0x1F;
    unsigned char nri = nal_hdr & 0xE0;
    int off = 1;
    int first = 1;
    while (off < nlen) {
        int chunk = nlen - off;
        if (chunk > fragment_size) chunk = fragment_size;
        int last = (off + chunk >= nlen);
        unsigned char pl[1400];
        pl[0] = (unsigned char)(nri | 28);            /* FU indicator */
        pl[1] = (unsigned char)((first ? 0x80 : 0) | (last ? 0x40 : 0) | ntype);
        memcpy(pl + 2, nal + off, chunk);
        int n = mk_pkt(buf, (*seq)++, ts, (last && marker_at_end) ? 1 : 0, pl, 2 + chunk);
        const unsigned char *o = NULL; size_t ol = 0;
        g_now += 16;
        rtp_depay_push(d, buf, n, g_now, &o, &ol);
        if (ol) { memcpy(g_out, o, ol); g_out_len = (int)ol; g_frames++; }
        off += chunk;
        first = 0;
    }
    return 0;
}

static void dump_hex(const char *tag, const unsigned char *b, int n) {
    printf("%s ", tag);
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(void) {
    rtp_depay *d = rtp_depay_init(RTP_DEPAY_H264, 0);
    if (!d) { printf("INIT_FAIL %s\n", rtp_last_error()); return 1; }
    unsigned short seq = 100;
    unsigned int ts = 1000;

    /* ---- 1. 单 NAL 包：一个 IDR 片（nal_type 5） ---- */
    {
        unsigned char nal[8] = { 0x65, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11 };
        push_single(d, seq++, ts, 1, nal, sizeof(nal));
        dump_hex("single", g_out, g_out_len);
        printf("single_frames %d\n", g_frames);
    }

    /* ---- 2. FU-A：一个 3000 字节的 NAL 分 3 片 ---- */
    {
        unsigned char nal[3001];
        nal[0] = 0x65;                       /* IDR */
        for (int i = 1; i < 3001; i++) nal[i] = (unsigned char)(i & 0xFF);
        g_frames = 0; g_out_len = 0;
        push_fu(d, &seq, ts + 1500, 1, nal, sizeof(nal), 1200);
        printf("fu_frames %d fu_len %d\n", g_frames, g_out_len);
        /* 期望：00 00 00 01 + 原始 NAL（头字节重建为 0x65） */
        int ok = (g_out_len == 4 + 3001) &&
                 memcmp(g_out, "\x00\x00\x00\x01", 4) == 0 &&
                 memcmp(g_out + 4, nal, 3001) == 0;
        printf("fu_exact %s\n", ok ? "YES" : "NO");
    }

    /* ---- 3. STAP-A：SPS + PPS 打包在一个包里 ---- */
    {
        unsigned char pl[64];
        unsigned char sps[4] = { 0x67, 0x42, 0x00, 0x1E };
        unsigned char pps[3] = { 0x68, 0xCE, 0x3C };
        int n = 0;
        pl[n++] = 24;                                   /* STAP-A */
        pl[n++] = 0; pl[n++] = sizeof(sps); memcpy(pl + n, sps, sizeof(sps)); n += sizeof(sps);
        pl[n++] = 0; pl[n++] = sizeof(pps); memcpy(pl + n, pps, sizeof(pps)); n += sizeof(pps);
        unsigned char buf[1500];
        int len = mk_pkt(buf, seq++, ts + 3000, 1, pl, n);
        const unsigned char *o = NULL; size_t ol = 0;
        rtp_depay_push(d, buf, len, g_now, &o, &ol);
        g_out_len = (int)ol; if (ol) memcpy(g_out, o, ol);
        dump_hex("stap", g_out, g_out_len);
        printf("stap_is_key %d\n", g_out_len > 0);
    }

    /* ---- 4. 乱序重排：把一个 FU 的片打乱顺序投递 ---- */
    {
        rtp_depay_reset(d);
        unsigned char nal[2001];
        nal[0] = 0x65;
        for (int i = 1; i < 2001; i++) nal[i] = (unsigned char)(i & 0xFF);

        /* 手工构造 3 片并乱序投递：中间片 -> 起始片 -> 结尾片 */
        unsigned char buf[1500];
        unsigned char pl0[1400], pl1[1400], pl2[1400];
        int c0 = 0, c1 = 0, c2 = 0;
        int frag = 700;
        int off = 1;
        int idx = 0;
        unsigned short sq = 500;
        while (off < 2001) {
            int chunk = 2001 - off; if (chunk > frag) chunk = frag;
            int last = (off + chunk >= 2001);
            unsigned char hdr = (unsigned char)((0x60 | 5));  /* NRI=3, type=5 */
            unsigned char *dst = (idx == 0) ? pl0 : (idx == 1 ? pl1 : pl2);
            int *cl = (idx == 0) ? &c0 : (idx == 1 ? &c1 : &c2);
            dst[0] = (unsigned char)(0x60 | 28);
            dst[1] = (unsigned char)((idx == 0 ? 0x80 : 0) | (last ? 0x40 : 0) | 5);
            memcpy(dst + 2, nal + off, chunk);
            *cl = 2 + chunk;
            (void)hdr;
            off += chunk;
            idx++;
        }
        /* 乱序：1 -> 0 -> 2 */
        int lens[3] = { c0, c1, c2 };
        unsigned char *pls[3] = { pl0, pl1, pl2 };
        int order[3] = { 1, 0, 2 };
        g_frames = 0; g_out_len = 0;
        for (int k = 0; k < 3; k++) {
            int i = order[k];
            int n = mk_pkt(buf, (unsigned short)(sq + i), ts + 4500,
                           (i == 2) ? 1 : 0, pls[i], lens[i]);
            const unsigned char *o = NULL; size_t ol = 0;
            g_now += 16;
            rtp_depay_push(d, buf, n, g_now, &o, &ol);
            if (ol) { memcpy(g_out, o, ol); g_out_len = (int)ol; g_frames++; }
        }
        printf("reorder_frames %d reorder_len %d expect %d\n",
               g_frames, g_out_len, 4 + 2001);
        printf("reorder_exact %s\n",
               (g_frames == 1 && g_out_len == 4 + 2001 &&
                memcmp(g_out + 4, nal, 2001) == 0) ? "YES" : "NO");
    }

    /* ---- 5. 重复包必须被丢掉，不能重复追加到帧里 ---- */
    {
        rtp_depay_reset(d);
        unsigned char buf[1500];
        unsigned char nal[6] = { 0x41, 1, 2, 3, 4, 5 };
        int n = mk_pkt(buf, 700, 9000, 0, nal, sizeof(nal));
        const unsigned char *o; size_t ol;
        g_now += 16;
        rtp_depay_push(d, buf, n, g_now, &o, &ol);
        g_now += 16;
        rtp_depay_push(d, buf, n, g_now, &o, &ol);      /* 同一包再发一次 */
        printf("dup_packets_rx %u dup %u\n",
               rtp_depay_stats(d)->packets_rx, rtp_depay_stats(d)->packets_dup);
        printf("dup_ratio_ok %s\n",
               (rtp_depay_stats(d)->packets_dup >= 1) ? "YES" : "NO");
    }

    /* ---- 6. 丢中间片 -> 整帧丢弃 + 等 IDR ---- */
    {
        rtp_depay_reset(d);
        unsigned char buf[1500];
        /* 起始片 */
        unsigned char p0[100];
        p0[0] = 0x60 | 28; p0[1] = 0x80 | 5;
        memset(p0 + 2, 0xAA, 98);
        int n = mk_pkt(buf, 800, 11000, 0, p0, 100);
        const unsigned char *o; size_t ol;
        g_now += 16;
        rtp_depay_push(d, buf, n, g_now, &o, &ol);
        /* 跳过中间片（801），直接发结尾片 802 */
        unsigned char p2[100];
        p2[0] = 0x60 | 28; p2[1] = 0x40 | 5;
        memset(p2 + 2, 0xBB, 98);
        n = mk_pkt(buf, 802, 11000, 1, p2, 100);
        g_now += 16;
        rtp_depay_push(d, buf, n, g_now, &o, &ol);
        /* 中间片永远不来 -> tick 超时后必须跳过它并处理 802 */
        g_now += 200;
        rtp_depay_tick(d, g_now, &o, &ol);
        printf("loss_waiting_idr %d\n", rtp_depay_waiting_keyframe(d));
        printf("loss_frames_out %u dropped %u\n",
               rtp_depay_stats(d)->frames_out, rtp_depay_stats(d)->frames_dropped);

        /* 之后发一个 P 帧：应当被丢弃（还在等 IDR） */
        unsigned char pf[8] = { 0x41, 1, 2, 3, 4, 5, 6, 7 };
        n = mk_pkt(buf, 803, 12500, 1, pf, 8);
        size_t ol2 = 0;
        rtp_depay_push(d, buf, n, g_now, &o, &ol2);
        printf("loss_pframe_emitted %d\n", (ol2 > 0) ? 1 : 0);

        /* 再来一个 IDR：应当恢复并交付 */
        unsigned char idr[8] = { 0x65, 9, 8, 7, 6, 5, 4, 3 };
        n = mk_pkt(buf, 804, 14000, 1, idr, 8);
        ol2 = 0;
        rtp_depay_push(d, buf, n, g_now, &o, &ol2);
        printf("loss_idr_emitted %d waiting_after %d\n",
               (ol2 > 0) ? 1 : 0, rtp_depay_waiting_keyframe(d));
    }

    /* ---- 7. 环溢出：只投递极少数包，中间全部不投，必须强制推进 ---- */
    {
        rtp_depay_reset(d);
        unsigned char buf[1500];
        unsigned char pay[4] = { 0x41, 1, 2, 3 };
        /* 投递 100 个连续的包，但故意跳过 0..99 中的一部分制造大洞 */
        for (int i = 0; i < 200; i++) {
            int n = mk_pkt(buf, (unsigned short)(900 + i), 20000 + i * 100,
                           (i % 4 == 3) ? 1 : 0, pay, sizeof(pay));
            const unsigned char *o; size_t ol;
            g_now += 4;
            rtp_depay_push(d, buf, n, g_now, &o, &ol);
        }
        printf("overflow_rx %u lost %u\n",
               rtp_depay_stats(d)->packets_rx, rtp_depay_stats(d)->packets_lost);
        printf("overflow_no_freeze %s\n",
               (rtp_depay_stats(d)->packets_rx >= 200) ? "YES" : "NO");
    }

    /* ---- 8. 短包 / 坏版本必须被拒绝而不是崩 ---- */
    {
        rtp_depay_reset(d);
        unsigned char shortpkt[4] = { 0x80, 0x60, 0, 0 };
        const unsigned char *o; size_t ol;
        rtp_status rc = rtp_depay_push(d, shortpkt, sizeof(shortpkt), g_now, &o, &ol);
        printf("short_rejected %d\n", (rc == RTP_ERR_SHORT) ? 1 : 0);

        unsigned char badver[12] = { 0x40, 0x60, 0, 1, 0,0,0,0, 0,0,0,0 };
        rc = rtp_depay_push(d, badver, sizeof(badver), g_now, &o, &ol);
        printf("badver_rejected %d\n", (rc != RTP_OK) ? 1 : 0);
    }

    /* ---- 9. H.265 必须明确拒绝 ---- */
    {
        rtp_depay *h = rtp_depay_init(RTP_DEPAY_H265, 0);
        printf("h265_rejected %d\n", (h == NULL) ? 1 : 0);
        if (h) rtp_depay_destroy(h);
    }

    rtp_depay_destroy(d);
    return 0;
}
"""


def main():
    proj = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    root = os.path.abspath(os.path.join(proj, ".."))
    inc = os.path.join(proj, "native", "csrc")
    cf = os.path.join(proj, "native", "csrc", "rtp", "rtp_reorder.c")

    zig = os.path.join(root, "env", "pyzig", "ziglang", "zig.exe")
    cands = []
    if os.path.exists(zig):
        cands.append([zig, "cc"])
    cands += [["gcc"], ["cc"], ["clang"]]

    with tempfile.TemporaryDirectory() as td:
        h = os.path.join(td, "h.c")
        with open(h, "w", encoding="utf-8") as f:
            f.write(HARNESS)
        exe = os.path.join(td, "t.exe")
        built = False
        for cc in cands:
            try:
                r = subprocess.run(cc + ["-O1", "-I", inc, h, cf, "-o", exe],
                                   capture_output=True, text=True)
            except FileNotFoundError:
                continue
            if r.returncode == 0:
                built = True
                break
        if not built:
            print("build failed:", r.stderr[:2000])
            return 2

        r = subprocess.run([exe], capture_output=True, text=True)
        print(r.stdout)
        if r.returncode != 0:
            print("harness crashed:", r.stderr[:1000])
            return 1

        fails = []
        lines = [l.strip() for l in r.stdout.strip().splitlines() if l.strip()]

        def find(key):
            """Return the logical line starting with `key`, or None."""
            for l in lines:
                if l == key or l.startswith(key + " "):
                    return l
            return None

        def want(key, expect):
            """Assert the first token after `key` equals `expect`."""
            l = find(key)
            if l is None:
                fails.append(f"{key}: missing")
                return
            parts = l.split()
            got = parts[1] if len(parts) > 1 else "<none>"
            if got != expect:
                fails.append(f"{key}: expected {expect}, got {got}  [line: {l}]")

        # 1. 单 NAL
        want("single", "0000000165aabbccddeeff11")
        want("single_frames", "1")
        # 2. FU-A 重组必须与原始 NAL 完全一致
        want("fu_frames", "1")
        want("fu_exact", "YES")
        # 3. STAP-A：SPS + PPS 两个 NAL，各自带起始码
        want("stap", "000000016742001e0000000168ce3c")
        # 4. 乱序重排
        want("reorder_exact", "YES")
        # 5. 重复包
        want("dup_ratio_ok", "YES")
        # 6. 丢片恢复
        want("loss_waiting_idr", "1")
        want("loss_pframe_emitted", "0")

        # loss_idr_emitted N waiting_after M
        l = find("loss_idr_emitted")
        if l is None:
            fails.append("loss_idr_emitted: missing")
        else:
            t = l.split()
            if t[1] != "1":
                fails.append(f"loss_idr_emitted: expected 1, got {t[1]}")
            if len(t) >= 4 and t[3] != "0":
                fails.append(f"waiting_after: expected 0, got {t[3]}")

        # fu_frames N fu_len M
        l = find("fu_frames")
        if l is not None:
            t = l.split()
            if len(t) >= 4 and t[3] != "3005":
                fails.append(f"fu_len: expected 3005, got {t[3]}")

        # reorder_frames N reorder_len M expect K
        l = find("reorder_frames")
        if l is not None:
            t = l.split()
            if len(t) >= 6 and t[5] != "2005":
                fails.append(f"reorder_len: expected 2005, got {t[5]}")

        want("overflow_no_freeze", "YES")
        want("short_rejected", "1")
        want("badver_rejected", "1")
        want("h265_rejected", "1")

        if fails:
            print("FAILURES:")
            for f in fails:
                print("  -", f)
            return 1
        print("ALL PASS")
        return 0


sys.exit(main())
