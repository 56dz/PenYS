/* sig_selftest.c -- the whole signalling chain, proven on the device itself.
 *
 * Build (from the repo root):
 *   env/pyzig/ziglang/zig.exe cc -target aarch64-linux-gnu.2.17 -O2 -g0 -Wall \
 *     -I PenYS/native/csrc/webrtc \
 *     PenYS/native/csrc/webrtc/sig_selftest.c \
 *     PenYS/native/csrc/webrtc/sig.c PenYS/native/csrc/webrtc/wss.c \
 *     PenYS/native/csrc/webrtc/sdp.c PenYS/native/csrc/webrtc/dtls.c \
 *     .tmp/feas/libs/libssl.so.1.1 .tmp/feas/libs/libcrypto.so.1.1 \
 *     -o sig_selftest
 * Run with: LD_LIBRARY_PATH=/usr/lib ./sig_selftest
 *
 * It forks: the child is a throwaway TLS + RFC 6455 server standing in for the
 * real signalling endpoint, the parent drives the real sig_conn_t state machine
 * against it over 127.0.0.1. Nothing is stubbed inside the modules under test:
 * the same code path the product uses runs here, over a real socket, with a
 * real TLS handshake and real framed WebSocket traffic.
 *
 * WHAT IS VERIFIED
 *   - WSS connect + upgrade against a local TLS server;
 *   - "client hello" goes out as a SIG_TYPE_HANDSHAKE frame with the JSON the
 *     service expects;
 *   - an offer arrives, is JSON-unescaped, and reaches on_offer;
 *   - sig_answer_auto() builds an answer from cfg.fingerprint and sends it as
 *     {"type":"answer","sdp":"..."};
 *   - the answer says a=setup:active, refuses m=application, is recvonly,
 *     keeps the offer's payload types, and carries our fingerprint;
 *   - server candidates are rewritten to cfg.candidate_ip:candidate_port
 *     BEFORE on_candidate sees them;
 *   - bare empty text frames go out as heartbeats on schedule;
 *   - the state machine walks IDLE -> OPEN -> OFFERED -> ANSWERED -> CONNECTED;
 *   - sig_video_payload_type() / sig_audio_payload_type() report what the
 *     answer settled on.
 *
 * SESSION.C AUDIT POINTS (the six review items, made executable)
 *   - cfg.connect_timeout_ms bounds sig_open's blocking connect;
 *   - the last-rx age API's pre-open trap (raw uptime) and its reset at open;
 *   - stale_ms actually walks the machine to SIG_STATE_STALE;
 *   - the answer embeds the exact DTLS fingerprint (cert existed first);
 *   - candidate_port = 0 passes server candidates through untouched;
 *   - sig_close -> sig_destroy is safe, repeated, and sig_poll answers -1
 *     immediately afterwards (the session.c wait loop can neither hang nor
 *     spin on it).
 *
 * WHAT IS NOT VERIFIED
 *   The real service. A real game_server_wss_url only exists inside sdk_param,
 *   which is only obtainable through paasDispatch -- and that bills the user's
 *   play time. So this proves the protocol machine is correct, not that
 *   miHoYo's server accepts us. The fixture offer is hand-written from
 *   docs/webrtc.md, and no real ICE exchange is exercised.
 */

#include "sig.h"
#include "dtls.h"
#include "wss.h"
#include "ossl_shim.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;

static void ck(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s   [check %d]\n", what, checks);
        failures++;
    }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

/* ======================= child: fake signalling service ================= */

/* The offer, JSON-escaped by hand: every SDP CRLF is \\r\\n inside the string. */
static const char *OFFER_SDP =
    "v=0\\r\\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\\r\\n"
    "s=-\\r\\n"
    "t=0 0\\r\\n"
    "a=group:BUNDLE 0 1 2\\r\\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\\r\\n"
    "c=IN IP4 0.0.0.0\\r\\n"
    "a=ice-ufrag:SrvU\\r\\n"
    "a=ice-pwd:SrvPwdSrvPwdSrvPwdSrvPwd\\r\\n"
    "a=fingerprint:sha-256 "
    "7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:"
    "DC:B1:AA:87:63:CF:FB:2B:0F:1E:0E:2E:0A:0B:0C:0D\\r\\n"
    "a=setup:actpass\\r\\n"
    "a=mid:0\\r\\n"
    "a=sendrecv\\r\\n"
    "a=rtcp-mux\\r\\n"
    "a=rtpmap:111 opus/48000/2\\r\\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\\r\\n"
    "a=ssrc:1234567890 cname:gameaudio\\r\\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98\\r\\n"
    "c=IN IP4 0.0.0.0\\r\\n"
    "a=ice-ufrag:SrvU\\r\\n"
    "a=ice-pwd:SrvPwdSrvPwdSrvPwdSrvPwd\\r\\n"
    "a=fingerprint:sha-256 "
    "7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:"
    "DC:B1:AA:87:63:CF:FB:2B:0F:1E:0E:2E:0A:0B:0C:0D\\r\\n"
    "a=setup:actpass\\r\\n"
    "a=mid:1\\r\\n"
    "a=sendrecv\\r\\n"
    "a=rtcp-mux\\r\\n"
    "a=rtpmap:96 H264/90000\\r\\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;"
    "profile-level-id=42e01f\\r\\n"
    "a=rtpmap:97 H264/90000\\r\\n"
    "a=fmtp:97 level-asymmetry-allowed=1;packetization-mode=0;"
    "profile-level-id=42e01f\\r\\n"
    "a=rtpmap:98 H264/90000\\r\\n"
    "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;"
    "profile-level-id=640c1f\\r\\n"
    "a=ssrc:2222222222 cname:gamevideo\\r\\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\\r\\n"
    "c=IN IP4 0.0.0.0\\r\\n"
    "a=ice-ufrag:SrvU\\r\\n"
    "a=ice-pwd:SrvPwdSrvPwdSrvPwdSrvPwd\\r\\n"
    "a=fingerprint:sha-256 "
    "7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:"
    "DC:B1:AA:87:63:CF:FB:2B:0F:1E:0E:2E:0A:0B:0C:0D\\r\\n"
    "a=setup:actpass\\r\\n"
    "a=mid:2\\r\\n"
    "a=sctp-port:5000\\r\\n";

/* ---- base64 + SHA-1, only for Sec-WebSocket-Accept ---- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64(const unsigned char *in, size_t n, char *out, size_t cap)
{
    size_t i, o = 0;
    if (cap < 4 * ((n + 2) / 3) + 1)
        return -1;
    for (i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        size_t rem = n - i;
        if (rem > 1) v |= (unsigned)in[i + 1] << 8;
        if (rem > 2) v |= (unsigned)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = rem > 1 ? B64[(v >> 6) & 63] : '=';
        out[o++] = rem > 2 ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return (int)o;
}

static void ws_accept_key(const char *key, char *out, size_t cap)
{
    static const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char cat[256];
    unsigned char md[20];
    snprintf(cat, sizeof(cat), "%s%s", key, GUID);
    SHA1((const unsigned char *)cat, strlen(cat), md);
    b64(md, 20, out, cap);
}

/* ---- blocking SSL read/write ---- */

static int sreadn(SSL *ssl, void *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = SSL_read(ssl, (char *)buf + got, n - got);
        if (r <= 0)
            return got ? got : -1;
        got += r;
    }
    return got;
}

static int swriten(SSL *ssl, const void *buf, int n)
{
    int sent = 0;
    while (sent < n) {
        int r = SSL_write(ssl, (const char *)buf + sent, n - sent);
        if (r <= 0)
            return -1;
        sent += r;
    }
    return sent;
}

/* ---- frame I/O, server side ---- */

static int read_frame(SSL *ssl, int *opcode, unsigned char **payload,
                      size_t *plen)
{
    unsigned char h[2], ext[8], mask[4];
    int len;
    size_t i;
    unsigned char *buf;

    *payload = NULL;
    *plen = 0;
    if (sreadn(ssl, h, 2) != 2)
        return -1;

    *opcode = h[0] & 0x0F;
    len = h[1] & 0x7F;
    if (len == 126) {
        if (sreadn(ssl, ext, 2) != 2)
            return -1;
        len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
        if (sreadn(ssl, ext, 8) != 8)
            return -1;
        if (ext[0] || ext[1] || ext[2] || ext[3])
            return -1;
        len = (ext[4] << 24) | (ext[5] << 16) | (ext[6] << 8) | ext[7];
    }

    if ((h[1] & 0x80) != 0) {
        if (sreadn(ssl, mask, 4) != 4)
            return -1;
    } else {
        memset(mask, 0, 4);
    }

    if (len < 0 || len > 4 * 1024 * 1024)
        return -1;
    buf = (unsigned char *)malloc((size_t)len + 1);
    if (!buf)
        return -1;
    if (len > 0 && sreadn(ssl, buf, len) != len) {
        free(buf);
        return -1;
    }
    for (i = 0; i < (size_t)len; i++)
        buf[i] ^= mask[i & 3];
    buf[len] = 0;
    *payload = buf;
    *plen = (size_t)len;
    return 0;
}

static int write_frame(SSL *ssl, int opcode, const void *data, size_t len)
{
    unsigned char h[10];
    int hl;

    h[0] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        h[1] = (unsigned char)len;
        hl = 2;
    } else if (len <= 0xFFFF) {
        h[1] = 126;
        h[2] = (unsigned char)(len >> 8);
        h[3] = (unsigned char)(len & 0xFF);
        hl = 4;
    } else {
        int i;
        h[1] = 127;
        for (i = 0; i < 8; i++)
            h[2 + i] = (unsigned char)((unsigned long long)len >>
                                       (56 - 8 * i));
        hl = 10;
    }
    if (swriten(ssl, h, hl) < 0)
        return -1;
    if (len > 0 && swriten(ssl, data, (int)len) < 0)
        return -1;
    return 0;
}

/* Send one signalling frame: 8-byte big-endian header + JSON payload. */
static int send_sig_frame(SSL *ssl, int type, const char *json)
{
    size_t jl = strlen(json);
    size_t total = jl + 8;
    unsigned char *f = (unsigned char *)malloc(total);
    int rc;

    if (!f)
        return -1;
    f[0] = 0;
    f[1] = 0;
    f[2] = 0;
    f[3] = (unsigned char)type;
    f[4] = (unsigned char)(jl >> 24);
    f[5] = (unsigned char)(jl >> 16);
    f[6] = (unsigned char)(jl >> 8);
    f[7] = (unsigned char)(jl & 0xFF);
    memcpy(f + 8, json, jl);
    rc = write_frame(ssl, WSS_OP_BIN, f, total);
    free(f);
    return rc;
}

/* Undo JSON escaping so the answer's raw SDP can be searched. */
static void unescape(const unsigned char *in, size_t n, char *out, size_t cap)
{
    size_t i, o = 0;
    for (i = 0; i < n && o + 1 < cap; i++) {
        if (in[i] == '\\' && i + 1 < n) {
            i++;
            switch (in[i]) {
            case 'r': out[o++] = '\r'; break;
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            default:  out[o++] = (char)in[i]; break;
            }
        } else {
            out[o++] = (char)in[i];
        }
    }
    out[o] = 0;
}

/* Split a signalling frame into its type and its JSON body. */
static int frame_parts(const unsigned char *p, size_t n, int *type,
                       const unsigned char **body, size_t *blen)
{
    if (!p || n < 8)
        return -1;
    *type = ((int)p[0] << 24) | ((int)p[1] << 16) | ((int)p[2] << 8) | p[3];
    *body = p + 8;
    *blen = n - 8;
    return 0;
}

/* Wait until the socket has data, or the budget runs out. */
static int wait_readable(int fd, int ms)
{
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, ms);
}

static X509 *mk_cert(EVP_PKEY **out)
{
    EVP_PKEY *k = EVP_PKEY_new();
    X509 *x;
    X509_NAME *nm;

    if (!k)
        return NULL;
    {
        RSA *rsa = RSA_new();
        BIGNUM *e = BN_new();
        if (!rsa || !e) {
            EVP_PKEY_free(k);
            return NULL;
        }
        BN_set_word(e, RSA_F4);
        if (RSA_generate_key_ex(rsa, 2048, e, NULL) != 1) {
            BN_free(e);
            RSA_free(rsa);
            EVP_PKEY_free(k);
            return NULL;
        }
        BN_free(e);
        if (EVP_PKEY_assign(k, EVP_PKEY_RSA, rsa) != 1) {
            RSA_free(rsa);
            EVP_PKEY_free(k);
            return NULL;
        }
    }
    x = X509_new();
    if (!x) {
        EVP_PKEY_free(k);
        return NULL;
    }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 7);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, k);
    nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               (const unsigned char *)"127.0.0.1", -1, -1, 0);
    X509_set_issuer_name(x, nm);
    if (X509_sign(x, k, EVP_sha256()) == 0) {
        X509_free(x);
        EVP_PKEY_free(k);
        return NULL;
    }
    *out = k;
    return x;
}

/* Writes its own verdict lines into result_fd. */
static void run_child(int port_fd, int result_fd)
{
    int lfd = -1, cfd = -1, one = 1, port = 0;
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    EVP_PKEY *key = NULL;
    X509 *cert = NULL;
    char req[4096], key_hdr[128], accept_hdr[256], answer[8192];
    int n, saw_hello = 0, answer_ok = 0, answer_setup = 0, answer_recvonly = 0;
    int answer_no_app = 0, answer_fp = 0, answer_v96 = 0, answer_a111 = 0;
    int cand_tx = 0, cand_rx = 0, hb = 0;
    long long deadline;
    char verdict[512];

    signal(SIGALRM, SIG_DFL);
    alarm(30);
    verdict[0] = 0;

#define FAILWHY(why)                                                          \
    do {                                                                      \
        snprintf(verdict, sizeof(verdict), "VERDICT FAIL %s", why);            \
        goto out;                                                             \
    } while (0)

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
        FAILWHY("socket");
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        FAILWHY("bind");
    if (listen(lfd, 4) != 0)
        FAILWHY("listen");
    if (getsockname(lfd, (struct sockaddr *)&sa, &salen) != 0)
        FAILWHY("getsockname");
    port = ntohs(sa.sin_port);
    /* The parent cannot connect until we are listening, so publish the port
     * only now. This is why the port is ephemeral rather than hard-coded. */
    if (write(port_fd, &port, sizeof(port)) != (ssize_t)sizeof(port))
        FAILWHY("port_pipe");

    cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        FAILWHY("accept");
    setsockopt(cfd, IPPROTO_TCP, 1 /*TCP_NODELAY*/, &one, sizeof(one));

    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx)
        FAILWHY("ctx");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    cert = mk_cert(&key);
    if (!cert || SSL_CTX_use_certificate(ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey(ctx, key) != 1)
        FAILWHY("cert");
    ssl = SSL_new(ctx);
    if (!ssl)
        FAILWHY("ssl_new");
    /* One BIO shared by both directions: SSL_set_bio refcounts a repeated
     * argument only once, which is the only correct way to do this. */
    {
        BIO *b = BIO_new_socket(cfd, BIO_NOCLOSE);
        if (!b)
            FAILWHY("bio");
        SSL_set_bio(ssl, b, b);
    }
    SSL_set_accept_state(ssl);
    if (SSL_accept(ssl) != 1)
        FAILWHY("tls_accept");

    memset(req, 0, sizeof(req));
    n = 0;
    while (n < (int)sizeof(req) - 1) {
        int r = SSL_read(ssl, req + n, 1);
        if (r <= 0)
            break;
        n++;
        if (n >= 4 && memcmp(req + n - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    {
        char *p = strstr(req, "Sec-WebSocket-Key:");
        if (!p)
            FAILWHY("upgrade_no_key");
        p += 18;
        while (*p == ' ')
            p++;
        n = 0;
        while (p[n] && p[n] != '\r' && n < (int)sizeof(key_hdr) - 1) {
            key_hdr[n] = p[n];
            n++;
        }
        key_hdr[n] = 0;
        ws_accept_key(key_hdr, accept_hdr, sizeof(accept_hdr));
    }
    {
        char resp[512];
        n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept_hdr);
        if (swriten(ssl, resp, n) < 0)
            FAILWHY("upgrade_write");
    }

    /* ---- 1. client hello ---- */
    deadline = now_ms() + 5000;
    while (now_ms() < deadline && !saw_hello) {
        int op;
        unsigned char *pl;
        size_t ln;
        if (wait_readable(cfd, 500) <= 0)
            continue;
        if (read_frame(ssl, &op, &pl, &ln) != 0)
            break;
        if (op == WSS_OP_BIN && ln > 8) {
            int type;
            const unsigned char *body;
            size_t blen;
            if (frame_parts(pl, ln, &type, &body, &blen) == 0 && type == 3) {
                char tmp[512];
                unescape(body, blen, tmp, sizeof(tmp));
                if (strstr(tmp, "client hello") && strstr(tmp, "\"web\""))
                    saw_hello = 1;
            }
        }
        free(pl);
        break; /* exactly one frame expected here */
    }
    if (!saw_hello)
        FAILWHY("no_client_hello");

    /* ---- 2. offer ---- */
    {
        char *msg = (char *)malloc(strlen(OFFER_SDP) + 64);
        if (!msg)
            FAILWHY("offer_oom");
        snprintf(msg, strlen(OFFER_SDP) + 64,
                 "{\"sdp\":\"%s\",\"type\":\"offer\"}", OFFER_SDP);
        if (send_sig_frame(ssl, SIG_TYPE_SIGNALING, msg) != 0) {
            free(msg);
            FAILWHY("offer_send");
        }
        free(msg);
    }

    /* ---- 3. answer ---- */
    memset(answer, 0, sizeof(answer));
    deadline = now_ms() + 5000;
    while (now_ms() < deadline) {
        int op;
        unsigned char *pl;
        size_t ln;
        if (wait_readable(cfd, 500) <= 0)
            continue;
        if (read_frame(ssl, &op, &pl, &ln) != 0)
            break;
        if (op == WSS_OP_BIN && ln > 8) {
            int type;
            const unsigned char *body;
            size_t blen;
            if (frame_parts(pl, ln, &type, &body, &blen) == 0 && type == 1) {
                char tmp[8192];
                unescape(body, blen, tmp, sizeof(tmp));
                if (strstr(tmp, "\"type\":\"answer\"") ||
                    strstr(tmp, "\\\"type\\\":\\\"answer\\\"")) {
                    snprintf(answer, sizeof(answer), "%s", tmp);
                    answer_ok = 1;
                }
            }
        }
        free(pl);
        if (answer_ok)
            break;
    }
    if (!answer_ok)
        FAILWHY("no_answer");

    answer_setup = strstr(answer, "a=setup:active") != NULL;
    answer_recvonly = strstr(answer, "a=recvonly") != NULL;
    answer_no_app = strstr(answer, "m=application 0 ") != NULL;
    answer_fp = strstr(answer, "a=fingerprint:sha-256 ") != NULL;
    answer_v96 = strstr(answer, "a=rtpmap:96 H264/90000") != NULL;
    answer_a111 = strstr(answer, "a=rtpmap:111 opus/48000/2") != NULL;

    /* Echo the answer straight to stdout: it is for a human reading the device
     * log, and mixing a 1.5 KB blob into the pipe meant to carry a 21-byte
     * verdict is how the verdict kept getting lost. */
    printf("ANSWER-BEGIN\n%s\nANSWER-END\n", answer);
    fflush(stdout);

    if (!answer_setup || !answer_recvonly || !answer_no_app || !answer_fp ||
        !answer_v96 || !answer_a111)
        FAILWHY("answer_content");

    /* ---- 4. two server candidates ---- */
    {
        static const char *c1 =
            "{\"candidate\":\"candidate:842163049 1 udp 1677729535 "
            "157.255.56.121 50122 typ srflx raddr 10.0.0.5 rport 50122 "
            "generation 0 ufrag SrvU network-cost 999\",\"sdp-mid\":\"1\","
            "\"sdp-mline-index\":1,\"type\":\"candidates\"}";
        static const char *c2 =
            "{\"candidate\":\"candidate:842163050 1 udp 1677729535 "
            "10.0.0.5 5000 typ host generation 0 ufrag SrvU\","
            "\"sdp-mid\":\"1\",\"sdp-mline-index\":1,\"type\":\"candidates\"}";
        if (send_sig_frame(ssl, SIG_TYPE_SIGNALING, c1) != 0)
            FAILWHY("cand1_send");
        if (send_sig_frame(ssl, SIG_TYPE_SIGNALING, c2) != 0)
            FAILWHY("cand2_send");
        cand_tx = 2;
    }

    /* ---- 5. watch what the client sends back for a while ----
     * 3.5 s rather than 2 s: the parent's stale-wait (stale_ms = 1500 after
     * the candidates) must finish while this socket is still alive, or the
     * parent would see a closed socket instead of SIG_STATE_STALE. The parent
     * closes first in normal operation, which breaks this loop early. */
    deadline = now_ms() + 3500;
    while (now_ms() < deadline) {
        int op;
        unsigned char *pl;
        size_t ln;
        if (wait_readable(cfd, 200) <= 0)
            continue;
        if (read_frame(ssl, &op, &pl, &ln) != 0) {
            free(pl);
            break;
        }
        if (op == WSS_OP_TEXT && ln == 0) {
            hb++;                       /* a bare empty text frame */
        } else if (op == WSS_OP_BIN && ln > 8) {
            int type;
            const unsigned char *body;
            size_t blen;
            if (frame_parts(pl, ln, &type, &body, &blen) == 0 && type == 1) {
                char tmp[1024];
                unescape(body, blen, tmp, sizeof(tmp));
                if (strstr(tmp, "\"candidates\"") &&
                    strstr(tmp, "candidate:"))
                    cand_rx++;
            }
        } else if (op == WSS_OP_CLOSE) {
            free(pl);
            break;
        }
        free(pl);
    }

    snprintf(verdict, sizeof(verdict),
             "VERDICT PASS hello=%d answer_ok=%d cand_tx=%d cand_rx=%d hb=%d",
             saw_hello, answer_ok, cand_tx, cand_rx, hb);

out:
    if (verdict[0] == 0)
        snprintf(verdict, sizeof(verdict), "VERDICT FAIL unknown");
    {
        /* Belt and braces: report to stdout too, so a lost pipe still leaves
         * a trace in the device log. */
        ssize_t w = write(result_fd, verdict, strlen(verdict));
        fprintf(stderr, "  [child] verdict=(%s) pipe_bytes=%d\n", verdict,
                (int)w);
        fflush(stderr);
    }
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (ctx)
        SSL_CTX_free(ctx);
    if (cert)
        X509_free(cert);
    if (key)
        EVP_PKEY_free(key);
    if (cfd >= 0)
        close(cfd);
    if (lfd >= 0)
        close(lfd);
#undef FAILWHY
}

/* ============================== parent: the client ====================== */

typedef struct {
    int offer_seen;
    int answers_sent;
    int cand_sent;              /* did we trickle one back to the service? */
    int cand_seen;
    char cand_first[512];       /* the first remote candidate: this is the
                                 * srflx one with the long attribute tail, so
                                 * it is the one that proves the rewrite kept
                                 * everything after cs[5] intact */
    char cand_line[512];        /* most recent */
    int states[16];
    int n_states;
    int entered_offered, entered_answered, entered_connected;
} client_ctx_t;

static void note_state(client_ctx_t *c, int st)
{
    if (c->n_states < (int)(sizeof(c->states) / sizeof(c->states[0])))
        c->states[c->n_states++] = st;
    if (st == SIG_STATE_OFFERED) c->entered_offered = 1;
    if (st == SIG_STATE_ANSWERED) c->entered_answered = 1;
    if (st == SIG_STATE_CONNECTED) c->entered_connected = 1;
}

static void on_offer(sig_conn_t *s, const char *sdp, void *user)
{
    client_ctx_t *c = (client_ctx_t *)user;
    (void)s;
    c->offer_seen = 1;
    printf("  [cb] on_offer: %d bytes, starts \"%.20s...\"\n", (int)strlen(sdp),
           sdp);
}

static void on_candidate(sig_conn_t *s, const char *line, void *user)
{
    client_ctx_t *c = (client_ctx_t *)user;
    (void)s;
    c->cand_seen++;
    if (c->cand_seen == 1)
        snprintf(c->cand_first, sizeof(c->cand_first), "%s", line);
    snprintf(c->cand_line, sizeof(c->cand_line), "%s", line);
    printf("  [cb] on_candidate: %s\n", line);
}

static void on_state(sig_conn_t *s, int old_st, int new_st, void *user)
{
    client_ctx_t *c = (client_ctx_t *)user;
    (void)s;
    printf("  [cb] state %s -> %s\n", sig_state_name(old_st),
           sig_state_name(new_st));
    note_state(c, new_st);
}

static int run_parent(int port_fd, int result_fd)
{
    client_ctx_t cc;
    sig_cfg_t cfg;
    sig_conn_t *s;
    dtls_client_t *d;
    const char *fp;
    char url[128], verdict[32768];
    int port = 0, i, connected_ms = -1;
    long long deadline;
    ssize_t rn;

    memset(&cc, 0, sizeof(cc));
    verdict[0] = 0;

    if (read(port_fd, &port, sizeof(port)) != (ssize_t)sizeof(port)) {
        printf("  FAIL could not read the child's port\n");
        return 1;
    }
    printf("  child service listening on 127.0.0.1:%d\n", port);

    d = dtls_client_create(DTLS_KEY_ECDSA_P256);
    if (!d) {
        printf("  FAIL dtls_client_create\n");
        return 1;
    }
    fp = dtls_local_fingerprint(d);
    printf("  our fingerprint: %s\n", fp);

    snprintf(url, sizeof(url), "wss://127.0.0.1:%d/rtc-sdk", port);
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.fingerprint = fp;
    cfg.candidate_ip = "157.255.56.100";
    cfg.candidate_port = 44403;
    cfg.local_ip = "127.0.0.1";
    cfg.codecs = 0;               /* accept what is offered */
    cfg.sendrecv = 0;
    cfg.accept_datachannel = 0;
    cfg.connect_timeout_ms = 8000; /* session.c passes YS_WSS_TIMEOUT_MS here */
    cfg.heartbeat_ms = 300;       /* short, so heartbeats are observable */
    cfg.stale_ms = 1500;          /* short, so SIG_STATE_STALE is observable */
    cfg.cbs.on_offer = on_offer;
    cfg.cbs.on_candidate = on_candidate;
    cfg.cbs.on_state = on_state;
    cfg.user = &cc;

    s = sig_create(&cfg);
    if (!s) {
        printf("  FAIL sig_create\n");
        dtls_client_destroy(d);
        return 1;
    }
    note_state(&cc, SIG_STATE_IDLE);

    /* --- audit point (6): what does the age API say before sig_open()? ---
     * sig.c only stamps t_last_rx_ms inside sig_open(), so a created-but-not-
     * opened handle reports now - 0 = raw CLOCK_MONOTONIC uptime. If
     * session.c ever ran its stale check before PLAYING this would read as an
     * instant stale (age > YS_STALE_MS = 30000). session.c gates the check on
     * (state == YS_ST_PLAYING && W.sig) and PLAYING is unreachable without a
     * successful sig_open, so the trap exists here and must stay unreachable
     * there. */
    {
        long long u0 = now_ms();
        int pre = sig_last_rx_age_ms(s);
        long long u1 = now_ms();
        printf("  pre-open last-rx age = %d ms (raw monotonic clock)\n", pre);
        ck(sig_last_rx_age_ms(NULL) == -1,
           "sig_last_rx_age_ms(NULL) == -1");
        ck((long long)pre >= u0 && (long long)pre <= u1,
           "pre-open age is raw uptime: session must not read it before PLAYING");
    }

    {
        long long t0 = now_ms();
        int rc = sig_open(s);
        long long dt = now_ms() - t0;
        printf("  sig_open rc=%d in %lld ms, state=%s\n", rc, dt,
               sig_state_str(s));
        ck(rc == 0, "sig_open connected and sent client hello");
        /* audit point (2): the worker-thread block must fit the budget. */
        ck(dt >= 0 && dt < 8000,
           "sig_open blocked < 8000 ms (YS_WSS_TIMEOUT_MS budget)");
        /* audit point (6): open stamps t_last_rx_ms = t_open_ms. */
        ck(sig_last_rx_age_ms(s) >= 0 && sig_last_rx_age_ms(s) < 1000,
           "sig_open resets last-rx age (stale timer starts at open)");
    }
    if (sig_state(s) < SIG_STATE_OPEN) {
        printf("  FAIL open failed: %s\n", sig_last_error(s));
        sig_destroy(s);
        dtls_client_destroy(d);
        return 1;
    }

    /* The main loop a real client runs: a short poll so the RTP path can also
     * be serviced, with sig_poll clamping its own timeout to the heartbeat.
     * The heartbeat period is 300 ms here (not the production 10 s) so that a
     * couple of them fit inside a test run; the exit condition therefore waits
     * for three of them rather than quitting the moment CONNECTED is reached,
     * which is what made the earlier run report heartbeats=0. */
    {
        long long t_connected = 0;
        deadline = now_ms() + 6000;
        while (now_ms() < deadline) {
            int rc = sig_poll(s, 10);
            if (rc < 0 && sig_state(s) >= SIG_STATE_CLOSED) {
                printf("  poll ended: %s\n", sig_last_error(s));
                break;
            }
            if (cc.offer_seen && cc.answers_sent == 0) {
                int an = sig_answer_auto(s);
                printf("  sig_answer_auto rc=%d\n", an);
                if (an == 0)
                    cc.answers_sent = 1;
            }
            /* Once the remote candidate arrives, trickle ours back the way a
             * real client would, so the service can see the up-path too. */
            if (cc.entered_connected && !cc.cand_sent) {
                const char *mine =
                    "candidate:1 1 udp 2130706431 10.1.2.3 51234 typ host";
                if (sig_send_candidate(s, mine) == 0) {
                    cc.cand_sent = 1;
                    printf("  trickled our candidate\n");
                }
            }
            if (cc.entered_connected && !t_connected)
                t_connected = now_ms();
            if (t_connected && now_ms() - t_connected > 1200 &&
                sig_heartbeats_sent(s) >= 3)
                break;
        }
        (void)connected_ms;
    }

    /* --- audit point (1)/(6): stale_ms semantics ---
     * The mock goes silent after its two candidates; with stale_ms = 1500 the
     * machine must walk itself to SIG_STATE_STALE with no help, and the age
     * API must then agree the silence is past the deadline. session.c uses
     * the same mechanism with YS_STALE_MS = 30000 (session.c:955), gated on
     * PLAYING (session.c:1129-1131). */
    {
        long long dl = now_ms() + 4 * 1500;
        while (sig_state(s) != SIG_STATE_STALE && now_ms() < dl) {
            if (sig_poll(s, 50) < 0)
                break;
        }
        printf("  state after silence: %s, last rx age = %d ms\n",
               sig_state_str(s), sig_last_rx_age_ms(s));
        ck(sig_state(s) == SIG_STATE_STALE,
           "stale fires after stale_ms of server silence");
        ck(sig_last_rx_age_ms(s) > 1500,
           "age exceeds stale_ms once the state is STALE");
    }

    /* --- audit point (4): the DTLS cert must predate the answer ---
     * session.c creates W.dtls (session.c:903) before sig_create
     * (session.c:960) and only then lets sig_answer_auto (session.c:629)
     * run; here `d` likewise predates sig_create, and we prove it by
     * rebuilding the answer and finding the exact same fingerprint inside
     * it -- with the "sha-256 " prefix sig.h requires. */
    {
        char abuf[8192];
        int an = sig_build_answer(s, abuf, sizeof(abuf));
        printf("  sig_build_answer -> %d bytes\n", an);
        ck(an > 0, "sig_build_answer rebuilt the answer after the exchange");
        ck(strstr(abuf, "a=fingerprint:sha-256 ") != NULL,
           "answer carries a=fingerprint:sha-256 (sig.h contract)");
        ck(fp != NULL && strstr(abuf, fp) != NULL,
           "answer embeds the exact DTLS fingerprint (cert existed first)");
    }

    printf("\n  --- what the machine reported ---\n");
    ck(cc.offer_seen, "on_offer fired");
    ck(cc.answers_sent == 1, "sig_answer_auto() sent exactly one answer");
    ck(cc.entered_offered, "state reached OFFERED");
    ck(cc.entered_answered, "state reached ANSWERED");
    ck(cc.entered_connected, "state reached CONNECTED");
    ck(cc.cand_seen == 2, "both server candidates arrived");

    /* Q2 in one line: what does on_candidate actually hand the caller? */
    printf("  first candidate line as delivered:\n    %s\n", cc.cand_first);
    ck(strstr(cc.cand_first, "157.255.56.100 44403") != NULL,
       "candidate was rewritten to cfg.candidate_ip:candidate_port");
    ck(strstr(cc.cand_first, "net-region") == NULL, "(sanity) no bogus field");
    ck(strstr(cc.cand_first, "typ srflx") != NULL &&
           strstr(cc.cand_first, "raddr 10.0.0.5 rport 50122") != NULL &&
           strstr(cc.cand_first, "network-cost 999") != NULL,
       "trailing candidate attributes survived the rewrite");
    ck(strstr(cc.cand_first, "157.255.56.121") == NULL,
       "the server's original address is gone");

    /* Q1 in one line: the payload types the answer settled on. */
    printf("  sig_video_payload_type = %d\n", sig_video_payload_type(s));
    printf("  sig_audio_payload_type = %d\n", sig_audio_payload_type(s));
    ck(sig_video_payload_type(s) == 96,
       "video payload type is 96 (H.264 Baseline from the offer)");
    ck(sig_audio_payload_type(s) == 111, "audio payload type is 111 (opus)");

    printf("  heartbeats sent = %d\n", sig_heartbeats_sent(s));
    ck(sig_heartbeats_sent(s) >= 3,
       "heartbeats fired on the 300 ms period while looping");
    printf("  frames received = %d, last rx age = %d ms\n", sig_frames_rx(s),
           sig_last_rx_age_ms(s));

    /* sig_next_timeout must never strand the caller. */
    {
        int t = sig_next_timeout(s);
        printf("  sig_next_timeout = %d\n", t);
        ck(t >= 0 && t <= 1000, "sig_next_timeout is a usable poll timeout");
    }

    /* --- audit points (3) + (5): teardown contract ---
     * session.c's w_wait does `if (sig_poll(W.sig, ...) < 0) return -1;`
     * (session.c:422) -- it must get an immediate -1 after close, never a
     * 5000 ms nap and never a silent success that keeps the wait loop alive.
     * And sig_close must be safe to repeat before sig_destroy frees the
     * handle: w_teardown is idempotent and runs on three paths (stop, error
     * unwind, worker exit; session.c:841-846). */
    ck(sig_close(s) == 0, "sig_close on the live connection");
    {
        long long t0 = now_ms();
        int rc = sig_poll(s, 5000);
        long long dt = now_ms() - t0;
        printf("  sig_poll after close -> %d in %lld ms\n", rc, dt);
        ck(rc == -1 && dt < 500,
           "sig_poll returns -1 immediately after close (no wait-loop hang)");
    }
    ck(sig_close(s) == 0, "sig_close twice is safe (teardown is idempotent)");
    sig_destroy(s);
    ck(1, "sig_destroy after close did not crash");
    dtls_client_destroy(d);

    /* Collect the child's verdict. A single read() is not enough: the answer
     * echo plus the verdict easily exceeds one pipe buffer's worth of early
     * chunks, and the earlier version stopped after the first chunk and then
     * judged itself on a truncated string. Read until EOF. */
    (void)waitpid(-1, NULL, 0);
    {
        size_t got = 0;
        while (got < sizeof(verdict) - 1) {
            ssize_t r = read(result_fd, verdict + got,
                             sizeof(verdict) - 1 - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        verdict[got] = 0;
        rn = (ssize_t)got;
    }
    if (rn > 0) {
        char *vline;
        printf("\n  --- service verdict ---\n");
        {
            char *p = verdict;
            while (*p) {
                char *nl = strchr(p, '\n');
                printf("  %.*s\n", nl ? (int)(nl - p) : (int)strlen(p), p);
                if (!nl)
                    break;
                p = nl + 1;
            }
        }
        /* The pipe also carries the ANSWER-BEGIN/END echo, so the verdict is
         * not at offset 0 -- locate the line itself. */
        vline = strstr(verdict, "VERDICT ");
        ck(vline != NULL && strncmp(vline, "VERDICT PASS", 12) == 0,
           "the service accepted the whole exchange");
        ck(vline && strstr(vline, "hello=1") != NULL,
           "service saw client hello");
        ck(vline && strstr(vline, "answer_ok=1") != NULL,
           "service saw the answer");
        ck(vline && (strstr(vline, "cand_rx=1") != NULL ||
                     strstr(vline, "cand_rx=2") != NULL),
           "service received our trickled candidate");
        {
            /* Parse hb=N rather than substring-matching, so hb=10 is not
             * mistaken for a failure of the hb=0 check. */
            const char *hb = vline ? strstr(vline, "hb=") : NULL;
            int hbn = hb ? atoi(hb + 3) : 0;
            printf("  service counted %d heartbeat(s)\n", hbn);
            ck(hbn > 0, "service received bare-empty-frame heartbeats");
        }
    } else {
        printf("  FAIL no verdict from the child\n");
        failures++;
        checks++;
    }

    (void)connected_ms;
    (void)i;
    return 0;
}

/* ============ second session: candidate_port = 0 (audit point 1) ==========
 *
 * sig.h: "Set candidate_port to 0 to disable rewriting (candidates pass
 * through)." session.c relies on exactly that when game_svr_addr carries no
 * port (session.c:940-942 clears cand_ip => candidate_ip NULL, cand_port
 * stays 0 at session.c:917/948), and sig.c:715 only rewrites when
 * candidate_ip && candidate_port > 0. Same mock service, same fixture
 * candidates -- only the config differs, so any difference in what
 * on_candidate delivers is the gate's doing. */
static int run_parent_port0(int port_fd, int result_fd)
{
    client_ctx_t cc;
    sig_cfg_t cfg;
    sig_conn_t *s;
    dtls_client_t *d;
    char url[128], verdict[4096];
    int port = 0;
    long long deadline;

    memset(&cc, 0, sizeof(cc));
    verdict[0] = 0;

    if (read(port_fd, &port, sizeof(port)) != (ssize_t)sizeof(port)) {
        printf("  FAIL could not read child2's port\n");
        return 1;
    }
    printf("\n  ===== second session: candidate_port=0 =====\n");
    printf("  child2 service listening on 127.0.0.1:%d\n", port);

    d = dtls_client_create(DTLS_KEY_ECDSA_P256);
    if (!d) {
        printf("  FAIL dtls_client_create (child2)\n");
        return 1;
    }

    snprintf(url, sizeof(url), "wss://127.0.0.1:%d/rtc-sdk", port);
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.fingerprint = dtls_local_fingerprint(d);
    cfg.candidate_ip = NULL;      /* the gate under test */
    cfg.candidate_port = 0;       /* sig.h: 0 disables rewriting */
    cfg.local_ip = "127.0.0.1";
    cfg.connect_timeout_ms = 8000;
    cfg.heartbeat_ms = 300;
    cfg.stale_ms = 30000;         /* keep stale out of this session */
    cfg.cbs.on_offer = on_offer;
    cfg.cbs.on_candidate = on_candidate;
    cfg.cbs.on_state = on_state;
    cfg.user = &cc;

    s = sig_create(&cfg);
    if (!s) {
        printf("  FAIL sig_create (child2)\n");
        dtls_client_destroy(d);
        return 1;
    }
    note_state(&cc, SIG_STATE_IDLE);

    {
        long long t0 = now_ms();
        int rc = sig_open(s);
        printf("  sig_open rc=%d in %lld ms\n", rc, now_ms() - t0);
        ck(rc == 0, "port0: sig_open connected");
    }
    if (sig_state(s) < SIG_STATE_OPEN) {
        printf("  FAIL open failed: %s\n", sig_last_error(s));
        sig_destroy(s);
        dtls_client_destroy(d);
        return 1;
    }

    /* Offer -> answer -> candidates -> trickle, the same shape as session.c's
     * w_run_session, bounded so a broken gate fails instead of hanging. */
    deadline = now_ms() + 6000;
    while (now_ms() < deadline && !cc.entered_connected) {
        if (sig_poll(s, 20) < 0 && sig_state(s) >= SIG_STATE_CLOSED)
            break;
        if (cc.offer_seen && cc.answers_sent == 0) {
            if (sig_answer_auto(s) == 0)
                cc.answers_sent = 1;
        }
        if (cc.entered_connected && !cc.cand_sent) {
            if (sig_send_candidate(s,
                    "candidate:1 1 udp 2130706431 10.1.2.3 51234 typ host") == 0)
                cc.cand_sent = 1;
        }
    }

    printf("  port0: offer=%d answers=%d candidates=%d\n",
           cc.offer_seen, cc.answers_sent, cc.cand_seen);
    printf("  first candidate as delivered:\n    %s\n", cc.cand_first);
    ck(cc.offer_seen, "port0: offer arrived");
    ck(cc.answers_sent == 1, "port0: answer sent");
    ck(cc.cand_seen == 2, "port0: both server candidates arrived");
    ck(strstr(cc.cand_first, "157.255.56.121 50122") != NULL,
       "candidate_port=0: original ip:port preserved (rewrite disabled)");
    ck(strstr(cc.cand_first, "157.255.56.100") == NULL,
       "candidate_port=0: the rewrite target is absent");
    ck(strstr(cc.cand_first, "network-cost 999") != NULL,
       "candidate_port=0: trailing attributes still intact");
    ck(strstr(cc.cand_line, "10.0.0.5 5000") != NULL,
       "candidate_port=0: second candidate also untouched");

    sig_close(s);
    sig_destroy(s);
    dtls_client_destroy(d);

    (void)waitpid(-1, NULL, 0);
    {
        size_t got = 0;
        while (got < sizeof(verdict) - 1) {
            ssize_t r = read(result_fd, verdict + got,
                             sizeof(verdict) - 1 - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        verdict[got] = 0;
    }
    ck(strstr(verdict, "VERDICT PASS") != NULL,
       "port0: the service accepted the whole exchange");
    return 0;
}

/* ====== third probe: connect_timeout_ms is a real budget (audit point 2) ==
 *
 * An unreachable endpoint with a 1500 ms budget must fail fast and bounded.
 * session.c passes YS_WSS_TIMEOUT_MS (8000, session.c:953) and the worker
 * thread sits inside sig_open for the whole attempt (session.c:965 runs on
 * the worker; stop requests are only observed after w_run_session returns),
 * so an ignored budget would wedge the worker. The bound asserted here is
 * 8000 ms: three wss phases of 1500 ms each stay comfortably inside it. */
static void run_parent_budget(void)
{
    sig_cfg_t cfg;
    sig_conn_t *s;
    long long t0, dt;
    int rc;

    printf("\n  ===== connect budget probe (unreachable endpoint) =====\n");
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = "wss://10.255.255.1:9/rtc-sdk";
    cfg.connect_timeout_ms = 1500;

    s = sig_create(&cfg);
    ck(s != NULL, "budget: sig_create for the unreachable probe");
    if (!s)
        return;
    ck(sig_next_timeout(s) == -1, "budget: next_timeout == -1 before open");
    ck(sig_last_rx_age_ms(s) != -1, "budget: age is not -1 on a live handle");

    t0 = now_ms();
    rc = sig_open(s);
    dt = now_ms() - t0;
    printf("  sig_open(unreachable) rc=%d in %lld ms (budget 1500 ms)\n",
           rc, dt);
    ck(rc == -1, "budget: sig_open fails against an unreachable endpoint");
    ck(dt >= 0 && dt < 8000,
       "budget: bounded well under YS_WSS_TIMEOUT_MS (8000 ms)");
    ck(sig_state(s) == SIG_STATE_FAILED,
       "budget: state FAILED after a failed open");
    ck(sig_poll(s, 0) == -1, "budget: sig_poll after failed open is -1");
    ck(sig_close(s) == 0, "budget: sig_close after failed open is safe");
    sig_destroy(s);
    ck(1, "budget: destroy after failed open did not crash");
}

int main(void)
{
    int port_pipe[2], result_pipe[2];
    pid_t pid;

    printf("===== sig.c end-to-end self-test (in-process TLS service) =====\n");

    if (pipe(port_pipe) != 0 || pipe(result_pipe) != 0) {
        perror("pipe");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        close(port_pipe[0]);
        close(result_pipe[0]);
        run_child(port_pipe[1], result_pipe[1]);
        _exit(0);
    }

    close(port_pipe[1]);
    close(result_pipe[1]);
    run_parent(port_pipe[0], result_pipe[0]);
    close(port_pipe[0]);
    close(result_pipe[0]);

    /* Second session: same mock service, candidate_port=0 (audit point 1). */
    if (pipe(port_pipe) != 0 || pipe(result_pipe) != 0) {
        perror("pipe2");
        return 1;
    }
    pid = fork();
    if (pid < 0) {
        perror("fork2");
        return 1;
    }
    if (pid == 0) {
        close(port_pipe[0]);
        close(result_pipe[0]);
        run_child(port_pipe[1], result_pipe[1]);
        _exit(0);
    }
    close(port_pipe[1]);
    close(result_pipe[1]);
    run_parent_port0(port_pipe[0], result_pipe[0]);
    close(port_pipe[0]);
    close(result_pipe[0]);

    /* Third probe: the connect budget against an unreachable endpoint. */
    run_parent_budget();

    printf("\n===== %s (%d checks, %d failures) =====\n",
           failures ? "FAILED" : "ALL PASS", checks, failures);
    return failures ? 1 : 0;
}
