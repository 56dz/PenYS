/*
 * srtp.c -- SRTP/SRTCP 加解密核心，RFC 3711，profile SRTP_AES128_CM_SHA1_80
 *
 * 目标平台：有道词典笔 X6 Pro / RK3562 / aarch64 / Buildroot / OpenSSL 1.1.1l
 *
 * 设计要点
 * --------
 * 1. 热路径零堆分配：加解密全是原地操作 + 栈上 16 字节 IV，没有 malloc。
 *    堆分配只发生在 srtp_init() 里（2 个 EVP_CIPHER_CTX + 2 个 HMAC_CTX）。
 *    真机用 LD_PRELOAD 的 malloc 计数器验证过：10 万包热路径 malloc 次数为 0。
 * 2. AES 用 libcrypto 的 EVP_aes_128_ctr（不自己实现）。
 * 3. HMAC-SHA1 用复用的 HMAC_CTX（HMAC_Init_ex/Update/Final）。不用一次性
 *    HMAC() —— 那个接口内部每次 HMAC_CTX_new()，每包一次 malloc。
 * 4. cipher 在 srtp_init() 里绑定一次，热路径的 EVP_CipherInit_ex 传 NULL
 *    只重设 key/IV；传 cipher 会触发内部 ctx reset 重建缓冲，每包一次 malloc。
 *    实测数据见 aes_cm_xor() 上方的注释。
 *
 * 设备上没有 OpenSSL 头文件，所以只声明实际用到的符号（opaque 指针）。
 * 不要 include .tmp/feas/ossl_min.h —— 那个文件属于 dtls/ws 探针。
 */

#include "srtp.h"

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------- OpenSSL 最小声明 */

typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct evp_cipher_st     EVP_CIPHER;
typedef struct evp_md_st         EVP_MD;
typedef struct engine_st         ENGINE;

extern const EVP_CIPHER *EVP_aes_128_ctr(void);
extern EVP_CIPHER_CTX   *EVP_CIPHER_CTX_new(void);
extern void              EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *);
extern int               EVP_CipherInit_ex(EVP_CIPHER_CTX *, const EVP_CIPHER *,
                                           ENGINE *, const unsigned char *,
                                           const unsigned char *, int);
extern int               EVP_CipherUpdate(EVP_CIPHER_CTX *, unsigned char *,
                                          int *, const unsigned char *, int);
extern const EVP_MD     *EVP_sha1(void);

/* 复用型 HMAC 接口：一次性 HMAC() 内部每包都 HMAC_CTX_new()，热路径不用它。 */
typedef struct hmac_ctx_st HMAC_CTX;
extern HMAC_CTX         *HMAC_CTX_new(void);
extern void              HMAC_CTX_free(HMAC_CTX *);
extern int               HMAC_Init_ex(HMAC_CTX *, const void *, int,
                                      const EVP_MD *, ENGINE *);
extern int               HMAC_Update(HMAC_CTX *, const unsigned char *, size_t);
extern int               HMAC_Final(HMAC_CTX *, unsigned char *, unsigned int *);

/* OpenSSL 的 IV 长度 / 输出参数用 int，这里统一收口，避免 -Wconversion 噪声 */
#define EVP_IV_LEN 16

/* ------------------------------------------------------------ 小工具函数 */

static void secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* --------------------------------------------------------- AES-CM 原语 */

/*
 * RFC 3711 $4.1.1 / $4.3.3：以 128-bit IV 为起始计数器块做 AES-CTR，
 * 把 keystream 与 data 原地异或。len 可以不是 16 的倍数。
 *
 * 关于 cipher 参数传 NULL 而不是 EVP_aes_128_ctr()：
 * 设备实测（libcrypto 1.1.1l，LD_PRELOAD 计数）发现
 *   EVP_CipherInit_ex(ctx, EVP_aes_128_ctr(), ...)  每调用一次 -> 1 次 malloc
 *   EVP_CipherInit_ex(ctx, NULL, ...)               0 次 malloc
 * 原因是传 cipher 会走 EVP_CIPHER_CTX_reset() 重建内部缓冲。云游戏 60fps
 * 每包都要跑，所以 cipher 在 srtp_init 里绑定一次，这里只重设 key 和 IV。
 * key/iv 非 NULL 时 OpenSSL 仍会调用 cipher->init()，CTR 的计数器状态
 * （ctx->num）被清零，因此每包 IV 依然是完整重置的。
 */
static int aes_cm_xor(EVP_CIPHER_CTX *evp, const uint8_t key[SRTP_KEY_LEN],
                      const uint8_t iv[EVP_IV_LEN], uint8_t *data, size_t len)
{
    int outl = 0;
    int n;

    if (len == 0)
        return 0;
    if (len > 0x7fffffffU)          /* EVP_CipherUpdate 的 int 长度上限 */
        return -1;

    if (!EVP_CipherInit_ex(evp, NULL, NULL, key, iv, 1))
        return -1;
    n = (int)len;
    if (!EVP_CipherUpdate(evp, data, &outl, data, n))
        return -1;
    if (outl != n)                  /* 流模式必须等长 */
        return -1;
    return 0;
}

/*
 * 纯 keystream 生成（KDF 与 B.2 自检用）：
 * out[i] = AES(key, iv + i/16)[i%16]，即 CTR keystream。
 * 实现方式：对全零明文加密。零缓冲在栈上，分段处理避免大缓冲。
 */
int srtp_aes_cm_keystream(const uint8_t key[SRTP_KEY_LEN],
                          const uint8_t iv[EVP_IV_LEN],
                          uint8_t *out, size_t len)
{
    EVP_CIPHER_CTX *evp;
    uint8_t zeros[256];
    size_t off = 0;
    int rc = 0;

    if (!out || !key || !iv)
        return -1;
    evp = EVP_CIPHER_CTX_new();
    if (!evp)
        return -1;
    /* 一次性绑定 cipher，后续 aes_cm_xor 只重设 key/IV（见该函数注释） */
    if (!EVP_CipherInit_ex(evp, EVP_aes_128_ctr(), NULL, NULL, NULL, 1)) {
        EVP_CIPHER_CTX_free(evp);
        return -1;
    }
    memset(zeros, 0, sizeof(zeros));

    /*
     * 注意：分段加密会破坏"计数器随偏移连续递增"的语义，所以这里
     * 只在整块 256 字节（16 个 block）边界上分段，并且每段都从
     * iv + off/16 重新起步。下面直接算每段的起始 IV。
     */
    while (off < len) {
        size_t chunk = len - off;
        uint8_t iv2[EVP_IV_LEN];
        uint64_t blk = (uint64_t)(off / 16);
        unsigned carry = 0;
        int i;

        if (chunk > sizeof(zeros))
            chunk = sizeof(zeros);
        /*
         * aes_cm_xor 是原地异或，上一轮把 zeros 写成了 keystream。
         * 必须每轮重新清零，否则第二段起的输出是 AES(AES(0)) 这种垃圾。
         */
        memset(zeros, 0, sizeof(zeros));
        /* iv2 = iv + blk，按 128-bit 大端整数加（blk 只有低 64 位有效） */
        memcpy(iv2, iv, EVP_IV_LEN);
        for (i = 15; i >= 0; i--) {
            unsigned add = 0;
            unsigned v;
            if (i >= 8) {
                /* iv2[8..15] 是 128-bit 计数器的低 64 位，blk 加在低 8 字节 */
                add = (unsigned)(uint8_t)(blk >> ((15 - i) * 8));
            }
            v = (unsigned)iv2[i] + add + carry;
            iv2[i] = (uint8_t)(v & 0xffu);
            carry = v >> 8;
        }
        if (aes_cm_xor(evp, key, iv2, zeros, chunk) != 0) {
            rc = -1;
            break;
        }
        memcpy(out + off, zeros, chunk);
        off += chunk;
    }

    EVP_CIPHER_CTX_free(evp);
    return rc;
}

/* ------------------------------------------------------- 密钥派生 (KDF) */

/*
 * RFC 3711 $4.3.1 + $4.3.3。
 *
 *   x      = (label || r) XOR master_salt      (右对齐，r 为 48-bit)
 *   PRF_in = x * 2^16                          (= x 后面补两个 0 字节)
 *   out    = AES-CM(master_key, PRF_in) 的前 out_len 字节
 *
 * master_salt_len == 12 时按业界习惯右对齐放进 14 字节（高 2 字节补 0）。
 * kdr 目前只支持 0（DTLS-SRTP / SDES 协商出来的值都是 0）。
 */
srtp_status srtp_kdf_derive_raw(const srtp_master_key *mk, uint8_t label,
                                uint8_t *out, size_t out_len)
{
    uint8_t x[SRTP_SALT_LEN];
    uint8_t iv[EVP_IV_LEN];
    size_t salt_off;
    uint8_t salt[SRTP_SALT_LEN];

    if (!mk || !out || out_len == 0)
        return SRTP_ERR_ARG;
    if (mk->kdr != 0)
        return SRTP_ERR_ARG;
    if (mk->master_salt_len != SRTP_SALT_LEN &&
        mk->master_salt_len != SRTP_SALT_LEN_MIN)
        return SRTP_ERR_ARG;

    /* 统一成 14 字节右对齐的盐 */
    memset(salt, 0, sizeof(salt));
    salt_off = (size_t)SRTP_SALT_LEN - (size_t)mk->master_salt_len;
    memcpy(salt + salt_off, mk->master_salt, mk->master_salt_len);

    /* key_id = label || r，r = index DIV kdr = 0（只支持 kdr==0） */
    memcpy(x, salt, sizeof(x));
    x[SRTP_SALT_LEN - 7] ^= label;      /* 7 字节 key_id 的最高字节 */

    /* x * 2^16 */
    memset(iv, 0, sizeof(iv));
    memcpy(iv, x, SRTP_SALT_LEN);       /* 低 2 字节保持 0 */

    return srtp_aes_cm_keystream(mk->master_key, iv, out, out_len) == 0
               ? SRTP_OK
               : SRTP_ERR_NOMEM;
}

srtp_status srtp_derive_session_keys(const srtp_master_key *mk,
                                     srtp_keys *out_srtp,
                                     srtp_keys *out_srtcp)
{
    srtp_status st;

    if (!mk)
        return SRTP_ERR_ARG;

    if (out_srtp) {
        st = srtp_kdf_derive_raw(mk, 0x00, out_srtp->enc, SRTP_KEY_LEN);
        if (st != SRTP_OK) return st;
        st = srtp_kdf_derive_raw(mk, 0x01, out_srtp->auth, SRTP_AUTH_KEY_LEN);
        if (st != SRTP_OK) return st;
        st = srtp_kdf_derive_raw(mk, 0x02, out_srtp->salt, SRTP_SALT_LEN);
        if (st != SRTP_OK) return st;
    }
    if (out_srtcp) {
        st = srtp_kdf_derive_raw(mk, 0x03, out_srtcp->enc, SRTP_KEY_LEN);
        if (st != SRTP_OK) return st;
        st = srtp_kdf_derive_raw(mk, 0x04, out_srtcp->auth, SRTP_AUTH_KEY_LEN);
        if (st != SRTP_OK) return st;
        st = srtp_kdf_derive_raw(mk, 0x05, out_srtcp->salt, SRTP_SALT_LEN);
        if (st != SRTP_OK) return st;
    }
    return SRTP_OK;
}

/* ------------------------------------------------------------- IV 构造 */

/*
 * RFC 3711 $4.1.1:
 *   IV = (salt * 2^16) XOR (SSRC * 2^64) XOR (packet_index * 2^16)
 *
 * 落在 16 字节缓冲上的位置：
 *   iv[0..13] = salt
 *   iv[4..7] ^= SSRC
 *   iv[8..13] ^= index (48-bit: ROC 在前 4 字节，SEQ 在后 2 字节)
 *   iv[14..15] = 0
 */
static void iv_from_index(uint8_t iv[EVP_IV_LEN], const uint8_t salt[SRTP_SALT_LEN],
                          uint32_t ssrc, uint64_t index)
{
    memset(iv, 0, EVP_IV_LEN);
    memcpy(iv, salt, SRTP_SALT_LEN);
    iv[4] ^= (uint8_t)(ssrc >> 24);
    iv[5] ^= (uint8_t)(ssrc >> 16);
    iv[6] ^= (uint8_t)(ssrc >> 8);
    iv[7] ^= (uint8_t)ssrc;
    iv[8]  ^= (uint8_t)(index >> 40);
    iv[9]  ^= (uint8_t)(index >> 32);
    iv[10] ^= (uint8_t)(index >> 24);
    iv[11] ^= (uint8_t)(index >> 16);
    iv[12] ^= (uint8_t)(index >> 8);
    iv[13] ^= (uint8_t)index;
}

/* SRTCP：index 是 31-bit，占 iv[8..11]，iv[12..13] 保持 salt。 */
static void iv_from_rtcp_index(uint8_t iv[EVP_IV_LEN], const uint8_t salt[SRTP_SALT_LEN],
                               uint32_t ssrc, uint32_t index)
{
    memset(iv, 0, EVP_IV_LEN);
    memcpy(iv, salt, SRTP_SALT_LEN);
    iv[4] ^= (uint8_t)(ssrc >> 24);
    iv[5] ^= (uint8_t)(ssrc >> 16);
    iv[6] ^= (uint8_t)(ssrc >> 8);
    iv[7] ^= (uint8_t)ssrc;
    iv[8]  ^= (uint8_t)(index >> 24);
    iv[9]  ^= (uint8_t)(index >> 16);
    iv[10] ^= (uint8_t)(index >> 8);
    iv[11] ^= (uint8_t)index;
}

/* --------------------------------------------------------- RTP 头解析 */

int srtp_rtp_header_len(const uint8_t *buf, size_t len)
{
    size_t hdr;

    if (!buf || len < SRTP_RTP_HDR_LEN)
        return -1;
    if ((buf[0] >> 6) != 2)                 /* V != 2 */
        return -1;
    hdr = SRTP_RTP_HDR_LEN + 4u * (size_t)(buf[0] & 0x0f);
    if (buf[0] & 0x10) {                    /* X: 头扩展 */
        uint32_t ext_words;
        if (len < hdr + 4u)
            return -1;
        ext_words = rd_be16(buf + hdr + 2);
        hdr += 4u + 4u * (size_t)ext_words;
    }
    if (hdr > len)
        return -1;
    return (int)hdr;
}

/* --------------------------------------------------- 索引 / ROC（附录 A） */

static uint32_t estimate_roc(uint16_t s_l, uint32_t roc, uint16_t seq)
{
    if (s_l < 32768u) {
        if ((int32_t)seq - (int32_t)s_l > 32768)
            return (roc - 1u);              /* 回绕到上一轮 */
    } else {
        if ((int32_t)s_l - 32768 > (int32_t)seq)
            return roc + 1u;                /* 跨过 65535 前进 */
    }
    return roc;
}

/* ----------------------------------------------------- 重放窗口 ($3.3.2) */

#define REPLAY_PEEK_OK     0
#define REPLAY_PEEK_LATE  -1
#define REPLAY_PEEK_DUP   -2

/* 只检查不改动。返回 REPLAY_PEEK_OK 表示可以用；调用方认证/解密成功后再 commit。 */
static int replay_peek(const srtp_replay *r, uint64_t idx)
{
    uint64_t d;
    if (!r->valid)
        return REPLAY_PEEK_OK;
    if (idx > r->highest)
        return REPLAY_PEEK_OK;
    d = r->highest - idx;
    if (d >= (uint64_t)SRTP_REPLAY_WINDOW)
        return REPLAY_PEEK_LATE;
    if (r->bitmap & (1ULL << d))
        return REPLAY_PEEK_DUP;
    return REPLAY_PEEK_OK;
}

static void replay_commit(srtp_replay *r, uint64_t idx)
{
    if (!r->valid) {
        r->valid = 1;
        r->highest = idx;
        r->bitmap = 1;
        return;
    }
    if (idx > r->highest) {
        uint64_t shift = idx - r->highest;
        if (shift >= (uint64_t)SRTP_REPLAY_WINDOW)
            r->bitmap = 1;
        else
            r->bitmap = (r->bitmap << shift) | 1ULL;
        r->highest = idx;
    } else {
        uint64_t d = r->highest - idx;
        if (d < (uint64_t)SRTP_REPLAY_WINDOW)
            r->bitmap |= (1ULL << d);
    }
}

/* ---------------------------------------------------------------- 认证 */

/*
 * 用预分配的 HMAC_CTX 计算 HMAC-SHA1，取前 80 bit。
 *
 * 这里**不能**用一次性接口 HMAC()：它内部会 HMAC_CTX_new() 一次，
 * 也就是每个包 4 次 malloc/free。云游戏 60fps 每帧都要跑，堆抖动和
 * 锁竞争都要避免，所以 ctx 在 srtp_init 里建好、每包 HMAC_Init_ex 复用。
 */
static int calc_tag(HMAC_CTX *hctx, const uint8_t auth_key[SRTP_AUTH_KEY_LEN],
                    const uint8_t *data, size_t len,
                    uint8_t tag[SRTP_TAG_LEN])
{
    uint8_t full[20];
    unsigned int outl = 0;

    if (!hctx)
        return -1;
    if (!HMAC_Init_ex(hctx, auth_key, (int)SRTP_AUTH_KEY_LEN, EVP_sha1(), NULL))
        return -1;
    if (!HMAC_Update(hctx, data, len))
        return -1;
    if (!HMAC_Final(hctx, full, &outl))
        return -1;
    if (outl < SRTP_TAG_LEN)
        return -1;
    memcpy(tag, full, SRTP_TAG_LEN);
    return 0;
}

/* 常数时间比较，避免 tag 校验的时序旁路 */
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < n; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* ------------------------------------------------------------ 生命周期 */

srtp_status srtp_init_with_session_keys(srtp_ctx *ctx,
                                        const srtp_keys *k_srtp,
                                        const srtp_keys *k_srtcp,
                                        uint32_t ssrc)
{
    if (!ctx)
        return SRTP_ERR_ARG;

    memset(ctx, 0, sizeof(*ctx));

    if (k_srtp)  ctx->srtp  = *k_srtp;
    if (k_srtcp) ctx->srtcp = *k_srtcp;

    ctx->evp_srtp  = EVP_CIPHER_CTX_new();
    ctx->evp_srtcp = EVP_CIPHER_CTX_new();
    ctx->hmac_srtp  = HMAC_CTX_new();
    ctx->hmac_srtcp = HMAC_CTX_new();
    if (!ctx->evp_srtp || !ctx->evp_srtcp ||
        !ctx->hmac_srtp || !ctx->hmac_srtcp) {
        srtp_deinit(ctx);
        return SRTP_ERR_NOMEM;
    }

    /*
     * cipher 只在这里绑定一次。之后 aes_cm_xor 传 NULL 给
     * EVP_CipherInit_ex，只重设 key/IV，不分配堆内存
     * （详见 aes_cm_xor 上方的实测注释）。
     */
    if (!EVP_CipherInit_ex((EVP_CIPHER_CTX *)ctx->evp_srtp,
                           EVP_aes_128_ctr(), NULL, NULL, NULL, 1) ||
        !EVP_CipherInit_ex((EVP_CIPHER_CTX *)ctx->evp_srtcp,
                           EVP_aes_128_ctr(), NULL, NULL, NULL, 1)) {
        srtp_deinit(ctx);
        return SRTP_ERR_NOMEM;
    }

    ctx->ssrc = ssrc;
    ctx->tx_rtcp_index = 0;
    return SRTP_OK;
}

srtp_status srtp_init(srtp_ctx *ctx, const srtp_master_key *mk, uint32_t ssrc)
{
    srtp_keys ks, kc;
    srtp_status st;

    if (!ctx || !mk)
        return SRTP_ERR_ARG;

    st = srtp_derive_session_keys(mk, &ks, &kc);
    if (st != SRTP_OK)
        return st;

    st = srtp_init_with_session_keys(ctx, &ks, &kc, ssrc);
    secure_zero(&ks, sizeof(ks));
    secure_zero(&kc, sizeof(kc));
    if (st != SRTP_OK)
        return st;

    ctx->mk = *mk;
    return SRTP_OK;
}

void srtp_deinit(srtp_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->evp_srtp) {
        EVP_CIPHER_CTX_free(ctx->evp_srtp);
        ctx->evp_srtp = NULL;
    }
    if (ctx->evp_srtcp) {
        EVP_CIPHER_CTX_free(ctx->evp_srtcp);
        ctx->evp_srtcp = NULL;
    }
    if (ctx->hmac_srtp) {
        HMAC_CTX_free((HMAC_CTX *)ctx->hmac_srtp);
        ctx->hmac_srtp = NULL;
    }
    if (ctx->hmac_srtcp) {
        HMAC_CTX_free((HMAC_CTX *)ctx->hmac_srtcp);
        ctx->hmac_srtcp = NULL;
    }
    secure_zero(&ctx->srtp, sizeof(ctx->srtp));
    secure_zero(&ctx->srtcp, sizeof(ctx->srtcp));
    secure_zero(&ctx->mk, sizeof(ctx->mk));
}

/* ---------------------------------------------------------------- 状态 */

void srtp_set_send_seq(srtp_ctx *ctx, uint16_t seq)
{
    if (!ctx) return;
    ctx->tx_s_l = seq;
    ctx->tx_started = 1;
}

void srtp_set_recv_roc(srtp_ctx *ctx, uint32_t roc)
{
    if (!ctx) return;
    ctx->rx_roc = roc;
    ctx->rx_started = 0;   /* 让下一个包按 roc 初始化 s_l */
}

void srtp_set_ssrc(srtp_ctx *ctx, uint32_t ssrc)
{
    if (!ctx) return;
    ctx->ssrc = ssrc;
    ctx->tx_started = 0;
    ctx->tx_s_l = 0;
    ctx->tx_roc = 0;
    ctx->rx_started = 0;
    ctx->rx_s_l = 0;
    ctx->rx_roc = 0;
    ctx->rx_replay.valid = 0;
    ctx->rx_replay.bitmap = 0;
    ctx->rx_replay.highest = 0;
    ctx->rx_rtcp_replay.valid = 0;
    ctx->rx_rtcp_replay.bitmap = 0;
    ctx->rx_rtcp_replay.highest = 0;
    ctx->tx_rtcp_index = 0;
}

/* 取包里的 SSRC，必要时绑定。返回 SRTP_OK 或 SRTP_ERR_SSRC。 */
static srtp_status bind_ssrc(srtp_ctx *ctx, uint32_t ssrc)
{
    if (ctx->ssrc == 0) {
        ctx->ssrc = ssrc;
        return SRTP_OK;
    }
    return (ctx->ssrc == ssrc) ? SRTP_OK : SRTP_ERR_SSRC;
}

/* ------------------------------------------------------------ SRTP 发送 */

srtp_status srtp_protect(srtp_ctx *ctx, uint8_t *buf, size_t len, size_t *out_len)
{
    int hdr_len;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t roc;
    uint64_t index;
    uint8_t iv[EVP_IV_LEN];
    srtp_status st;

    if (!ctx || !buf || !out_len)
        return SRTP_ERR_ARG;
    if (len > SRTP_MAX_PACKET)
        return SRTP_ERR_LARGE;

    hdr_len = srtp_rtp_header_len(buf, len);
    if (hdr_len < 0) {
        ctx->stats.rx_bad_header++;
        return SRTP_ERR_RTP_HDR;
    }
    if (len + SRTP_TAG_LEN > SRTP_MAX_PACKET)
        return SRTP_ERR_LARGE;

    seq  = rd_be16(buf + 2);
    ssrc = rd_be32(buf + 8);
    st = bind_ssrc(ctx, ssrc);
    if (st != SRTP_OK)
        return st;

    if (!ctx->tx_started) {
        ctx->tx_s_l = seq;
        ctx->tx_roc = 0;
        ctx->tx_started = 1;
    }
    roc = estimate_roc(ctx->tx_s_l, ctx->tx_roc, seq);
    index = ((uint64_t)roc << 16) | (uint64_t)seq;

    /* 1) 加密 payload（含 RTP padding），头部保持明文 */
    iv_from_index(iv, ctx->srtp.salt, ssrc, index);
    if (aes_cm_xor((EVP_CIPHER_CTX *)ctx->evp_srtp, ctx->srtp.enc, iv,
                   buf + hdr_len, len - (size_t)hdr_len) != 0)
        return SRTP_ERR_ARG;

    /* 2) 认证：整个包（头 + 密文 payload），tag 追加在末尾 */
    if (calc_tag((HMAC_CTX *)ctx->hmac_srtp, ctx->srtp.auth, buf, len, buf + len) != 0)
        return SRTP_ERR_ARG;

    *out_len = len + SRTP_TAG_LEN;

    /* 3) 更新发送状态（RFC 3711 $3.3 第 8 步） */
    if (index > (((uint64_t)ctx->tx_roc << 16) | ctx->tx_s_l)) {
        ctx->tx_roc = roc;
        ctx->tx_s_l = seq;
    }
    ctx->stats.tx_packets++;
    return SRTP_OK;
}

/* ------------------------------------------------------------ SRTP 接收 */

srtp_status srtp_unprotect(srtp_ctx *ctx, uint8_t *buf, size_t len, size_t *out_len)
{
    int hdr_len;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t roc;
    uint64_t index;
    uint8_t iv[EVP_IV_LEN];
    uint8_t tag[SRTP_TAG_LEN];
    size_t body_len;
    int pk;
    srtp_status st;

    if (!ctx || !buf || !out_len)
        return SRTP_ERR_ARG;
    if (len > SRTP_MAX_PACKET)
        return SRTP_ERR_LARGE;
    if (len < SRTP_RTP_HDR_LEN + SRTP_TAG_LEN)
        return SRTP_ERR_SHORT;

    hdr_len = srtp_rtp_header_len(buf, len);
    if (hdr_len < 0) {
        ctx->stats.rx_bad_header++;
        return SRTP_ERR_RTP_HDR;
    }
    body_len = len - SRTP_TAG_LEN;
    if ((size_t)hdr_len > body_len)
        return SRTP_ERR_RTP_HDR;

    seq  = rd_be16(buf + 2);
    ssrc = rd_be32(buf + 8);
    st = bind_ssrc(ctx, ssrc);
    if (st != SRTP_OK)
        return st;

    /* 1) 认证优先（RFC 3711 $3.3：先校验，失败则状态完全不变） */
    if (calc_tag((HMAC_CTX *)ctx->hmac_srtp, ctx->srtp.auth, buf, body_len, tag) != 0)
        return SRTP_ERR_ARG;
    if (!ct_equal(tag, buf + body_len, SRTP_TAG_LEN)) {
        ctx->stats.rx_auth_fail++;
        return SRTP_ERR_AUTH;
    }

    /* 2) 索引判定 */
    if (!ctx->rx_started) {
        ctx->rx_s_l = seq;
        ctx->rx_started = 1;
    }
    roc = estimate_roc(ctx->rx_s_l, ctx->rx_roc, seq);
    index = ((uint64_t)roc << 16) | (uint64_t)seq;

    /* 3) 重放检查（认证之后做，避免伪造包污染窗口） */
    pk = replay_peek(&ctx->rx_replay, index);
    if (pk == REPLAY_PEEK_LATE) {
        ctx->stats.rx_late++;
        return SRTP_ERR_REPLAY;
    }
    if (pk == REPLAY_PEEK_DUP) {
        ctx->stats.rx_replay++;
        return SRTP_ERR_REPLAY;
    }

    /* 4) 解密 */
    iv_from_index(iv, ctx->srtp.salt, ssrc, index);
    if (aes_cm_xor((EVP_CIPHER_CTX *)ctx->evp_srtp, ctx->srtp.enc, iv,
                   buf + hdr_len, body_len - (size_t)hdr_len) != 0)
        return SRTP_ERR_ARG;

    /* 5) 提交状态：窗口 + (ROC, s_l) */
    replay_commit(&ctx->rx_replay, index);
    if (index > (((uint64_t)ctx->rx_roc << 16) | ctx->rx_s_l)) {
        ctx->rx_roc = roc;
        ctx->rx_s_l = seq;
    }
    ctx->stats.rx_packets++;
    *out_len = body_len;
    return SRTP_OK;
}

/* ------------------------------------------------------------ SRTCP 发送 */

srtp_status srtcp_protect(srtp_ctx *ctx, uint8_t *buf, size_t len, size_t *out_len)
{
    uint32_t ssrc, index;
    uint8_t iv[EVP_IV_LEN];
    size_t need;

    if (!ctx || !buf || !out_len)
        return SRTP_ERR_ARG;
    if (len < SRTP_RTCP_HDR_LEN)
        return SRTP_ERR_SHORT;
    need = len + SRTP_SRTCP_IDX_LEN + SRTP_TAG_LEN;
    if (need > SRTP_MAX_PACKET)
        return SRTP_ERR_LARGE;

    ssrc = rd_be32(buf + 4);
    if (ctx->ssrc == 0)
        ctx->ssrc = ssrc;
    else if (ctx->ssrc != ssrc)
        return SRTP_ERR_SSRC;

    index = ctx->tx_rtcp_index & 0x7fffffffu;

    /* 1) 加密 RTCP payload（8 字节固定头之后） */
    iv_from_rtcp_index(iv, ctx->srtcp.salt, ssrc, index);
    if (aes_cm_xor((EVP_CIPHER_CTX *)ctx->evp_srtcp, ctx->srtcp.enc, iv,
                   buf + SRTP_RTCP_HDR_LEN, len - SRTP_RTCP_HDR_LEN) != 0)
        return SRTP_ERR_ARG;

    /* 2) 追加 4 字节 E|index（E=1 表示 payload 已加密） */
    wr_be32(buf + len, 0x80000000u | index);
    len += SRTP_SRTCP_IDX_LEN;

    /* 3) 认证：覆盖整个包 + index 字段 */
    if (calc_tag((HMAC_CTX *)ctx->hmac_srtcp, ctx->srtcp.auth, buf, len, buf + len) != 0)
        return SRTP_ERR_ARG;

    *out_len = len + SRTP_TAG_LEN;
    ctx->tx_rtcp_index = (index + 1u) & 0x7fffffffu;
    ctx->stats.rtcp_tx++;
    return SRTP_OK;
}

/* ------------------------------------------------------------ SRTCP 接收 */

srtp_status srtcp_unprotect(srtp_ctx *ctx, uint8_t *buf, size_t len, size_t *out_len)
{
    uint32_t ssrc, field, index;
    uint8_t iv[EVP_IV_LEN];
    uint8_t tag[SRTP_TAG_LEN];
    size_t body_len;
    int pk;

    if (!ctx || !buf || !out_len)
        return SRTP_ERR_ARG;
    if (len < SRTP_RTCP_HDR_LEN + SRTP_SRTCP_IDX_LEN + SRTP_TAG_LEN)
        return SRTP_ERR_SHORT;

    body_len = len - SRTP_TAG_LEN;

    /* 1) 认证 */
    if (calc_tag((HMAC_CTX *)ctx->hmac_srtcp, ctx->srtcp.auth, buf, body_len, tag) != 0)
        return SRTP_ERR_ARG;
    if (!ct_equal(tag, buf + body_len, SRTP_TAG_LEN)) {
        ctx->stats.rx_auth_fail++;
        return SRTP_ERR_AUTH;
    }

    /* 2) 取 E|index */
    field = rd_be32(buf + body_len - SRTP_SRTCP_IDX_LEN);
    index = field & 0x7fffffffu;

    ssrc = rd_be32(buf + 4);
    if (ctx->ssrc == 0)
        ctx->ssrc = ssrc;
    else if (ctx->ssrc != ssrc)
        return SRTP_ERR_SSRC;

    /* 3) 重放 */
    pk = replay_peek(&ctx->rx_rtcp_replay, (uint64_t)index);
    if (pk == REPLAY_PEEK_LATE) {
        ctx->stats.rx_late++;
        return SRTP_ERR_REPLAY;
    }
    if (pk == REPLAY_PEEK_DUP) {
        ctx->stats.rx_replay++;
        return SRTP_ERR_REPLAY;
    }

    /* 4) 解密（仅当 E 标志置位） */
    body_len -= SRTP_SRTCP_IDX_LEN;
    if (field & 0x80000000u) {
        iv_from_rtcp_index(iv, ctx->srtcp.salt, ssrc, index);
        if (aes_cm_xor((EVP_CIPHER_CTX *)ctx->evp_srtcp, ctx->srtcp.enc, iv,
                       buf + SRTP_RTCP_HDR_LEN, body_len - SRTP_RTCP_HDR_LEN) != 0)
            return SRTP_ERR_ARG;
    }

    replay_commit(&ctx->rx_rtcp_replay, (uint64_t)index);
    ctx->rx_rtcp_index = index;
    ctx->stats.rtcp_rx++;
    *out_len = body_len;
    return SRTP_OK;
}

/* -------------------------------------------------------------- 诊断 */

const char *srtp_strerror(srtp_status st)
{
    switch (st) {
    case SRTP_OK:          return "OK";
    case SRTP_ERR_AUTH:    return "AUTH_FAIL";
    case SRTP_ERR_REPLAY:  return "REPLAY";
    case SRTP_ERR_SHORT:   return "SHORT";
    case SRTP_ERR_LARGE:   return "TOO_LARGE";
    case SRTP_ERR_RTP_HDR: return "BAD_RTP_HEADER";
    case SRTP_ERR_NOMEM:   return "NOMEM";
    case SRTP_ERR_ARG:     return "BAD_ARG";
    case SRTP_ERR_SSRC:    return "SSRC_MISMATCH";
    }
    return "UNKNOWN";
}
