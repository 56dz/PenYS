/*
 * srtp.h -- SRTP/SRTCP 加解密核心（RFC 3711，profile: SRTP_AES128_CM_SHA1_80）
 *
 * 适用场景：有道词典笔 X6 Pro（RK3562 / aarch64 / Buildroot / OpenSSL 1.1.1l）
 * 上的云游戏客户端。设计目标是能被 GStreamer appsrc/appsink 或 SDL 风格循环
 * 逐包调用：
 *
 *   - **零堆分配**：所有 per-packet 缓冲都在栈上（上限 SRTP_MAX_PACKET）。
 *     唯一的堆分配发生在 srtp_init()（4 个 OpenSSL 句柄），不在热路径上，
 *     因此不会有每帧 malloc 抖动。
 *   - **原地加解密**：入参与出参是同一块缓冲，不额外拷贝。
 *   - **单线程上下文**：一个 srtp_ctx 只允许一个线程使用（GStreamer 里就是
 *     appsink 的 streaming 线程）。
 *
 * 实现内容（RFC 3711）：
 *   $4.1.1  AES-128 分段整数计数器模式（AES-CM）
 *   $4.2.1  HMAC-SHA1，截断到前 80 bit（10 字节）
 *   $4.3.1  密钥派生（master key/salt -> session enc/auth/salt），标签 0..5
 *   $3.3.1  包索引（ROC/SEQ）判定，附录 A 伪码
 *   $3.3.2  重放保护（64 包滑动窗口）
 *   $3.4    SRTCP（独立 session key，31-bit index + E flag）
 *
 * 不实现（并在 srtp_init 中显式拒绝或注释说明）：
 *   - AES-f8 模式（RFC 3711 $4.1.2）：本 profile 不用，附录 B.1 的向量属于它。
 *   - 非零 key_derivation_rate：DTLS-SRTP（RFC 5764）与 SDES 协商出来的
 *     kdr 恒为 0，RFC 3711 $5.3 的默认值也是 0；传非 0 会在 init 处返回
 *     SRTP_ERR_ARG，不会静默降级。
 *   - MKI 字段：本部署不使用（MKI indicator = 0）。
 *
 * 线程安全：非线程安全。每个方向一个 srtp_ctx，各自单线程使用。
 */

#ifndef PENYS_RTP_SRTP_H
#define PENYS_RTP_SRTP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 常量 */

#define SRTP_KEY_LEN        16u   /* n_e: session encryption key, 128 bit   */
#define SRTP_SALT_LEN       14u   /* n_s: session salt key, 112 bit          */
#define SRTP_SALT_LEN_MIN   12u   /* master salt 常见长度（WebRTC/DTLS-SRTP）*/
#define SRTP_AUTH_KEY_LEN   20u   /* n_a: session authentication key, 160bit */
#define SRTP_TAG_LEN        10u   /* n_tag: 80 bit                           */
#define SRTP_MAX_PACKET     1600u /* MTU 上限；所有栈缓冲按此尺寸           */
#define SRTP_REPLAY_WINDOW  64u   /* 重放窗口大小，必须是 2 的幂且 <= 64     */

/* RTP/RTCP 固定头长度 */
#define SRTP_RTP_HDR_LEN    12u
#define SRTP_RTCP_HDR_LEN   8u
#define SRTP_SRTCP_IDX_LEN  4u    /* E flag(1bit) + 31-bit SRTCP index */

/* 返回码。用法：0 == 成功，负数 == 拒绝原因。 */
typedef enum {
    SRTP_OK          = 0,
    SRTP_ERR_AUTH    = -1,  /* 认证标签校验失败（包被篡改或不是本 key）  */
    SRTP_ERR_REPLAY  = -2,  /* 重放包，或过于陈旧（超出窗口）            */
    SRTP_ERR_SHORT   = -3,  /* 包太短，连头+tag 都放不下                 */
    SRTP_ERR_LARGE   = -4,  /* 包超出 SRTP_MAX_PACKET                    */
    SRTP_ERR_RTP_HDR = -5,  /* RTP 头非法（版本 != 2、CSRC/扩展越界等）  */
    SRTP_ERR_NOMEM   = -6,  /* srtp_init 里 OpenSSL 句柄分配失败          */
    SRTP_ERR_ARG     = -7,  /* 参数非法（kdr != 0、SSRC 不匹配等）       */
    SRTP_ERR_SSRC    = -8   /* 包里的 SSRC 与上下文不一致                 */
} srtp_status;

/* 一组（SRTP 或 SRTCP 的）session key。由 srtp_derive_session_keys() 填充。 */
typedef struct {
    uint8_t enc[SRTP_KEY_LEN];        /* k_e  */
    uint8_t auth[SRTP_AUTH_KEY_LEN];  /* k_a  */
    uint8_t salt[SRTP_SALT_LEN];      /* k_s  */
} srtp_keys;

/*
 * 主密钥材料（key management 交下来的东西）。
 *
 * master_salt_len 支持 12 和 14 两种：
 *   - 14：RFC 3711 / RFC 5764 SRTP_AES128_CM_HMAC_SHA1_80 的标准长度
 *         （master key 16 + master salt 14 = 30 字节）。
 *   - 12：WebRTC/SDES 常见的 16+12=28 字节材料。12 字节盐在 RFC 里没有
 *         定义，业界（libsrtp 兼容）的做法是把它**右对齐**放进 14 字节盐里，
 *         即高 2 字节补 0。本实现照此处理，见 srtp.c 的 kdf_derive()。
 */
typedef struct {
    uint8_t  master_key[SRTP_KEY_LEN];
    uint8_t  master_salt[SRTP_SALT_LEN];  /* 只用最后 master_salt_len 字节 */
    uint8_t  master_salt_len;             /* 12 或 14                     */
    uint32_t kdr;                         /* key_derivation_rate，必须 0  */
} srtp_master_key;

/* 统计计数器。只读，用于日志/看门狗。 */
typedef struct {
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t rx_auth_fail;
    uint32_t rx_replay;
    uint32_t rx_late;        /* 认证通过但超出重放窗口 */
    uint32_t rx_bad_header;
    uint32_t rtcp_tx;
    uint32_t rtcp_rx;
} srtp_stats;

/* 重放窗口（RFC 3711 $3.3.2）。64 位位图 + 最高序列号。 */
typedef struct {
    uint64_t bitmap;   /* bit0 == highest，bit k == highest-k */
    uint64_t highest;  /* 已接收的最大 48-bit SRTP index        */
    int      valid;
} srtp_replay;

/*
 * 加解密上下文。
 *
 * 公开结构体（不是 opaque），这样调用方可以把 ctx 直接放进程/bss 里，
 * 也可以嵌进自己的 session 结构；测试程序也能读到派生的 session key
 * 去和 RFC 3711 附录 B.3 的权威值比对。
 *
 * 一个 ctx 同时持有发送侧和接收侧状态（互不干扰），但**收发必须同 SSRC**。
 */
typedef struct {
    /* 派生出来的 session key：仅供调试/自检读取 */
    srtp_keys srtp;            /* k_e/k_a/k_s for SRTP  */
    srtp_keys srtcp;           /* k_e/k_a/k_s for SRTCP */

    /* OpenSSL 句柄（srtp_init 时一次性分配，仅 4 个） */
    void *evp_srtp;            /* EVP_CIPHER_CTX* for SRTP AES-CM  */
    void *evp_srtcp;           /* EVP_CIPHER_CTX* for SRTCP AES-CM */
    void *hmac_srtp;           /* HMAC_CTX* for SRTP               */
    void *hmac_srtcp;          /* HMAC_CTX* for SRTCP              */

    srtp_master_key mk;        /* 主密钥副本（deinit 时清零）      */

    uint32_t ssrc;             /* 0 == 尚未绑定，收到首个包再绑定  */

    /* ---- 发送侧 (SRTP) ---- */
    uint16_t tx_s_l;           /* 最近发送的 SEQ                   */
    uint32_t tx_roc;           /* 对应 ROC                         */
    int      tx_started;

    /* ---- 接收侧 (SRTP) ---- */
    uint16_t rx_s_l;
    uint32_t rx_roc;
    int      rx_started;
    srtp_replay rx_replay;

    /* ---- SRTCP 发送/接收 ---- */
    uint32_t    tx_rtcp_index; /* 31-bit，从 0 开始递增             */
    uint32_t    rx_rtcp_index; /* 最近接收的 index（诊断用）        */
    srtp_replay rx_rtcp_replay;

    srtp_stats stats;
} srtp_ctx;

/* ------------------------------------------------------------- 生命周期 */

/*
 * 初始化上下文并派生 session key。
 *
 *   ssrc 传 0 表示"还不知道 SSRC"：第一个 protect/unprotect 的包会绑定它；
 *   之后包里出现别的 SSRC 会返回 SRTP_ERR_SSRC（防止跨流混用同一密钥）。
 *
 * 返回 SRTP_ERR_NOMEM（OpenSSL 句柄分配失败）或 SRTP_ERR_ARG（参数非法，
 * 含 kdr != 0 / master_salt_len 不在 {12,14}）。
 */
srtp_status srtp_init(srtp_ctx *ctx, const srtp_master_key *mk, uint32_t ssrc);

/*
 * 用**已经派生好的 session key** 初始化（跳过 KDF）。
 * 用于 key management 直接下发 session key 的场景，以及测试里对接
 * RFC 3711 附录 B 的权威 session key。
 */
srtp_status srtp_init_with_session_keys(srtp_ctx *ctx,
                                        const srtp_keys *k_srtp,
                                        const srtp_keys *k_srtcp,
                                        uint32_t ssrc);

/* 释放 OpenSSL 句柄并清零 key 材料。可重复调用。 */
void srtp_deinit(srtp_ctx *ctx);

/* --------------------------------------------------------------- 密钥派生 */

/*
 * RFC 3711 $4.3.1 密钥派生。kdr 必须为 0（= 只做一次初始派生）。
 * 输出 SRTP 的 (k_e,k_a,k_s) 标签 0/1/2 和 SRTCP 的 标签 3/4/5。
 * 任一输出指针可为 NULL（只算需要的那一半）。
 */
srtp_status srtp_derive_session_keys(const srtp_master_key *mk,
                                     srtp_keys *out_srtp,
                                     srtp_keys *out_srtcp);

/* ----------------------------------------------------------------- 状态 */

/* 发送起始 SEQ（例如 key management / 中途接管时告知）。 */
void srtp_set_send_seq(srtp_ctx *ctx, uint16_t seq);

/* 接收侧初始 ROC（未知时传 0）。 */
void srtp_set_recv_roc(srtp_ctx *ctx, uint32_t roc);

/* 绑定/更换 SSRC，并复位收发序列状态（换流时用）。 */
void srtp_set_ssrc(srtp_ctx *ctx, uint32_t ssrc);

/* ------------------------------------------------------------------ SRTP */

/*
 * 加密 + 认证一个 RTP 包（原地）。
 *
 *   buf/len  进入时是完整 RTP 包（头 + 明文 payload），len 不含 tag。
 *   out_len  返回 len + SRTP_TAG_LEN。
 *   缓冲必须至少能放下 len + SRTP_TAG_LEN 字节，且 <= SRTP_MAX_PACKET。
 *
 * 只加密 payload（含 RTP padding），头保持明文；认证覆盖头 + 密文 payload。
 */
srtp_status srtp_protect(srtp_ctx *ctx, uint8_t *buf, size_t len,
                         size_t *out_len);

/*
 * 认证 + 解密一个 SRTP 包（原地）。
 *
 *   buf/len  进入时是完整 SRTP 包（含 10 字节 tag）。
 *   out_len  返回去掉 tag 之后的长度（即还原的 RTP 包长度）。
 *
 * 处理顺序严格按 RFC 3711 $3.3：先认证，再查重放，最后解密；认证失败
 * 不更新 ROC/重放窗口。
 */
srtp_status srtp_unprotect(srtp_ctx *ctx, uint8_t *buf, size_t len,
                           size_t *out_len);

/* ----------------------------------------------------------------- SRTCP */

/*
 * 加密 + 认证一个 RTCP 包（原地）。
 * 语义：加密 offset 8 之后的全部内容，末尾追加 4 字节 (E|index)，再追加
 * 10 字节 tag。out_len = len + 4 + 10。
 */
srtp_status srtcp_protect(srtp_ctx *ctx, uint8_t *buf, size_t len,
                          size_t *out_len);

/* 认证 + 解密一个 SRTCP 包（原地）。out_len 为去掉 index 字段和 tag 之后的值。 */
srtp_status srtcp_unprotect(srtp_ctx *ctx, uint8_t *buf, size_t len,
                            size_t *out_len);

/* --------------------------------------------------------------- 小工具 */

/* 解析 RTP 头长度（含 CSRC / 扩展头）。失败返回 -1。供外部预判 payload 偏移。 */
int srtp_rtp_header_len(const uint8_t *buf, size_t len);

/*
 * 底层原语，对外暴露**仅供自检与单元测试**使用；生产代码不要直接调用。
 *
 * srtp_aes_cm_keystream: RFC 3711 $4.3.3 的 AES-CM PRF，输出 len 字节 keystream。
 * srtp_kdf_derive_raw:    RFC 3711 $4.3.1，用 label 派生 out_len 字节。
 */
int srtp_aes_cm_keystream(const uint8_t key[SRTP_KEY_LEN],
                          const uint8_t iv[16],
                          uint8_t *out, size_t len);

srtp_status srtp_kdf_derive_raw(const srtp_master_key *mk, uint8_t label,
                                uint8_t *out, size_t out_len);

/* 返回码 -> 可读字符串（日志用）。 */
const char *srtp_strerror(srtp_status st);

#ifdef __cplusplus
}
#endif

#endif /* PENYS_RTP_SRTP_H */
