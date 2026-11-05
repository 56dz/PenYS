/* ys_net.h -- cloud-Genshin native HTTP core for the X6 Pro.
 *
 * WHY THIS EXISTS
 * The Falcon JS runtime's built-in http jsapi cannot read response headers and
 * cannot preserve large bodies (PenBili measured a 1.2 MB body arriving as
 * 2 bytes). mihoyo hands the whole session out through `Set-Cookie`, so both
 * limits are fatal. This module drives libcurl directly: it collects the raw
 * header block, keeps a per-key cookie jar, and reports the real status code.
 *
 * PLATFORM NOTES (verified on device, Buildroot / X6 Pro)
 *   - no curl headers ship on the device -> every CURLOPT value is #defined
 *     here and was checked against libcurl's own name table at runtime
 *     (see .tmp/feas/net/optprobe.c): 30/31 match, and CURLOPT_COOKIELIST is
 *     10135, NOT the 10091 that older header sets use.
 *   - /etc/ssl/certs/ca-certificates.crt exists and validates mihoyo hosts.
 *   - every request is a hard timeout (CURLOPT_TIMEOUT_MS + NOSIGNAL), because
 *     a stuck request in a cloud-game chain is worse than a failed one.
 */
#ifndef YS_NET_H
#define YS_NET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- growable byte buffer ---------------- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ys_buf;

void ys_buf_init(ys_buf *b);
void ys_buf_free(ys_buf *b);
int ys_buf_append(ys_buf *b, const void *p, size_t n);
int ys_buf_puts(ys_buf *b, const char *s);
/* NUL-terminates in place (the terminator is not counted in len). */
const char *ys_buf_cstr(ys_buf *b);

/* ---------------- one HTTP response ---------------- */
typedef struct {
    int ok;          /* transport succeeded and a status line was parsed */
    long status;     /* HTTP status, 0 if the transfer failed */
    int curl_code;   /* raw CURLcode */
    int insecure;    /* 1 when TLS verification had to be disabled */
    char *headers;   /* raw response header block(s), CRLF preserved */
    char *body;
    size_t body_len;
    char *error;     /* human-readable failure, or NULL */
} ys_resp;

void ys_resp_free(ys_resp *r);

/* Perform one request. All strings are borrowed; `out` is filled in and must
 * be released with ys_resp_free().
 *
 * method         "GET" / "POST" / "PUT" / ... (NULL = GET, or POST if body)
 * header_lines   "K: V\r\nK2: V2" (CRLF or LF separated), NULL for none
 * body           request body bytes, or NULL
 * timeout_ms     hard wall-clock limit for the whole transfer
 * follow         non-zero to follow 3xx (max 5)
 * jar_key        cookie-jar bucket; NULL/"" = a throwaway jar
 *
 * Thread-safe: the jar table is mutex protected, so concurrent requests may
 * use the same jar_key (they serialize on that jar only).
 * Returns 0 if the transport succeeded (even for 4xx/5xx), -1 on failure. */
/* Same as ys_http_do but captures the exact request header block libcurl sent
 * (CURLINFO_HEADER_OUT). Used to PROVE cookie replay: the outgoing text must
 * contain "Cookie:". dbg_out may be NULL. */
int ys_http_do_debug(const char *method, const char *url, const char *header_lines,
                     const char *body, size_t body_len, long timeout_ms, int follow,
                     const char *jar_key, ys_resp *out, char *dbg_out, size_t dbg_cap);

int ys_http_do(const char *method, const char *url, const char *header_lines,
               const char *body, size_t body_len, long timeout_ms,
               int follow, const char *jar_key, ys_resp *out);

/* ---------------- cookie jars ---------------- */
/* Dump the jar as Netscape cookie-file text (as libcurl reports it). Caller
 * frees. Returns NULL for an unknown key or an empty jar. */
char *ys_jar_dump(const char *jar_key);
/* Import Netscape cookie-file text into the jar (replaces nothing: adds). */
int ys_jar_import(const char *jar_key, const char *text);
int ys_jar_save(const char *jar_key, const char *path);
int ys_jar_load(const char *jar_key, const char *path);
int ys_jar_clear(const char *jar_key);
/* Number of cookies currently held. */
int ys_jar_count(const char *jar_key);

/* ---------------- crypto (libcrypto, not QuickJS) ---------------- */
/* QuickJS on this device is built without BigInt and has no crypto module, so
 * HMAC and RSA both live here. Returns malloc'd strings; NULL on failure. */
char *ys_hmac_sha256_hex(const char *key, size_t key_len,
                         const void *data, size_t data_len);
char *ys_sha256_hex(const void *data, size_t data_len);
/* RSA public-key encrypt (PKCS#1 v1.5) of a UTF-8 string, base64 out.
 * pubkey_b64 is an X.509 SubjectPublicKeyInfo in DER, base64 encoded
 * (exactly the mihoyo passport key). err (may be NULL) receives malloc'd
 * detail. */
char *ys_rsa_encrypt_b64(const char *pubkey_b64, const char *plain,
                         size_t plain_len, char **err);

/* ---------------- base64 ---------------- */
char *ys_b64_encode(const void *p, size_t n);
unsigned char *ys_b64_decode(const char *s, size_t *out_len);

/* ---------------- WebSocket RTT probe ---------------- */
typedef struct {
    int ok;          /* 1 if the handshake succeeded and >=1 pong arrived */
    int sent;
    int recvd;
    int avg_rtt;     /* mean round trip, ms; 0 when unknown */
    int min_rtt;
    int max_rtt;
    int connect_ms;
    int tls_ms;
    char error[192];
} ys_rtt_result;

/* Opens a WSS connection to `url` (wss://host[:port]/path), waits for the
 * upgrade, then sends `count` text frames and measures each reply.
 *
 * AF_INET IS MANDATORY: with AF_UNSPEC this firmware tries IPv6 first and the
 * TCP connect takes ~15 s instead of ~250 ms (measured; same trap as
 * .tmp/feas/srtp/ws_probe.c). */
int ys_ws_rtt_test(const char *url, int count, int timeout_ms,
                   ys_rtt_result *out);

/* Milliseconds since an arbitrary fixed point. */
long long ys_now_ms(void);

/* ---------------- JSAPI registration ---------------- */
/* Registers the "yshttp" and "ysws" C modules. Called from the project's
 * custom_init_jsapis(). */
void ys_net_register_jsapis(void);

#ifdef __cplusplus
}
#endif
#endif /* YS_NET_H */
