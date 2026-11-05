/* wss.c -- RFC6455 WebSocket client over TLS for the X6 Pro.
 *
 * Layering: raw fd -> SSL (BIO_new_socket) -> frame codec -> public API.
 *
 * Memory discipline: the socket read buffer, the message reassembly buffer and
 * the TLS objects are allocated once in wss_connect(). The only per-call
 * allocation is a transient mask scratch (malloc/free) whose size is the
 * payload length, because the caller's buffer must not be mutated. Nothing
 * grows without bound while the session runs.
 */
#include "wss.h"
#include "ossl_shim.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define WSS_RBUF          (64 * 1024)
#define WSS_MSGBUF_CAP    (4 * 1024 * 1024)
#define WSS_MAX_PAYLOAD   (16 * 1024 * 1024)
#define WSS_MAX_HDR       14

struct wss {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;

    unsigned char rbuf[WSS_RBUF];
    size_t rlen;

    unsigned char *msg;          /* reassembly buffer */
    size_t msg_len;
    size_t msg_cap;
    int msg_opcode;              /* 0 == no message in progress */

    int closed;
    int failed;
    char err[256];

    int tcp_ms, tls_ms, upgrade_ms, total_ms;
    char subproto[128];
};

static char g_last_error[256] = "no error";

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void set_err(wss_t *h, const char *fmt, ...)
{
    va_list ap;
    if (h && h->failed) return;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
    if (h) {
        snprintf(h->err, sizeof(h->err), "%s", g_last_error);
        h->failed = 1;
    }
}

/* ------------------------------------------------------------ base64/CI */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void ws_base64(const unsigned char *in, size_t n, char *out)
{
    size_t i = 0, o = 0;
    while (i + 2 < n) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
        i += 3;
    }
    if (i < n) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
}

/* Locating a header is easier case-insensitively; the device has no
 * strcasestr prototype guaranteed by any strict header mode, so keep ours. */
static const char *find_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            if (!hay[i]) return NULL;      /* ran off the end mid-compare */
        }
        if (i == nl) return hay;
    }
    return NULL;
}

/* ------------------------------------------------------------------ TCP */

/* AF_INET is mandatory. Measured on this firmware: with AF_UNSPEC the resolver
 * returns an IPv6 address first and connect() burns ~15 s on it before falling
 * back, versus ~120 ms with AF_INET. Also non-blocking connect + poll so the
 * whole prologue respects the caller's budget. */
static int tcp_connect(const char *host, int port, int timeout_ms, wss_t *h)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(portstr, sizeof(portstr), "%d", port);

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        set_err(h, "getaddrinfo(%s): %s", host, gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        int fl, soerr = 0;
        socklen_t sl = sizeof(soerr);

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            break;
        }
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            rc = poll(&p, 1, timeout_ms);
            if (rc > 0) {
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0) {
                    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
                    break;
                }
                set_err(h, "connect(%s:%d): %s", host, port, strerror(soerr));
            } else if (rc == 0) {
                set_err(h, "connect(%s:%d): timeout after %d ms", host, port,
                        timeout_ms);
            } else {
                set_err(h, "connect: poll: %s", strerror(errno));
            }
        } else {
            set_err(h, "connect(%s:%d): %s", host, port, strerror(errno));
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && !h->failed)
        set_err(h, "connect(%s:%d): no usable address", host, port);
    return fd;
}

/* ------------------------------------------------------------------ TLS */

static int ssl_write_all(wss_t *h, const unsigned char *p, size_t n);

static int tls_handshake(wss_t *h, const char *host, int timeout_ms)
{
    int r;
    long t0 = now_ms();

    h->ctx = SSL_CTX_new(TLS_client_method());
    if (!h->ctx) { set_err(h, "SSL_CTX_new failed"); return -1; }
    /* The cloud-gaming endpoint uses a certificate we cannot pin or verify on
     * the device (no CA store), so verification is off by design. */
    SSL_CTX_set_verify(h->ctx, SSL_VERIFY_NONE, NULL);

    h->ssl = SSL_new(h->ctx);
    if (!h->ssl) { set_err(h, "SSL_new failed"); return -1; }

    /* SNI, via SSL_CTRL_SET_TLSEXT_HOSTNAME (55). */
    SSL_ctrl(h->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, 0, (void *)host);
    {
        /* One BIO shared by both directions: SSL_set_bio bumps the refcount
         * once when rbio == wbio, so a single object is the correct form. */
        BIO *bio = BIO_new_socket(h->fd, BIO_NOCLOSE);
        if (!bio) { set_err(h, "BIO_new_socket failed"); return -1; }
        SSL_set_bio(h->ssl, bio, bio);
    }
    SSL_set_connect_state(h->ssl);

    /* The socket is non-blocking, so drive the handshake with poll(). */
    for (;;) {
        long left;
        struct pollfd p;

        r = SSL_connect(h->ssl);
        if (r == 1) return 0;

        {
            int e = SSL_get_error(h->ssl, r);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                unsigned long oe;
                char b[200];
                set_err(h, "SSL_connect rc=%d ssl_err=%d", r, e);
                while ((oe = ERR_get_error()) != 0) {
                    ERR_error_string_n(oe, b, sizeof(b));
                    fprintf(stderr, "[wss] tls: %s\n", b);
                }
                return -1;
            }
            left = timeout_ms - (now_ms() - t0);
            if (left <= 0) { set_err(h, "TLS handshake timeout"); return -1; }
            p.fd = h->fd;
            p.events = (e == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            p.revents = 0;
            if (poll(&p, 1, (int)left) < 0) {
                set_err(h, "poll(tls): %s", strerror(errno));
                return -1;
            }
        }
    }
}

/* -------------------------------------------------------------- upgrade */

static int ws_upgrade(wss_t *h, const char *host, const char *path,
                      const char *subprotos, int timeout_ms)
{
    unsigned char rnd[16];
    char key[32], req[1024], resp[4096];
    int n, total = 0;
    long t0 = now_ms();
    size_t extra;

    RAND_bytes(rnd, (int)sizeof(rnd));
    ws_base64(rnd, sizeof(rnd), key);

    if (subprotos && *subprotos)
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Protocol: %s\r\n"
                     "\r\n",
                     path, host, key, subprotos);
    else
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     path, host, key);
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        set_err(h, "upgrade request too long");
        return -1;
    }
    if (ssl_write_all(h, (const unsigned char *)req, (size_t)n) != 0) {
        set_err(h, "SSL_write(upgrade) failed");
        return -1;
    }

    for (;;) {
        int e;
        long spent;
        int left;

        if (total >= (int)sizeof(resp) - 1) {
            set_err(h, "upgrade response exceeds %u bytes", (unsigned)sizeof(resp));
            return -1;
        }
        n = SSL_read(h->ssl, resp + total, (int)sizeof(resp) - 1 - total);
        if (n > 0) {
            total += n;
            resp[total] = 0;
            if (strstr(resp, "\r\n\r\n")) break;
            continue;
        }
        e = SSL_get_error(h->ssl, n);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            set_err(h, "SSL_read(upgrade) eof/err e=%d", e);
            return -1;
        }
        spent = now_ms() - t0;
        left = timeout_ms - (int)spent;
        if (left <= 0) { set_err(h, "upgrade timeout after %ld ms", spent); return -1; }
        {
            struct pollfd p;
            p.fd = h->fd;
            p.events = (e == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            p.revents = 0;
            if (poll(&p, 1, left) <= 0) {
                set_err(h, "upgrade timeout after %d ms", timeout_ms);
                return -1;
            }
        }
    }

    if (!find_ci(resp, "101 switching protocols")) {
        char *eol = strchr(resp, '\r');
        char saved = 0;
        if (eol) { saved = *eol; *eol = 0; }
        set_err(h, "upgrade rejected: %s", resp);
        if (eol) *eol = saved;
        return -1;
    }

    {
        const char *sp = find_ci(resp, "\r\nsec-websocket-protocol:");
        if (sp) {
            const char *end;
            size_t l;
            sp += strlen("\r\nsec-websocket-protocol:");
            while (*sp == ' ' || *sp == '\t') sp++;
            end = strstr(sp, "\r\n");
            l = end ? (size_t)(end - sp) : strlen(sp);
            if (l >= sizeof(h->subproto)) l = sizeof(h->subproto) - 1;
            memcpy(h->subproto, sp, l);
            h->subproto[l] = 0;
        }
    }

    /* Bytes already consumed past the header block belong to the frame
     * stream; stash them so the first wss_poll() sees them. */
    {
        const char *hdr_end = strstr(resp, "\r\n\r\n");
        const char *body = hdr_end + 4;
        extra = (size_t)(resp + total - body);
        if (extra > 0) {
            if (extra > sizeof(h->rbuf)) {
                set_err(h, "%u stray bytes after upgrade headers",
                        (unsigned)extra);
                return -1;
            }
            memcpy(h->rbuf, body, extra);
            h->rlen = extra;
        }
    }
    return 0;
}

/* ------------------------------------------------------------- frame I/O */

static int ssl_write_all(wss_t *h, const unsigned char *p, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        int w = SSL_write(h->ssl, p + sent, (int)(n - sent));
        if (w > 0) { sent += (size_t)w; continue; }
        {
            int e = SSL_get_error(h->ssl, w);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                struct pollfd pf;
                pf.fd = h->fd;
                pf.events = (e == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                pf.revents = 0;
                if (poll(&pf, 1, 5000) < 0) {
                    set_err(h, "poll(write): %s", strerror(errno));
                    return -1;
                }
                continue;
            }
            set_err(h, "SSL_write failed e=%d", e);
            return -1;
        }
    }
    return 0;
}

static int send_frame(wss_t *h, int opcode, const void *data, size_t len)
{
    unsigned char hdr[WSS_MAX_HDR];
    unsigned char mask[4];
    size_t hl = 0, i;
    unsigned char *tmp;

    if (!h || h->closed || h->failed) return -1;

    hdr[0] = (unsigned char)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        hdr[1] = (unsigned char)(0x80 | len);
        hl = 2;
    } else if (len <= 0xFFFF) {
        hdr[1] = (unsigned char)(0x80 | 126);
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)(len & 0xFF);
        hl = 4;
    } else {
        hdr[1] = (unsigned char)(0x80 | 127);
        for (i = 0; i < 8; i++)
            hdr[2 + i] = (unsigned char)((unsigned long long)len >> (56 - 8 * i));
        hl = 10;
    }
    RAND_bytes(mask, (int)sizeof(mask));
    memcpy(hdr + hl, mask, 4);
    hl += 4;

    if (ssl_write_all(h, hdr, hl) != 0) return -1;
    if (!len) return 0;

    /* The caller's buffer is const, so mask a copy. */
    tmp = (unsigned char *)malloc(len);
    if (!tmp) { set_err(h, "oom masking %u bytes", (unsigned)len); return -1; }
    {
        const unsigned char *src = (const unsigned char *)data;
        for (i = 0; i < len; i++) tmp[i] = (unsigned char)(src[i] ^ mask[i & 3]);
    }
    i = (size_t)ssl_write_all(h, tmp, len);
    free(tmp);
    return (int)i;
}

int wss_send_bin(wss_t *h, const void *data, size_t len)
{
    return send_frame(h, WSS_OP_BIN, data, len);
}

int wss_send_text(wss_t *h, const char *s)
{
    if (!s) return -1;
    return send_frame(h, WSS_OP_TEXT, s, strlen(s));
}

int wss_send_ping(wss_t *h, const void *data, size_t len)
{
    if (len > 125) len = 125;
    return send_frame(h, WSS_OP_PING, data, len);
}

/* ---------------------------------------------------------------- reader */

/* Returns bytes appended, 0 on timeout, -1 on EOF/fatal error. */
static int fill(wss_t *h, int wait_ms)
{
    for (;;) {
        int n, e;
        struct pollfd p;

        if (h->rlen >= sizeof(h->rbuf)) return 0;   /* caller must consume */

        n = SSL_read(h->ssl, h->rbuf + h->rlen, (int)(sizeof(h->rbuf) - h->rlen));
        if (n > 0) { h->rlen += (size_t)n; return n; }

        e = SSL_get_error(h->ssl, n);
        if (e == SSL_ERROR_ZERO_RETURN) return -1;
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            if (!(e == SSL_ERROR_SYSCALL && n == 0))
                set_err(h, "SSL_read failed e=%d", e);
            return -1;
        }
        p.fd = h->fd;
        p.events = (e == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
        p.revents = 0;
        n = poll(&p, 1, wait_ms);
        if (n == 0) return 0;                       /* timeout */
        if (n < 0) {
            if (errno == EINTR) continue;
            set_err(h, "poll(read): %s", strerror(errno));
            return -1;
        }
        wait_ms = 0;                                /* only wait once */
    }
}

/* 1 = frame consumed, 0 = need more bytes, -1 = protocol error. */
static int parse_frame(wss_t *h, int *opcode, int *fin,
                       const unsigned char **payload, size_t *plen)
{
    unsigned char *b = h->rbuf;
    size_t hl, need, i;
    unsigned long long len;
    int masked;

    if (h->rlen < 2) return 0;
    *fin = (b[0] & 0x80) != 0;
    *opcode = b[0] & 0x0F;
    masked = (b[1] & 0x80) != 0;
    len = b[1] & 0x7F;
    hl = 2;

    if (len == 126) {
        if (h->rlen < 4) return 0;
        len = ((unsigned)b[2] << 8) | b[3];
        hl = 4;
    } else if (len == 127) {
        if (h->rlen < 10) return 0;
        len = 0;
        for (i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
        hl = 10;
    }
    if (masked) {
        if (h->rlen < hl + 4) return 0;
        hl += 4;
    }
    if (len > WSS_MAX_PAYLOAD) {
        set_err(h, "frame payload too large: %llu bytes", len);
        return -1;
    }
    need = hl + (size_t)len;
    if (h->rlen < need) return 0;

    if (masked) {
        const unsigned char *m = b + hl - 4;
        for (i = 0; i < (size_t)len; i++) b[hl + i] ^= m[i & 3];
    }
    *payload = b + hl;
    *plen = (size_t)len;

    if (need < h->rlen)
        memmove(b, b + need, h->rlen - need);
    h->rlen -= need;
    return 1;
}

/* Append to the reassembly buffer. Returns 0 on success, -1 on OOM. */
static int append_msg(wss_t *h, const unsigned char *p, size_t n)
{
    if (!n) return 0;
    if (h->msg_len + n > h->msg_cap) {
        size_t cap = h->msg_cap ? h->msg_cap : 8192;
        unsigned char *nm;
        while (cap < h->msg_len + n) cap *= 2;
        if (cap > WSS_MSGBUF_CAP) {
            set_err(h, "reassembled message would exceed %u bytes",
                    (unsigned)WSS_MSGBUF_CAP);
            return -1;
        }
        nm = (unsigned char *)realloc(h->msg, cap);
        if (!nm) { set_err(h, "oom reassembling %u bytes", (unsigned)cap); return -1; }
        h->msg = nm;
        h->msg_cap = cap;
    }
    memcpy(h->msg + h->msg_len, p, n);
    h->msg_len += n;
    return 0;
}

/* Drain every complete frame currently in rbuf.
 *  1  keep going / nothing pending
 *  0  need more bytes
 * -1  fatal
 * -2  peer closed (callback already told) */
static int drain(wss_t *h, wss_msg_cb cb, void *user, int *count)
{
    for (;;) {
        int op, fin, rc;
        const unsigned char *pl;
        size_t plen;

        rc = parse_frame(h, &op, &fin, &pl, &plen);
        if (rc < 0) return -1;
        if (rc == 0) return 0;

        if (op >= 0x8) {
            /* Control frames are never fragmented and may interleave a
             * fragmented data message. RFC6455 requires FIN on them. */
            if (op == WSS_OP_PING) {
                if (send_frame(h, WSS_OP_PONG, pl, plen) != 0) return -1;
                if (cb) cb(h, WSS_OP_PING, pl, plen, user);
            } else if (op == WSS_OP_PONG) {
                if (cb) cb(h, WSS_OP_PONG, pl, plen, user);
            } else if (op == WSS_OP_CLOSE) {
                if (!h->closed) {
                    unsigned char z[2];
                    z[0] = 0x03; z[1] = 0xE8;      /* 1000 normal closure */
                    send_frame(h, WSS_OP_CLOSE, z, sizeof(z));
                    h->closed = 1;
                }
                if (cb) cb(h, WSS_OP_CLOSE, NULL, 0, user);
                return -2;
            }
            (*count)++;
            continue;
        }

        if (op == WSS_OP_CONT) {
            if (h->msg_opcode == 0) {
                set_err(h, "continuation frame without an open message");
                return -1;
            }
        } else {
            if (h->msg_opcode != 0) {
                set_err(h, "data frame while a fragmented message is open");
                return -1;
            }
            h->msg_opcode = op;
            h->msg_len = 0;
        }

        if (append_msg(h, pl, plen) != 0) return -1;

        if (fin) {
            int o = h->msg_opcode;
            h->msg_opcode = 0;
            (*count)++;
            if (cb) cb(h, o, h->msg, h->msg_len, user);
        }
    }
}

int wss_poll(wss_t *h, int timeout_ms, wss_msg_cb cb, void *user)
{
    int count = 0, rc;
    long deadline;

    if (!h) return -1;
    if (h->failed) return -1;
    if (h->closed) return -2;

    rc = drain(h, cb, user, &count);
    if (rc == -1 || rc == -2) return rc;

    deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        int left, got;

        if (timeout_ms < 0) left = -1;
        else {
            left = (int)(deadline - now_ms());
            if (left < 0) left = 0;
        }

        got = fill(h, left);
        if (got < 0) {
            /* Peer vanished without a close frame: surface it as a close. */
            if (!h->failed) {
                if (cb) cb(h, WSS_OP_CLOSE, NULL, 0, user);
                h->closed = 1;
                return count > 0 ? count : -2;
            }
            return -1;
        }
        if (got == 0) return count;             /* timeout expired */

        rc = drain(h, cb, user, &count);
        if (rc == -1 || rc == -2) return rc;

        if (timeout_ms == 0) return count;
        if (timeout_ms > 0 && now_ms() >= deadline) return count;
    }
}

/* ---------------------------------------------------------------- public */

wss_t *wss_connect(const char *host, int port, const char *path,
                   const char *subprotocols, int timeout_ms)
{
    wss_t *h;
    long t0, t1, t2, t3;

    if (!host || !path) {
        set_err(NULL, "wss_connect: host and path are required");
        return NULL;
    }
    g_last_error[0] = 0;

    h = (wss_t *)calloc(1, sizeof(*h));
    if (!h) { set_err(NULL, "wss_connect: oom"); return NULL; }
    h->fd = -1;

    t0 = now_ms();
    h->fd = tcp_connect(host, port, timeout_ms, h);
    if (h->fd < 0) { wss_close(h); return NULL; }
    t1 = now_ms();
    h->tcp_ms = (int)(t1 - t0);

    {
        int one = 1;
        /* Non-blocking from here on: SSL_read() must return WANT_READ so that
         * wss_poll()'s timeout is honoured by poll(2) instead of the kernel's
         * SO_RCVTIMEO (a blocking socket made every poll() take 8 s). */
        int fl = fcntl(h->fd, F_GETFL, 0);
        fcntl(h->fd, F_SETFL, fl | O_NONBLOCK);
        /* TCP_NODELAY == 1 on Linux; keeping the literal avoids pulling in
         * <netinet/tcp.h> for one constant. */
        setsockopt(h->fd, IPPROTO_TCP, 1, &one, sizeof(one));
    }

    if (tls_handshake(h, host, timeout_ms) != 0) { wss_close(h); return NULL; }
    t2 = now_ms();
    h->tls_ms = (int)(t2 - t1);

    if (ws_upgrade(h, host, path, subprotocols, timeout_ms) != 0) {
        wss_close(h);
        return NULL;
    }
    t3 = now_ms();
    h->upgrade_ms = (int)(t3 - t2);
    h->total_ms = (int)(t3 - t0);
    return h;
}

void wss_close(wss_t *h)
{
    if (!h) return;
    if (h->ssl) {
        if (!h->closed && !h->failed) {
            unsigned char z[2];
            z[0] = 0x03; z[1] = 0xE8;
            send_frame(h, WSS_OP_CLOSE, z, sizeof(z));
            h->closed = 1;
        }
        SSL_shutdown(h->ssl);
        SSL_free(h->ssl);
        h->ssl = NULL;
    }
    if (h->ctx) { SSL_CTX_free(h->ctx); h->ctx = NULL; }
    if (h->fd >= 0) { close(h->fd); h->fd = -1; }
    free(h->msg);
    free(h);
}

const char *wss_last_error(void)
{
    return g_last_error[0] ? g_last_error : "no error";
}

const char *wss_error(const wss_t *h)
{
    if (h && h->err[0]) return h->err;
    return wss_last_error();
}

void wss_timing(const wss_t *h, int *tcp_ms, int *tls_ms, int *upgrade_ms,
                int *total_ms)
{
    if (tcp_ms) *tcp_ms = h ? h->tcp_ms : -1;
    if (tls_ms) *tls_ms = h ? h->tls_ms : -1;
    if (upgrade_ms) *upgrade_ms = h ? h->upgrade_ms : -1;
    if (total_ms) *total_ms = h ? h->total_ms : -1;
}

const char *wss_selected_subprotocol(const wss_t *h)
{
    return h ? h->subproto : "";
}

int wss_fd(const wss_t *h)
{
    return h ? h->fd : -1;
}
