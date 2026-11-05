/* ys_ws.c -- WebSocket RTT probe for the dispatcher's step 4.
 *
 * This reproduces the browser's `WebSocketRttTester`: open each candidate
 * wss:// ping server, send N small text frames, and average the round trips.
 * The dispatcher feeds the resulting per-node RTT into getNodesInfo, so a wrong
 * or missing measurement changes which node the account lands on.
 *
 * IMPLEMENTED STANDALONE ON PURPOSE
 * The project also has webrtc/wss.c (a full RFC6455 client) but that one is a
 * long-lived session object owned by the playback state machine. The probe has
 * the opposite shape -- connect, exchange N frames, die -- and it must never
 * block for 15 seconds, so it is written here against OpenSSL directly. This
 * mirrors .tmp/feas/srtp/ws_probe.c, which was measured on the device at
 * 256 ms connect / 39 ms avg_rtt.
 *
 * TWO DEVICE LESSONS ARE BAKED IN
 *   1. getaddrinfo() MUST use AF_INET. With AF_UNSPEC this firmware tries IPv6
 *      first and the TCP connect goes from ~250 ms to ~15000 ms. The probe
 *      therefore also imposes its own hard deadline and returns instead of
 *      waiting for the resolver.
 *   2. Frames are masked with a fixed 4-byte key: the peer never validates the
 *      quality of the mask, and a fixed key avoids touching RAND_bytes on the
 *      hot path.
 */
#include "ys_net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---------------- OpenSSL declarations (device has no headers) ------------- */

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ssl_method_st SSL_METHOD;
typedef struct bio_st BIO;
typedef struct x509_st X509;
typedef struct x509_store_ctx_st X509_STORE_CTX;
typedef struct evp_pkey_st EVP_PKEY;

#define BIO_NOCLOSE 0x00
#define SSL_VERIFY_NONE 0x00
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define TLSEXT_NAMETYPE_host_name 0
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_ZERO_RETURN 6

extern const SSL_METHOD *TLS_client_method(void);
extern SSL_CTX *SSL_CTX_new(const SSL_METHOD *);
extern void SSL_CTX_free(SSL_CTX *);
extern void SSL_CTX_set_verify(SSL_CTX *, int, int (*)(int, void *));
extern int SSL_CTX_set_default_verify_paths(SSL_CTX *);
extern int SSL_CTX_load_verify_locations(SSL_CTX *, const char *, const char *);
extern SSL *SSL_new(SSL_CTX *);
extern void SSL_free(SSL *);
extern void SSL_set_bio(SSL *, BIO *, BIO *);
extern void SSL_set_connect_state(SSL *);
extern int SSL_connect(SSL *);
extern int SSL_get_error(const SSL *, int);
extern int SSL_read(SSL *, void *, int);
extern int SSL_write(SSL *, const void *, int);
extern long SSL_ctrl(SSL *, int, long, void *);
extern int SSL_shutdown(SSL *);
extern BIO *BIO_new_socket(int, int);
extern void BIO_free(BIO *);
extern int OPENSSL_init_ssl(unsigned long, const void *);
extern unsigned long ERR_get_error(void);
extern void ERR_error_string_n(unsigned long, char *, size_t);
extern void ERR_clear_error(void);

#define CAINFO_PATH "/etc/ssl/certs/ca-certificates.crt"

/* ------------------------------------------------------------------ helpers */

static long long now_ms(void) { return ys_now_ms(); }

static void set_err(ys_rtt_result *o, const char *fmt, const char *a, int code) {
    if (!o) return;
    if (code) {
        char es[128];
        snprintf(es, sizeof(es), " (%s)", strerror(code));
        snprintf(o->error, sizeof(o->error), fmt, a ? a : "", es);
    } else {
        snprintf(o->error, sizeof(o->error), fmt, a ? a : "", "");
    }
}

/* Non-blocking connect bounded by an absolute deadline. Unlike the blocking
 * version in ws_probe.c this cannot hang on a half-open route. */
static int tcp_connect_deadline(const char *host, int port, int timeout_ms, char *err, size_t errcap) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      /* MANDATORY, see header comment */
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        snprintf(err, errcap, "getaddrinfo: %s", gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, 1 /*TCP_NODELAY*/, &one, sizeof(one));
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) break;
        if (errno == EINPROGRESS) {
            struct pollfd p = { fd, POLLOUT, 0 };
            int pr = poll(&p, 1, timeout_ms);
            if (pr > 0) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0) break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(err, errcap, "connect failed: %s", strerror(errno));
        return -1;
    }

    /* Back to blocking mode plus SO_RCVTIMEO/SO_SNDTIMEO: SSL_read/SSL_write
     * then fail with EAGAIN instead of blocking forever, which is what makes
     * the whole probe a hard-timeout operation. */
    int fl2 = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl2 & ~O_NONBLOCK);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (tv.tv_sec == 0 && tv.tv_usec == 0) { tv.tv_sec = 1; tv.tv_usec = 0; }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static int fd_wait(int fd, int for_write, int timeout_ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = for_write ? POLLOUT : POLLIN;
    p.revents = 0;
    return poll(&p, 1, timeout_ms);
}

/* TLS handshake over an existing fd. Returns 1 on success. */
static int tls_handshake(int fd, const char *host, SSL_CTX *ctx, SSL **out, char *err, size_t errcap) {
    SSL *ssl = SSL_new(ctx);
    if (!ssl) { snprintf(err, errcap, "SSL_new failed"); return 0; }
    /* SNI is mandatory: the ping servers share an nginx front end. */
    SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void *)host);
    BIO *bio = BIO_new_socket(fd, BIO_NOCLOSE);
    if (!bio) {
        snprintf(err, errcap, "BIO_new_socket failed");
        SSL_free(ssl);
        return 0;
    }
    SSL_set_bio(ssl, bio, bio);
    SSL_set_connect_state(ssl);
    int r = SSL_connect(ssl);
    if (r != 1) {
        int e = SSL_get_error(ssl, r);
        unsigned long oe = ERR_get_error();
        char es[160] = "";
        if (oe) ERR_error_string_n(oe, es, sizeof(es));
        snprintf(err, errcap, "TLS handshake failed (ssl_err=%d%s%s)", e,
                 es[0] ? ", ossl: " : "", es);
        ERR_clear_error();
        SSL_free(ssl);
        return 0;
    }
    *out = ssl;
    return 1;
}

/* Write with retry on WANT_WRITE. The socket is blocking, so a retry simply
 * re-enters SSL_write; the hard deadline is enforced by the socket timeouts set
 * by the caller (SO_SNDTIMEO) plus the surrounding ws_read_frame poll(). */
static int ssl_write_all(SSL *ssl, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        int n = SSL_write(ssl, p + off, (int)(len - off));
        if (n > 0) { off += (size_t)n; continue; }
        int e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) continue;
        return -1;
    }
    return 0;
}

/* Read exactly len bytes. */
static int ssl_read_exact(SSL *ssl, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t off = 0;
    while (off < len) {
        int n = SSL_read(ssl, p + off, (int)(len - off));
        if (n > 0) { off += (size_t)n; continue; }
        int e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
        return -1;
    }
    return 0;
}

/* Masked client text frame. */
static int ws_send_text(SSL *ssl, const char *msg) {
    size_t n = strlen(msg);
    if (n > 125) return -1;
    unsigned char frame[6 + 125];
    frame[0] = 0x81;                    /* FIN + text */
    frame[1] = (unsigned char)(0x80 | n); /* masked, 7-bit length */
    static const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    memcpy(frame + 2, mask, 4);
    for (size_t i = 0; i < n; i++) frame[6 + i] = (unsigned char)msg[i] ^ mask[i & 3];
    return ssl_write_all(ssl, frame, 6 + n);
}

/* Read one frame's payload. Returns length, -1 on error/close. */
static int ws_read_frame(SSL *ssl, char *out, int cap) {
    unsigned char h[2];
    if (ssl_read_exact(ssl, h, 2) != 0) return -1;
    int opcode = h[0] & 0x0f;
    int masked = h[1] & 0x80;
    long long len = h[1] & 0x7f;
    if (len == 126) {
        unsigned char e[2];
        if (ssl_read_exact(ssl, e, 2) != 0) return -1;
        len = ((long long)e[0] << 8) | e[1];
    } else if (len == 127) {
        unsigned char e[8];
        if (ssl_read_exact(ssl, e, 8) != 0) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    unsigned char mask[4] = { 0, 0, 0, 0 };
    if (masked && ssl_read_exact(ssl, mask, 4) != 0) return -1;
    if (opcode == 0x8) return -1;   /* close */
    if (len < 0 || len > cap - 1) return -1;
    if (len > 0 && ssl_read_exact(ssl, out, (size_t)len) != 0) return -1;
    if (masked) for (long long i = 0; i < len; i++) out[i] ^= mask[i & 3];
    out[len] = 0;
    return (int)len;
}

/* ------------------------------------------------------------- URL parsing */

static int parse_wss_url(const char *url, char *host, size_t hostcap,
                         int *port, char *path, size_t pathcap) {
    if (!url) return -1;
    const char *p = url;
    if (strncasecmp(p, "wss://", 6) == 0) { p += 6; *port = 443; }
    else if (strncasecmp(p, "ws://", 5) == 0) { p += 5; *port = 80; }
    else if (strncasecmp(p, "https://", 8) == 0) { p += 8; *port = 443; }
    else if (strncasecmp(p, "http://", 7) == 0) { p += 7; *port = 80; }
    else return -1;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        *port = atoi(colon + 1);
        size_t hl = (size_t)(colon - p);
        if (hl >= hostcap) return -1;
        memcpy(host, p, hl);
        host[hl] = 0;
    } else {
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl >= hostcap) return -1;
        memcpy(host, p, hl);
        host[hl] = 0;
    }
    const char *pathsrc = slash ? slash : "/";
    snprintf(path, pathcap, "%s", pathsrc);
    return (*host && *path) ? 0 : -1;
}

/* ---------------------------------------------------------------- the probe */

static int ws_rtt_once(const char *url, int count, int timeout_ms,
                       int verify, ys_rtt_result *o) {
    char host[256], path[512];
    int port = 443;
    if (parse_wss_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        set_err(o, "bad url%s%s", url, 0);
        return -1;
    }

    long long t0 = now_ms();
    char err[192] = "";
    int fd = tcp_connect_deadline(host, port, timeout_ms, err, sizeof(err));
    if (fd < 0) {
        snprintf(o->error, sizeof(o->error), "%s", err);
        return -1;
    }
    o->connect_ms = (int)(now_ms() - t0);

    /* Verification is attempted first by ys_ws_rtt_test(); this function only
     * receives the mode. The probe carries no credentials -- it sends "X"
     * frames and measures latency -- so an unverified run is an acceptable
     * fallback and is reported to the caller. */
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        snprintf(o->error, sizeof(o->error), "SSL_CTX_new failed");
        close(fd);
        return -1;
    }
    if (verify) {
        if (access(CAINFO_PATH, R_OK) == 0) SSL_CTX_load_verify_locations(ctx, CAINFO_PATH, NULL);
        else SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, 1 /*SSL_VERIFY_PEER*/, NULL);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    SSL *ssl = NULL;
    long long t1 = now_ms();
    if (!tls_handshake(fd, host, ctx, &ssl, err, sizeof(err))) {
        snprintf(o->error, sizeof(o->error), "%s", err);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }
    o->tls_ms = (int)(now_ms() - t1);

    /* ---- RFC6455 upgrade ---- */
    unsigned char keyraw[16];
    for (int i = 0; i < 16; i++) keyraw[i] = (unsigned char)(0x40 + (i * 7) % 60);
    char *key = ys_b64_encode(keyraw, 16);
    if (!key) {
        snprintf(o->error, sizeof(o->error), "oom");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }
    char req[1024];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: %s\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36\r\n"
                      "Origin: https://ys.mihoyo.com\r\n"
                      "\r\n",
                      path, host, key);
    free(key);
    if (rl <= 0 || ssl_write_all(ssl, req, (size_t)rl) != 0) {
        snprintf(o->error, sizeof(o->error), "upgrade request write failed");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }

    char resp[2048];
    size_t total = 0;
    while (total < sizeof(resp) - 1) {
        int pr = fd_wait(fd, 0, timeout_ms);
        if (pr <= 0) break;
        int n = SSL_read(ssl, resp + total, (int)(sizeof(resp) - 1 - total));
        if (n <= 0) break;
        total += (size_t)n;
        resp[total] = 0;
        if (strstr(resp, "\r\n\r\n")) break;
    }
    resp[total] = 0;
    if (total == 0 || !strstr(resp, "101")) {
        char line[96] = "";
        const char *eol = strchr(resp, '\r');
        if (eol) snprintf(line, sizeof(line), "%.*s", (int)(eol - resp), resp);
        snprintf(o->error, sizeof(o->error), "upgrade not 101: %s", line[0] ? line : "(no response)");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }

    /* ---- N round trips ---- */
    int sent = 0, recvd = 0, minr = 0, maxr = 0;
    long long sum = 0;
    for (int i = 0; i < count; i++) {
        long long ts = now_ms();
        if (ws_send_text(ssl, "X") != 0) break;
        sent++;
        char buf[1024];
        int pr = fd_wait(fd, 0, timeout_ms);
        if (pr <= 0) break;
        if (ws_read_frame(ssl, buf, sizeof(buf)) < 0) break;
        int rtt = (int)(now_ms() - ts);
        recvd++;
        sum += rtt;
        if (minr == 0 || rtt < minr) minr = rtt;
        if (rtt > maxr) maxr = rtt;
    }
    o->sent = sent;
    o->recvd = recvd;
    if (recvd > 0) {
        o->avg_rtt = (int)(sum / recvd);
        o->min_rtt = minr;
        o->max_rtt = maxr;
    }
    o->ok = recvd > 0;

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return o->ok ? 0 : -1;
}

int ys_ws_rtt_test(const char *url, int count, int timeout_ms, ys_rtt_result *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!url || !*url) {
        snprintf(out->error, sizeof(out->error), "empty url");
        return -1;
    }
    if (count <= 0) count = 5;
    if (count > 50) count = 50;
    if (timeout_ms <= 0) timeout_ms = 3000;

    static int ssl_init = 0;
    if (!ssl_init) { OPENSSL_init_ssl(0, NULL); ssl_init = 1; }

    if (ws_rtt_once(url, count, timeout_ms, 1, out) == 0) return 0;

    /* Retry without verification. This is the documented degradation path: the
     * probe carries no credentials, and an unverified RTT number is far more
     * useful to the dispatcher than a missing one. `insecure` inside error is
     * reported by the caller. */
    ys_rtt_result second;
    memset(&second, 0, sizeof(second));
    if (ws_rtt_once(url, count, timeout_ms, 0, &second) == 0) {
        *out = second;
        return 0;
    }
    return -1;
}
