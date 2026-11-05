/*
 * srtp_test.c -- SRTP 核心的真机自检程序（aarch64 / X6 Pro）
 *
 * 测试分四组，全部必须 ALL PASS：
 *
 *   [1] RFC 3711 附录 B 权威测试向量（外部权威值，不是自己验证自己）
 *       B.2  AES-CM keystream（含跨越 0xFEFF/0xFF00 计数器的回绕）
 *       B.3  密钥派生，label 0x00/0x01/0x02，其中 auth key 用 94 octet
 *            以覆盖 KDF 的"多块 + 末尾截断"路径
 *
 *   [2] 完整的包级黄金向量（由 host 上的 OpenSSL 3.5.9 CLI 独立算出，
 *       与设备上的 libcrypto 1.1.1l 是两个不同实现，见 gen.py）
 *       SRTP 三例（含 ROC != 0）+ SRTCP 一例
 *
 *   [3] 自洽性：100 个包，制造丢包与乱序投递，全部正确还原；
 *       重放包必须被拒；篡改包必须被拒。
 *
 *   [4] 性能：1500 字节包，测 protect/unprotect 的 pps。
 *
 * 编译（host 上）：
 *   env/pyzig/ziglang/zig.exe cc -target aarch64-linux-gnu.2.17 -O2 -g0 \
 *       -I PenYS/native/csrc/rtp PenYS/native/csrc/rtp/srtp.c \
 *       PenYS/native/csrc/rtp/srtp_test.c \
 *       .tmp/feas/libs/libcrypto.so.1.1 -o .tmp/srtpwork/srtp_test
 *
 * 运行（设备上）：
 *   LD_LIBRARY_PATH=/usr/lib ./srtp_test
 */

#include "srtp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ------------------------------------------------------------ 测试框架 */

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *what)
{
    g_pass++;
    printf("  [PASS] %s\n", what);
}

static void fail(const char *what, const char *detail)
{
    g_fail++;
    printf("  [FAIL] %s%s%s\n", what, detail ? " : " : "",
           detail ? detail : "");
}

static void check_int(const char *what, long got, long want)
{
    char buf[128];
    if (got == want) {
        ok(what);
    } else {
        snprintf(buf, sizeof(buf), "got=%ld want=%ld", got, want);
        fail(what, buf);
    }
}

static void hexdump_line(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        printf("%02X", p[i]);
}

/* 比较两块内存并打印首个差异偏移 */
static void check_mem(const char *what, const uint8_t *got,
                      const uint8_t *want, size_t n)
{
    size_t i;
    char buf[256];

    for (i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            snprintf(buf, sizeof(buf),
                     "first diff at byte %u: got=%02X want=%02X", (unsigned)i,
                     got[i], want[i]);
            fail(what, buf);
            printf("         got  = ");
            hexdump_line(got, n < 64 ? n : 64);
            printf("\n         want = ");
            hexdump_line(want, n < 64 ? n : 64);
            printf("\n");
            return;
        }
    }
    ok(what);
}

/* 无条件打印一块十六进制，作为“实际值”取证用（验收报告要原样贴） */
static void show_mem(const char *tag, const uint8_t *p, size_t n)
{
    size_t i;
    printf("         %-9s = ", tag);
    for (i = 0; i < n; i++)
        printf("%02X", p[i]);
    printf("\n");
}

/* 大端 32 位读取，测试里比对 E|index 字段用 */
static uint32_t rd_be32_local(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        int v;
        if (*s == ' ' || *s == '\n' || *s == '\t' || *s == ':')
            continue;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else return (size_t)-1;
        if (hi < 0) hi = v;
        else { if (n >= cap) return (size_t)-1; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    return (hi < 0) ? n : (size_t)-1;
}

/* =========================================================== [1] RFC B.2 */

/*
 * B.2 的 keystream 全长 1044512 octets（65282 个 AES block）。
 * 这里真的把这 1MB 全跑一遍，只比对 RFC 给出的 6 个块：
 *   前 3 块（counter ...FD0000 / ...0001 / ...0002）
 *   后 3 块（counter ...F EFF 之前那三个：...FEF F / ...FF00 / ...FF01）
 * 这样才能验证"计数器跨 0xFF 边界正确进位"以及"分段生成时偏移正确"。
 */
static uint8_t g_ks[1044512];

static void test_rfc_b2(void)
{
    static const uint8_t key[16] = {
        0x2B,0x7E,0x15,0x16,0x28,0xAE,0xD2,0xA6,
        0xAB,0xF7,0x15,0x88,0x09,0xCF,0x4F,0x3C
    };
    static const uint8_t iv0[16] = {
        0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,
        0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0x00,0x00
    };
    static const char *want_head[3] = {
        "E03EAD0935C95E80E166B16DD92B4EB4",
        "D23513162B02D0F72A43A2FE4A5F97AB",
        "41E95B3BB0A2E8DD477901E4FCA894C0"
    };
    static const char *want_tail[3] = {
        "EC8CDF7398607CB0F2D21675EA9EA1E4",  /* counter ...FCFDFEFF */
        "362B7C3C6773516318A077D7FC5073AE",  /* counter ...FCFDFF00 */
        "6A2CC3787889374FBEB4C81B17BA6C44"   /* counter ...FCFDFF01 */
    };
    uint8_t want[16];
    char label[96];
    int i;

    printf("\n[1] RFC 3711 Appendix B.2 -- AES-CM keystream\n");
    printf("    segment = 1044512 octets (%u AES blocks)\n", 1044512u / 16u);

    if (srtp_aes_cm_keystream(key, iv0, g_ks, sizeof(g_ks)) != 0) {
        fail("generate 1044512-octet keystream", "returned -1");
        return;
    }
    ok("generate 1044512-octet keystream");

    for (i = 0; i < 3; i++) {
        unhex(want_head[i], want, sizeof(want));
        snprintf(label, sizeof(label),
                 "B.2 block %d (counter ...%04X) matches RFC", i,
                 (unsigned)i);
        check_mem(label, g_ks + (size_t)i * 16u, want, 16);
    }
    show_mem("ks[blk0]", g_ks + 0, 16);
    show_mem("ks[blk1]", g_ks + 16, 16);
    show_mem("ks[blk2]", g_ks + 32, 16);
    /* 尾段：65282 个 block，最后一个 block 号 65281 = 0xFF01 */
    for (i = 0; i < 3; i++) {
        size_t blk = (size_t)(65279u + (unsigned)i);   /* 0xFEFF,0xFF00,0xFF01 */
        unhex(want_tail[i], want, sizeof(want));
        snprintf(label, sizeof(label),
                 "B.2 tail block (counter ...%04X) matches RFC",
                 (unsigned)blk);
        check_mem(label, g_ks + blk * 16u, want, 16);
    }
    show_mem("ks[FEFF]", g_ks + (size_t)65279u * 16u, 16);
    show_mem("ks[FF00]", g_ks + (size_t)65280u * 16u, 16);
    show_mem("ks[FF01]", g_ks + (size_t)65281u * 16u, 16);
}

/* =========================================================== [1] RFC B.3 */

static void test_rfc_b3(void)
{
    /* master key / salt 原文逐字节 */
    uint8_t mk_raw[16], ms_raw[14];
    uint8_t got[128], want[128];
    srtp_master_key mk;
    srtp_keys ks;
    srtp_status st;

    printf("\n[1] RFC 3711 Appendix B.3 -- key derivation\n");

    unhex("E1F97A0D3E018BE0D64FA32C06DE4139", mk_raw, sizeof(mk_raw));
    unhex("0EC675AD498AFEEBB6960B3AABE6", ms_raw, sizeof(ms_raw));

    memset(&mk, 0, sizeof(mk));
    memcpy(mk.master_key, mk_raw, 16);
    memcpy(mk.master_salt, ms_raw, 14);
    mk.master_salt_len = 14;
    mk.kdr = 0;

    /* label 0x00 -> cipher key */
    st = srtp_kdf_derive_raw(&mk, 0x00, got, 16);
    check_int("kdf label 0x00 returns OK", (long)st, 0);
    unhex("C61E7A93744F39EE10734AFE3FF7A087", want, 16);
    check_mem("B.3 cipher key (label 0x00)", got, want, 16);
    show_mem("want", want, 16);
    show_mem("actual", got, 16);

    /* label 0x02 -> cipher salt，14 octet */
    st = srtp_kdf_derive_raw(&mk, 0x02, got, 14);
    check_int("kdf label 0x02 returns OK", (long)st, 0);
    unhex("30CBBC08863D8C85D49DB34A9AE1", want, 14);
    check_mem("B.3 cipher salt (label 0x02)", got, want, 14);
    show_mem("want", want, 14);
    show_mem("actual", got, 14);

    /*
     * label 0x01 -> auth key。
     * RFC 这里故意用 94 octet（不是 §5.2 默认的 160 bit），
     * 正好覆盖"输出跨多个 AES block + 最后一块截断"的路径。
     * 原文 6 行，最后一行只有 14 字节（94 = 6*16 - 2）。
     */
    st = srtp_kdf_derive_raw(&mk, 0x01, got, 94);
    check_int("kdf label 0x01 (94 octet) returns OK", (long)st, 0);
    unhex("CEBE321F6FF7716B6FD4AB49AF256A15"
          "6D38BAA48F0A0ACF3C34E2359E6CDBCE"
          "E049646C43D9327AD175578EF7227098"
          "6371C10C9A369AC2F94A8C5FBCDDDC25"
          "6D6E919A48B610EF17C2041E47403576"
          "6B68642C59BBFC2F34DB60DBDFB2", want, sizeof(want));
    check_mem("B.3 auth key (label 0x01, 94 octet, multi-block + truncation)",
              got, want, 94);
    show_mem("want", want, 94);
    show_mem("actual", got, 94);

    /* 高层 API：一次性派生 SRTP 的 3 个 key，应与逐 label 派生一致 */
    st = srtp_derive_session_keys(&mk, &ks, NULL);
    check_int("srtp_derive_session_keys returns OK", (long)st, 0);
    unhex("C61E7A93744F39EE10734AFE3FF7A087", want, 16);
    check_mem("derived SRTP k_e == RFC cipher key", ks.enc, want, 16);
    unhex("30CBBC08863D8C85D49DB34A9AE1", want, 14);
    check_mem("derived SRTP k_s == RFC cipher salt", ks.salt, want, 14);
    unhex("CEBE321F6FF7716B6FD4AB49AF256A15", want, 16);
    check_mem("derived SRTP k_a[0..15] == RFC auth key head", ks.auth, want, 16);
}

/* ================================================= [2] 包级黄金向量 */

static void test_golden_packets(void)
{
    srtp_master_key mk;
    srtp_ctx tx, rx;
    uint8_t buf[SRTP_MAX_PACKET];
    uint8_t want[SRTP_MAX_PACKET];
    size_t n, wn, outn;
    srtp_status st;

    printf("\n[2] packet-level golden vectors (host OpenSSL 3.5.9 CLI)\n");

    /* 与 B.3 相同的 master key/salt，session key 即上面派生的值 */
    memset(&mk, 0, sizeof(mk));
    unhex("E1F97A0D3E018BE0D64FA32C06DE4139", mk.master_key, 16);
    unhex("0EC675AD498AFEEBB6960B3AABE6", mk.master_salt, 14);
    mk.master_salt_len = 14;
    mk.kdr = 0;

    /* ---- SRTP case A: seq=0x1234 roc=0 ssrc=0xDEADBEEF ---- */
    {
        static const char *pkt_hex =
            "8060123400000000DEADBEEF76724382192E354F565D344CE4768828E86DF1BF"
            "A45D5D235C4757AAD566";
        n = unhex(
            "8060123400000000DEADBEEF"
            "68656C6C6F207372747020776F726C6420746869", buf, sizeof(buf));
        st = srtp_init(&tx, &mk, 0xDEADBEEFu);
        check_int("A: srtp_init", (long)st, 0);
        srtp_set_send_seq(&tx, 0x1234);
        st = srtp_protect(&tx, buf, n, &outn);
        check_int("A: protect returns OK", (long)st, 0);
        wn = unhex(pkt_hex, want, sizeof(want));
        check_int("A: protected length (12 hdr + 20 ct + 10 tag)", (long)outn,
                  (long)wn);
        check_mem("A: ciphertext+tag matches host reference", buf, want, wn);
        show_mem("want", want, wn);
        show_mem("actual", buf, wn);

        /* 反向：解密回原文 */
        st = srtp_init(&rx, &mk, 0xDEADBEEFu);
        check_int("A: rx srtp_init", (long)st, 0);
        srtp_set_recv_roc(&rx, 0);
        memcpy(want, buf, outn);
        st = srtp_unprotect(&rx, buf, outn, &outn);
        check_int("A: unprotect returns OK", (long)st, 0);
        check_int("A: recovered length", (long)outn, (long)n);
        {
            uint8_t exp[SRTP_MAX_PACKET];
            size_t en = unhex(
                "8060123400000000DEADBEEF"
                "68656C6C6F207372747020776F726C6420746869", exp, sizeof(exp));
            check_mem("A: recovered plaintext == original RTP", buf, exp, en);
        }
        srtp_deinit(&tx);
        srtp_deinit(&rx);
    }

    /* ---- SRTP case B: seq=0x0001 roc=0 ssrc=0xCAFEBABE, 短包 ---- */
    {
        static const char *pkt_hex =
            "806000010000F000CAFEBABECA09D50C48ECB71378758F17B5D3";
        n = unhex("806000010000F000CAFEBABE61626364", buf, sizeof(buf));
        st = srtp_init(&tx, &mk, 0xCAFEBABEu);
        check_int("B: srtp_init", (long)st, 0);
        srtp_set_send_seq(&tx, 0x0001);
        st = srtp_protect(&tx, buf, n, &outn);
        check_int("B: protect returns OK", (long)st, 0);
        wn = unhex(pkt_hex, want, sizeof(want));
        check_int("B: protected length", (long)outn, (long)wn);
        check_mem("B: ciphertext+tag matches host reference", buf, want, wn);
        srtp_deinit(&tx);
    }

    /* ---- SRTP case C: seq=0xFFFE roc=5 ssrc=0x01020304, 非零 ROC ---- */
    {
        static const char *pkt_hex =
            "8060FFFE1122334401020304BDCE0252FF2F74D0C7DAFFA061E8058EDDD507A0"
            "2241B25CC7D728E72F61BC0856519B6371E6DDC932E6ADA1B1F2A4912368";
        n = unhex("8060FFFE1122334401020304"
                  "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"
                  "2021222324252627", buf, sizeof(buf));
        st = srtp_init(&tx, &mk, 0x01020304u);
        check_int("C: srtp_init", (long)st, 0);
        /*
         * 直接把发送侧的 index 摆到 (ROC=5, SEQ=0xFFFE)。
         * srtp_ctx 是公开结构体专为这种向量测试留的：真实链路里 ROC 是
         * 发满 65536 个包自然累加出来的，测试里不必真跑 32 万个包。
         */
        srtp_set_send_seq(&tx, 0xFFFE);
        tx.tx_roc = 5;
        st = srtp_protect(&tx, buf, n, &outn);
        check_int("C: protect returns OK", (long)st, 0);
        wn = unhex(pkt_hex, want, sizeof(want));
        check_int("C: protected length", (long)outn, (long)wn);
        check_mem("C: ciphertext+tag matches host reference (nonzero ROC)",
                  buf, want, wn);
        show_mem("want", want, wn);
        show_mem("actual", buf, wn);

        /* 接收侧同样摆好 ROC 再解，必须还原原文 */
        {
            uint8_t exp[SRTP_MAX_PACKET];
            size_t en = unhex("8060FFFE1122334401020304"
                              "000102030405060708090A0B0C0D0E0F1011121314151617"
                              "18191A1B1C1D1E1F2021222324252627", exp, sizeof(exp));
            st = srtp_init(&rx, &mk, 0x01020304u);
            check_int("C: rx srtp_init", (long)st, 0);
            srtp_set_recv_roc(&rx, 5);
            rx.rx_s_l = 0xFFFD;      /* 让 0xFFFE 落在窗口内 */
            rx.rx_started = 1;
            st = srtp_unprotect(&rx, buf, outn, &outn);
            check_int("C: unprotect returns OK", (long)st, 0);
            check_mem("C: recovered plaintext == original RTP", buf, exp, en);
            srtp_deinit(&rx);
        }
        srtp_deinit(&tx);
    }

    /* ---- SRTCP ---- */
    /*
     * 注意：SRTCP 用的是**自己的一套 KDF label**（RFC 3711 §4.3.2）：
     *   0x03 -> SRTCP 加密密钥, 0x04 -> SRTCP 认证密钥, 0x05 -> SRTCP 盐。
     * 不要拿 SRTP 的 0x00/0x01/0x02 去算 SRTCP，这是实现里最容易搞错的地方。
     * 下面的期望值由 host OpenSSL 3.5.9 用 label 0x03/0x04/0x05 独立推导。
     */
    {
        static const char *pkt_hex =
            "81C90003DEADBEEF05ED5DEDCB73E8184CAAFE1A9D8A4C442319542480000005"
            "371ECBEA98E1B0F6DE07";
        srtp_ctx ctx;
        srtp_keys k_srtp, k_srtcp;
        uint8_t want2[64];

        printf("\n[2] SRTCP uses its own KDF labels 0x03/0x04/0x05\n");
        /* mk 已就绪，直接派生两套 key */
        st = srtp_derive_session_keys(&mk, &k_srtp, &k_srtcp);
        check_int("derive both SRTP and SRTCP session keys", (long)st, 0);
        unhex("4C1AA45A81F73D61C800BBB00FBB1EAA", want2, 16);
        check_mem("SRTCP k_e (label 0x03) matches host reference",
                  k_srtcp.enc, want2, 16);
        unhex("9581C7AD87B3E530BF3E4454A8B3", want2, 14);
        check_mem("SRTCP k_s (label 0x05) matches host reference",
                  k_srtcp.salt, want2, 14);
        unhex("8D54534FEB49AE8E7993A6BD0B844FC323A93DFD", want2, 20);
        check_mem("SRTCP k_a (label 0x04) matches host reference",
                  k_srtcp.auth, want2, 20);
        /* SRTP 与 SRTCP 的 key 必须不同，否则说明 label 用错了 */
        check_int("SRTP k_e != SRTCP k_e", memcmp(k_srtp.enc, k_srtcp.enc, 16) != 0, 1);

        n = unhex("81C90003DEADBEEF525443502D5245504F52542D444154412D58595A",
                  buf, sizeof(buf));
        st = srtp_init(&ctx, &mk, 0xDEADBEEFu);
        check_int("SRTCP: srtp_init", (long)st, 0);
        /* 让 tx_rtcp_index 走到 5 */
        {
            uint8_t tmp[64];
            size_t tn;
            int i;
            for (i = 0; i < 5; i++) {
                memset(tmp, 0, sizeof(tmp));
                tmp[0] = 0x81; tmp[1] = 0xC9; tmp[4] = 0xDE; tmp[5] = 0xAD;
                tmp[6] = 0xBE; tmp[7] = 0xEF;
                srtcp_protect(&ctx, tmp, 20, &tn);
            }
        }
        st = srtcp_protect(&ctx, buf, n, &outn);
        check_int("SRTCP: protect returns OK", (long)st, 0);
        wn = unhex(pkt_hex, want, sizeof(want));
        check_int("SRTCP: protected length (pkt + 4 idx + 10 tag)", (long)outn,
                  (long)wn);
        check_mem("SRTCP: ciphertext+E|index+tag matches host reference",
                  buf, want, wn);
        show_mem("want", want, wn);
        show_mem("actual", buf, wn);
        srtp_deinit(&ctx);
    }
}

/* ============================================ [3] ROC / 乱序 / 丢包 */

static void test_roc_rollover(void)
{
    srtp_master_key mk;
    srtp_ctx tx, rx;
    uint8_t buf[128];
    size_t n, outn;
    int i;
    int all_ok = 1;

    printf("\n[3] ROC estimation across sequence wraparound\n");

    memset(&mk, 0, sizeof(mk));
    unhex("E1F97A0D3E018BE0D64FA32C06DE4139", mk.master_key, 16);
    unhex("0EC675AD498AFEEBB6960B3AABE6", mk.master_salt, 14);
    mk.master_salt_len = 14;

    srtp_init(&tx, &mk, 0x11223344u);
    srtp_init(&rx, &mk, 0x11223344u);
    srtp_set_send_seq(&tx, 0xFFFB);

    /*
     * 送 0xFFFB..0xFFFF 再来 0x0000..0x0004：跨 16-bit 回绕，
     * 接收侧必须把后 5 个包的 ROC 估成 1，否则解密全错。
     */
    for (i = 0; i < 10; i++) {
        uint16_t seq = (uint16_t)(0xFFFB + i);
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x80; buf[1] = 0x60;
        buf[2] = (uint8_t)(seq >> 8); buf[3] = (uint8_t)seq;
        buf[8] = 0x11; buf[9] = 0x22; buf[10] = 0x33; buf[11] = 0x44;
        memset(buf + 12, (uint8_t)(0x40 + i), 20);
        n = 32;
        srtp_protect(&tx, buf, n, &outn);
        if (srtp_unprotect(&rx, buf, outn, &outn) != SRTP_OK)
            all_ok = 0;
        else if (buf[12] != (uint8_t)(0x40 + i))
            all_ok = 0;
    }
    check_int("10 packets across 0xFFFF->0x0000 wrap all decode", all_ok, 1);
    check_int("receiver ROC advanced to 1", (long)rx.rx_roc, 1);

    srtp_deinit(&tx);
    srtp_deinit(&rx);
}

static void test_selfconsistency(void)
{
    srtp_master_key mk;
    srtp_ctx tx, rx;
    uint8_t origin[100][SRTP_MAX_PACKET];
    uint8_t wire[100][SRTP_MAX_PACKET + 32];
    size_t   wire_len[100];
    size_t   orig_len[100];
    size_t   outn;
    int      order[100];
    int      i, j, bad = 0, n_ok = 0, n_replay_rejected = 0, n_tamper_rejected = 0;
    /* 故意丢掉的序号（下标） */
    static const int dropped[] = { 7, 23, 24, 55, 81, 99 };

    printf("\n[3] self-consistency: 100 packets, loss + reorder + replay\n");

    memset(&mk, 0, sizeof(mk));
    unhex("00A1B2C3D4E5F60718293A4B5C6D7E8F", mk.master_key, 16);
    unhex("112233445566778899AABBCCDDEE", mk.master_salt, 14);
    mk.master_salt_len = 14;

    srtp_init(&tx, &mk, 0xC0FFEE01u);
    srtp_init(&rx, &mk, 0xC0FFEE01u);
    srtp_set_send_seq(&tx, 0x0100);

    /* 生成 100 个包，payload 长度在 12..1500 之间变化 */
    for (i = 0; i < 100; i++) {
        uint16_t seq = (uint16_t)(0x0100 + i);
        size_t paylen = 12u + (size_t)((i * 137) % 1489);
        memset(origin[i], 0, sizeof(origin[i]));
        origin[i][0] = 0x80; origin[i][1] = 0x60;
        origin[i][2] = (uint8_t)(seq >> 8); origin[i][3] = (uint8_t)seq;
        for (j = 0; j < 4; j++)
            origin[i][4 + j] = (uint8_t)(0x10000000u >> (8 * (3 - j)));
        origin[i][8] = 0xC0; origin[i][9] = 0xFF; origin[i][10] = 0xEE;
        origin[i][11] = 0x01;
        for (j = 0; j < (int)paylen; j++)
            origin[i][12 + j] = (uint8_t)(i * 7 + j);
        orig_len[i] = 12 + paylen;
        memcpy(wire[i], origin[i], orig_len[i]);
        if (srtp_protect(&tx, wire[i], orig_len[i], &wire_len[i]) != SRTP_OK) {
            bad++;
            continue;
        }
    }
    check_int("all 100 packets protected", 100 - bad, 100);

    /* 构造乱序投递顺序：按 50 个一组，组内逆序。
       组内最大乱序跨度 49 < 重放窗口 64，所以都在窗口内。
       真正的“超窗太晚的包”单独在下面测。 */
    {
        int k = 0;
        int base;
        for (base = 0; base < 100; base += 50) {
            int hi = base + 49;
            for (i = (hi < 100 ? hi : 99); i >= base; i--)
                order[k++] = i;
        }
        check_int("delivery order built (50-packet blocks, reversed)",
                  k, 100);
    }

    for (i = 0; i < 100; i++) {
        int idx = order[i];
        int skip = 0;
        uint8_t tmp[SRTP_MAX_PACKET + 32];
        for (j = 0; j < (int)(sizeof(dropped) / sizeof(dropped[0])); j++)
            if (dropped[j] == idx) skip = 1;
        if (skip) continue;

        memcpy(tmp, wire[idx], wire_len[idx]);
        {
            srtp_status ur = srtp_unprotect(&rx, tmp, wire_len[idx], &outn);
            if (ur != SRTP_OK) {
                printf("      unprotect failed for idx=%d: %s (%d)\n", idx,
                       srtp_strerror(ur), (int)ur);
                bad++;
                continue;
            }
        }
        if (outn != orig_len[idx] ||
            memcmp(tmp, origin[idx], orig_len[idx]) != 0) {
            printf("      plaintext mismatch for idx=%d\n", idx);
            bad++;
            continue;
        }
        n_ok++;
    }
    check_int("out-of-order delivery: every non-dropped packet decodes",
              n_ok, 100 - (int)(sizeof(dropped) / sizeof(dropped[0])));
    check_int("no decode errors", bad, 0);

    /* 重放：重投已成功解出的包，必须被拒 */
    for (i = 0; i < 100; i++) {
        uint8_t tmp[SRTP_MAX_PACKET + 32];
        int skip = 0;
        for (j = 0; j < (int)(sizeof(dropped) / sizeof(dropped[0])); j++)
            if (dropped[j] == i) skip = 1;
        if (skip) continue;
        memcpy(tmp, wire[i], wire_len[i]);
        if (srtp_unprotect(&rx, tmp, wire_len[i], &outn) == SRTP_ERR_REPLAY)
            n_replay_rejected++;
    }
    check_int("every replayed packet rejected as REPLAY", n_replay_rejected,
              100 - (int)(sizeof(dropped) / sizeof(dropped[0])));

    /* 篡改：翻转密文一个 bit，必须 AUTH_FAIL 且不得改动接收状态 */
    {
        uint8_t tmp[SRTP_MAX_PACKET + 32];
        size_t before_highest = rx.rx_replay.highest;
        for (i = 0; i < 100; i++) {
            memcpy(tmp, wire[i], wire_len[i]);
            tmp[wire_len[i] - 1] ^= 0x01;     /* 打在 tag 上 */
            if (srtp_unprotect(&rx, tmp, wire_len[i], &outn) == SRTP_ERR_AUTH)
                n_tamper_rejected++;
        }
        check_int("tampered packets rejected as AUTH_FAIL", n_tamper_rejected, 100);
        check_int("replay window untouched by forged packets",
                  (long)rx.rx_replay.highest, (long)before_highest);
    }

    printf("      rx stats: packets=%u auth_fail=%u replay=%u late=%u\n",
           rx.stats.rx_packets, rx.stats.rx_auth_fail, rx.stats.rx_replay,
           rx.stats.rx_late);

    srtp_deinit(&tx);
    srtp_deinit(&rx);
}

/* ------------------------------------------ [3] 重放窗口边界专项 */

/*
 * 64 包滑动窗口的三个边界行为必须分开验证：
 *   (a) 窗口内的重复包        -> REPLAY（被拒）
 *   (b) 窗口外的旧包（太晚）  -> REPLAY（被拒）
 *   (c) 窗口内的乱序新包      -> 接受
 * 这三条混在自洽性测试里看不清楚，所以单独做一遍并打印判定值。
 */
static void test_replay_window(void)
{
    srtp_master_key mk;
    srtp_ctx tx, rx;
    uint8_t wire[130][SRTP_MAX_PACKET];
    size_t   wlen[130];
    uint8_t  buf[SRTP_MAX_PACKET];
    size_t   outn;
    int i, rc;

    printf("\n[3] replay window boundaries (SRTP_REPLAY_WINDOW = %d)\n",
           SRTP_REPLAY_WINDOW);

    memset(&mk, 0, sizeof(mk));
    unhex("00112233445566778899AABBCCDDEEFF", mk.master_key, 16);
    unhex("A0A1A2A3A4A5A6A7A8A9AAABACAD", mk.master_salt, 14);
    mk.master_salt_len = 14;

    srtp_init(&tx, &mk, 0x0BADC0DEu);
    srtp_init(&rx, &mk, 0x0BADC0DEu);
    srtp_set_send_seq(&tx, 0x0000);

    for (i = 0; i < 130; i++) {
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x80; buf[1] = 0x60;
        buf[2] = (uint8_t)(i >> 8); buf[3] = (uint8_t)i;
        buf[8] = 0x0B; buf[9] = 0xAD; buf[10] = 0xC0; buf[11] = 0xDE;
        memset(buf + 12, (uint8_t)i, 100);
        srtp_protect(&tx, buf, 112, &wlen[i]);
        memcpy(wire[i], buf, wlen[i]);
    }

    /* 按 0..99 顺序收下（故意跳过 40，留给下面的乱序用例），
       窗口最高位停在 99 */
    for (i = 0; i < 100; i++) {
        if (i == 40)
            continue;
        memcpy(buf, wire[i], wlen[i]);
        srtp_unprotect(&rx, buf, wlen[i], &outn);
    }
    check_int("(window) 99 sequential packets accepted",
              (long)rx.rx_replay.highest, 99);

    /* (a) 刚收过的包重放 -> 必须 REPLAY */
    memcpy(buf, wire[99], wlen[99]);
    rc = (int)srtp_unprotect(&rx, buf, wlen[99], &outn);
    check_int("(a) duplicate of highest (seq 99) rejected as REPLAY",
              rc == SRTP_ERR_REPLAY, 1);
    printf("         rc=%d (%s)\n", rc, srtp_strerror((srtp_status)rc));

    memcpy(buf, wire[50], wlen[50]);
    rc = (int)srtp_unprotect(&rx, buf, wlen[50], &outn);
    check_int("(a) duplicate inside window (seq 99-49) rejected as REPLAY",
              rc == SRTP_ERR_REPLAY, 1);
    printf("         rc=%d (%s)\n", rc, srtp_strerror((srtp_status)rc));

    /* (c) 窗口内的乱序新包 -> 接受。当前最高 99，窗口下沿 99-63=36。
           取 40（在窗口内，且尚未收到过）。 */
    memcpy(buf, wire[40], wlen[40]);
    rc = (int)srtp_unprotect(&rx, buf, wlen[40], &outn);
    check_int("(c) out-of-order seq 40 (inside 64-pkt window) accepted",
              rc == SRTP_OK, 1);
    printf("         rc=%d (%s)\n", rc, srtp_strerror((srtp_status)rc));

    /* (b) 窗口外太晚的包 -> 必须拒。窗口下沿 36，取 35 刚刚越界。 */
    memcpy(buf, wire[35], wlen[35]);
    rc = (int)srtp_unprotect(&rx, buf, wlen[35], &outn);
    check_int("(b) seq 35 (1 past the 64-pkt window) rejected",
              rc == SRTP_ERR_REPLAY, 1);
    printf("         rc=%d (%s)\n", rc, srtp_strerror((srtp_status)rc));

    memcpy(buf, wire[10], wlen[10]);
    rc = (int)srtp_unprotect(&rx, buf, wlen[10], &outn);
    check_int("(b) seq 10 (far outside window) rejected", 
              rc == SRTP_ERR_REPLAY, 1);
    printf("         rc=%d (%s)\n", rc, srtp_strerror((srtp_status)rc));

    /* 推进窗口后，旧包仍然被拒（窗口是滑动的，不是重置） */
    for (i = 100; i < 130; i++) {
        memcpy(buf, wire[i], wlen[i]);
        srtp_unprotect(&rx, buf, wlen[i], &outn);
    }
    check_int("(window) highest advanced to 129",
              (long)rx.rx_replay.highest, 129);
    memcpy(buf, wire[60], wlen[60]);
    rc = (int)srtp_unprotect(&rx, buf, wlen[60], &outn);
    check_int("(b) seq 60 slid out of window, still rejected",
              rc == SRTP_ERR_REPLAY, 1);

    printf("      rx stats: replay=%u late=%u\n",
           rx.stats.rx_replay, rx.stats.rx_late);

    srtp_deinit(&tx);
    srtp_deinit(&rx);
}

/* --------------------------------------- [2] SRTCP 收发往返 + 重放 */

/*
 * 上面的 SRTCP 用例只验证了发送方向的黄金密文，接收方向
 * (srtcp_unprotect) 完全没有覆盖。这里补上往返 + 认证失败 + 重放。
 */
static void test_srtcp_roundtrip(void)
{
    srtp_master_key mk;
    srtp_ctx tx, rx;
    uint8_t plain[128], buf[128];
    size_t pn, outn;
    int i, rc;

    printf("\n[2] SRTCP round-trip / auth / replay\n");

    memset(&mk, 0, sizeof(mk));
    unhex("E1F97A0D3E018BE0D64FA32C06DE4139", mk.master_key, 16);
    unhex("0EC675AD498AFEEBB6960B3AABE6", mk.master_salt, 14);
    mk.master_salt_len = 14;

    srtp_init(&tx, &mk, 0xDEADBEEFu);
    srtp_init(&rx, &mk, 0xDEADBEEFu);

    /* 构造一个 44 字节的假 RTCP BYE，payload 里放可校验的花纹 */
    pn = 44;
    memset(plain, 0, sizeof(plain));
    plain[0] = 0x81; plain[1] = 0xC9; plain[2] = 0x00; plain[3] = 0x07;
    plain[4] = 0xDE; plain[5] = 0xAD; plain[6] = 0xBE; plain[7] = 0xEF;
    for (i = 8; i < (int)pn; i++)
        plain[i] = (uint8_t)(0x30 + i);

    memcpy(buf, plain, pn);
    rc = (int)srtcp_protect(&tx, buf, pn, &outn);
    check_int("SRTCP rt: protect OK", rc == SRTP_OK, 1);
    check_int("SRTCP rt: protected length = 44 + 4 + 10", (long)outn, 58);
    check_int("SRTCP rt: payload really encrypted",
              memcmp(buf + 8, plain + 8, 30) != 0, 1);
    {
        uint8_t f[4];
        memcpy(f, buf + 44, 4);
        check_int("SRTCP rt: E flag set in index field",
                  (f[0] & 0x80) != 0, 1);
        check_int("SRTCP rt: index == 0",
                  (long)rd_be32_local(f) & 0x7fffffffL, 0);
    }

    rc = (int)srtcp_unprotect(&rx, buf, outn, &outn);
    check_int("SRTCP rt: unprotect OK", rc == SRTP_OK, 1);
    check_int("SRTCP rt: recovered length", (long)outn, (long)pn);
    check_mem("SRTCP rt: recovered RTCP identical to original", buf, plain, pn);

    /* 重放：把一个新包连投两次，第二次必须被拒。
       注意 unprotect 是原地解密，第二次必须用**保留的线上字节**，
       否则投进去的是被解过一次的缓冲区，会被当成 AUTH_FAIL。 */
    {
        uint8_t p2[128], wire2[128];
        size_t p2len, tmp;

        memcpy(p2, plain, pn);
        srtcp_protect(&tx, p2, pn, &p2len);
        memcpy(wire2, p2, p2len);

        rc = (int)srtcp_unprotect(&rx, p2, p2len, &outn);
        check_int("SRTCP rt: fresh packet accepted", rc == SRTP_OK, 1);

        tmp = p2len;
        rc = (int)srtcp_unprotect(&rx, wire2, tmp, &outn);
        check_int("SRTCP rt: replay of the same packet rejected",
                  rc == SRTP_ERR_REPLAY, 1);
        printf("         rc=%d (%s)\n", rc, srtp_strerror((srtp_status)rc));
    }

    /* 篡改 tag 必须 AUTH_FAIL */
    {
        uint8_t t[128];
        memcpy(t, plain, pn);
        srtcp_protect(&tx, t, pn, &outn);
        t[outn - 1] ^= 0x80;
        rc = (int)srtcp_unprotect(&rx, t, outn, &outn);
        check_int("SRTCP rt: tampered tag rejected as AUTH_FAIL",
                  rc == SRTP_ERR_AUTH, 1);
        printf("         rc=%d (%s)\n", rc, srtp_strerror((srtp_status)rc));
    }

    srtp_deinit(&tx);
    srtp_deinit(&rx);
}

/* -------------------------------------------------------- [4] 性能 */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_perf(void)
{
    srtp_master_key mk;
    srtp_ctx tx, rx;
    uint8_t buf[1600];
    uint8_t keep[1600];
    size_t n = 1500, outn = 0;
    int iters = 20000;
    int i;
    double t0, t1, dt_p, dt_u;

    printf("\n[4] performance (1500-byte packets)\n");

    memset(&mk, 0, sizeof(mk));
    unhex("E1F97A0D3E018BE0D64FA32C06DE4139", mk.master_key, 16);
    unhex("0EC675AD498AFEEBB6960B3AABE6", mk.master_salt, 14);
    mk.master_salt_len = 14;
    srtp_init(&tx, &mk, 0x55667788u);
    srtp_init(&rx, &mk, 0x55667788u);

    memset(buf, 0x5A, sizeof(buf));
    buf[0] = 0x80; buf[1] = 0x60;
    buf[8] = 0x55; buf[9] = 0x66; buf[10] = 0x77; buf[11] = 0x88;

    /* protect 吞吐 */
    t0 = now_sec();
    for (i = 0; i < iters; i++) {
        buf[2] = (uint8_t)(i >> 8); buf[3] = (uint8_t)i;
        memcpy(keep, buf, n);
        srtp_protect(&tx, keep, n, &outn);
    }
    t1 = now_sec();
    dt_p = t1 - t0;

    /* unprotect 吞吐（用上面最后生成的包反复解密会触发重放，
       所以先 reset 接收状态） */
    srtp_set_ssrc(&rx, 0x55667788u);
    tx.tx_started = 0;
    t0 = now_sec();
    for (i = 0; i < iters; i++) {
        buf[2] = (uint8_t)(i >> 8); buf[3] = (uint8_t)i;
        memcpy(keep, buf, n);
        srtp_protect(&tx, keep, n, &outn);
        srtp_unprotect(&rx, keep, outn, &outn);
    }
    t1 = now_sec();
    dt_u = t1 - t0;

    printf("    protect  : %d pkts in %.4f s -> %.1f pps  (%.2f us/pkt)\n",
           iters, dt_p, (double)iters / dt_p, dt_p / (double)iters * 1e6);
    printf("    prot+unp : %d pkts in %.4f s -> %.1f pps  (%.2f us/pkt)\n",
           iters, dt_u, (double)iters / dt_u, dt_u / (double)iters * 1e6);

    {
        double pps_prot = (double)iters / dt_p;
        double pps_both = (double)iters / dt_u;
        check_int("protect >= 1000 pps", pps_prot >= 1000.0, 1);
        check_int("protect+unprotect >= 1000 pps", pps_both >= 1000.0, 1);
        /* 60 fps 的实际负载：单包 1500B，每帧 1 包 */
        printf("    at 60 fps the CPU cost is %.3f%% of one core (protect+unprotect)\n",
               60.0 / pps_both * 100.0);
    }

    srtp_deinit(&tx);
    srtp_deinit(&rx);
}

/* --------------------------------------------------------------- main */

int main(void)
{
    printf("========================================================\n");
    printf(" SRTP/SRTCP core self-test  (RFC 3711)\n");
    printf(" profile: SRTP_AES128_CM_SHA1_80\n");
    printf(" target : aarch64 / X6 Pro (RK3562) / libcrypto 1.1.1l\n");
    printf("========================================================\n");

    test_rfc_b2();
    test_rfc_b3();
    test_golden_packets();
    test_srtcp_roundtrip();
    test_roc_rollover();
    test_replay_window();
    test_selfconsistency();
    test_perf();

    printf("\n--------------------------------------------------------\n");
    printf(" passed: %d   failed: %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf(" ALL PASS\n");
        return 0;
    }
    printf(" *** FAILED ***\n");
    return 1;
}
