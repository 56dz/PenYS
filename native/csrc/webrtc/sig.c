/* sig.c -- cloud-gaming signaling state machine over WSS.
 *
 * Single-threaded and non-blocking except for sig_open(), which does the TCP +
 * TLS + upgrade dance (measured 222-263 ms against the real service) and must
 * run on the worker thread.
 *
 * Wire format (from the reverse-engineered client):
 *   frame = u32 type | u32 length | payload
 *   type 1 = signaling JSON (SDP, ICE candidates)
 *   type 2 = proxy bytes
 *   type 3 = handshake JSON ("client hello" / "server hello")
 *   type 4 = explicit keepalive
 * The periodic keepalive is NOT a type-4 frame: it is a bare empty WebSocket
 * text frame, exactly like `websocket.send("")` in the reference client.
 */

#include "sig.h"
#include "wss.h"
#include "sdp.h"
#include "core/log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIG_OFFER_CAP   (64 * 1024)
#define SIG_ANSWER_CAP  (32 * 1024)
#define SIG_TMP_CAP     (96 * 1024)
#define SIG_MAX_FRAME   (8u * 1024u * 1024u)

struct sig {
    sig_cfg_t cfg;
    char cfg_url[512];
    char cfg_subproto[256];
    char cfg_fp[256];
    char cfg_cand_ip[64];
    char cfg_local_ip[64];

    /* StartGameReq strings. Same idea as the buffers above: the caller's
     * storage may die the moment sig_create() returns. */
    char cfg_local_cand_ip[64];
    char cfg_game_token[512];
    char cfg_account_id[128];
    char cfg_sid[256];
    char cfg_pod_id[256];
    char cfg_sdk_version[64];

    char host[256];
    int  port;
    char path[512];

    wss_t *wss;
    int    state;

    char  *offer;          /* last offer received (JSON-unescaped SDP) */
    size_t offer_len;

    /* Payload types we actually agreed to, cached when the answer is built.
     * The answer echoes the offer's numbers, so these equal the offer's, but
     * caching them means the caller never has to keep the offer around.
     * -1 = not negotiated yet. */
    int vpt;
    int apt;

    long long t_open_ms;
    long long t_last_rx_ms;
    long long t_last_hb_ms;

    int  hb_sent;
    int  frames_rx;
    int  closing;
    int  poll_depth;       /* re-entrancy guard for callbacks */

    int  hello_sent;       /* client hello already on the wire              */
    int  start_rsp_ok;     /* StartGameRsp with retcode==RtcSucc arrived    */
    int  start_rsp_code;   /* last retcode seen, for the error message      */
    long long t_start_game_ms;  /* when StartGameReq went out                */

    /* ICE short-term credentials of the current negotiation. The session's
     * STUN layer needs all four: USERNAME is remote:local, MESSAGE-INTEGRITY
     * is keyed with the remote pwd, and the responses are keyed with ours. */
    char local_ice_ufrag[64];
    char local_ice_pwd[128];
    char remote_ice_ufrag[64];
    char remote_ice_pwd[128];

    char err[256];
};

/* ------------------------------------------------------------------ time */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void set_err(sig_conn_t *s, const char *fmt, ...)
{
    va_list ap;
    if (!s) return;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof(s->err), fmt, ap);
    va_end(ap);
    s->err[sizeof(s->err) - 1] = 0;
    ys_logf("sig: err: %s", s->err);
}

const char *sig_last_error(const sig_conn_t *s)
{
    return s ? s->err : "sig: NULL handle";
}

/* ----------------------------------------------------------------- state */

const char *sig_state_name(int st)
{
    switch (st) {
    case SIG_STATE_IDLE:       return "idle";
    case SIG_STATE_CONNECTING: return "connecting";
    case SIG_STATE_OPEN:       return "open";
    case SIG_STATE_OFFERED:    return "offered";
    case SIG_STATE_ANSWERED:   return "answered";
    case SIG_STATE_CONNECTED:  return "connected";
    case SIG_STATE_STALE:      return "stale";
    case SIG_STATE_CLOSED:     return "closed";
    case SIG_STATE_FAILED:     return "failed";
    default:                   return "?";
    }
}

static void set_state(sig_conn_t *s, int st)
{
    int old = s->state;
    if (old == st) return;
    s->state = st;
    if (s->cfg.cbs.on_state)
        s->cfg.cbs.on_state(s, old, st, s->cfg.user);
}

/* ------------------------------------------------------------------ JSON */

/* Minimal escape-preserving extraction: find "key" at the top level-ish, then
 * read the following JSON string into out with escapes decoded. We do not
 * build a parser because the payloads are two known shapes and a full parser
 * would be more code to get wrong. */
static int json_find_key(const char *j, const char *key)
{
    size_t klen = strlen(key);
    const char *p = j;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q != ':') return -1;
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            return (int)(q - j);
        }
        /* skip to the end of this string */
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        if (!*p) break;
        p++;
    }
    return -1;
}

static int hexdig(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Read a JSON string starting at `p` (which must be '"') into out, decoding
 * \n \r \t \\ \" \/ \uXXXX. Returns bytes written, or -1. */
static int json_read_string(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    if (!p || *p != '"') return -1;
    p++;
    while (*p && *p != '"') {
        int c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; p++; break;
            case 'r': c = '\r'; p++; break;
            case 't': c = '\t'; p++; break;
            case 'b': c = '\b'; p++; break;
            case 'f': c = '\f'; p++; break;
            case '/': c = '/';  p++; break;
            case '\\': c = '\\'; p++; break;
            case '"': c = '"';  p++; break;
            case 'u': {
                int h0, h1, h2, h3;
                unsigned cp;
                if (!p[1] || !p[2] || !p[3] || !p[4]) return -1;
                h0 = hexdig(p[1]); h1 = hexdig(p[2]);
                h2 = hexdig(p[3]); h3 = hexdig(p[4]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return -1;
                cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                p += 5;
                /* Enough for SDP: encode as UTF-8, no surrogate pairs. */
                if (cp < 0x80) {
                    c = (int)cp;
                } else if (cp < 0x800) {
                    if (n + 2 >= cap) return -1;
                    out[n++] = (char)(0xC0 | (cp >> 6));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                    continue;
                } else {
                    if (n + 3 >= cap) return -1;
                    out[n++] = (char)(0xE0 | (cp >> 12));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                    continue;
                }
                break;
            }
            default:
                return -1;
            }
        } else {
            p++;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    if (*p != '"') return -1;
    out[n] = 0;
    return (int)n;
}

static int json_get_string(const char *j, const char *key, char *out, size_t cap)
{
    int off = json_find_key(j, key);
    if (off < 0) return -1;
    return json_read_string(j + off, out, cap);
}

/* Escape `in` into `out` as a JSON string body (no surrounding quotes).
 * Returns bytes written or -1. */
static int json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)in;
    for (; *p; p++) {
        unsigned char c = *p;
        const char *esc = NULL;
        char buf[8];
        size_t need;

        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default: break;
        }
        if (esc) {
            need = strlen(esc);
            if (n + need >= cap) return -1;
            memcpy(out + n, esc, need);
            n += need;
            continue;
        }
        if (c < 0x20) {
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            need = strlen(buf);
            if (n + need >= cap) return -1;
            memcpy(out + n, buf, need);
            n += need;
            continue;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return (int)n;
}

/* ------------------------------------------------------------- URL parse */

static int parse_url(const char *url, char *host, size_t hcap, int *port,
                     char *path, size_t pcap)
{
    const char *p;
    size_t hl = 0;
    if (!url) return -1;
    if (strncmp(url, "wss://", 6) == 0) p = url + 6;
    else if (strncmp(url, "ws://", 5) == 0) p = url + 5;
    else return -1;

    while (*p && *p != ':' && *p != '/' && hl + 1 < hcap)
        host[hl++] = *p++;
    host[hl] = 0;
    if (hl == 0) return -1;

    *port = 443;
    if (*p == ':') {
        char *end = NULL;
        long v;
        p++;
        v = strtol(p, &end, 10);
        if (!end || end == p || v <= 0 || v > 65535) return -1;
        *port = (int)v;
        p = end;
    }
    if (*p != '/') {
        if (*p) return -1;
        snprintf(path, pcap, "/");
        return 0;
    }
    snprintf(path, pcap, "%s", p);
    return 0;
}

/* --------------------------------------------------------------- lifecycle */

static void copy_cfg_strings(sig_conn_t *s, const sig_cfg_t *cfg)
{
    /* Copy the strings FIRST, then point the config at our storage. Doing it
     * the other way round leaves every pointer reading an empty buffer and
     * silently disables the fingerprint. */
    snprintf(s->cfg_url, sizeof(s->cfg_url), "%s", cfg->url ? cfg->url : "");
    snprintf(s->cfg_subproto, sizeof(s->cfg_subproto), "%s",
             cfg->subprotocols ? cfg->subprotocols : "");
    snprintf(s->cfg_fp, sizeof(s->cfg_fp), "%s",
             cfg->fingerprint ? cfg->fingerprint : "");
    snprintf(s->cfg_cand_ip, sizeof(s->cfg_cand_ip), "%s",
             cfg->candidate_ip ? cfg->candidate_ip : "");
    snprintf(s->cfg_local_ip, sizeof(s->cfg_local_ip), "%s",
             cfg->local_ip ? cfg->local_ip : "");
    snprintf(s->cfg_local_cand_ip, sizeof(s->cfg_local_cand_ip), "%s",
             cfg->local_cand_ip ? cfg->local_cand_ip : "");
    snprintf(s->cfg_game_token, sizeof(s->cfg_game_token), "%s",
             cfg->game_token ? cfg->game_token : "");
    snprintf(s->cfg_account_id, sizeof(s->cfg_account_id), "%s",
             cfg->account_id ? cfg->account_id : "");
    snprintf(s->cfg_sid, sizeof(s->cfg_sid), "%s", cfg->sid ? cfg->sid : "");
    snprintf(s->cfg_pod_id, sizeof(s->cfg_pod_id), "%s",
             cfg->pod_id ? cfg->pod_id : "");
    snprintf(s->cfg_sdk_version, sizeof(s->cfg_sdk_version), "%s",
             cfg->sdk_version ? cfg->sdk_version : "");

    s->cfg = *cfg;
    s->cfg.url = s->cfg_url;
    s->cfg.subprotocols = s->cfg_subproto[0] ? s->cfg_subproto : NULL;
    s->cfg.fingerprint = s->cfg_fp[0] ? s->cfg_fp : NULL;
    s->cfg.candidate_ip = s->cfg_cand_ip[0] ? s->cfg_cand_ip : NULL;
    s->cfg.local_cand_ip = s->cfg_local_cand_ip[0] ? s->cfg_local_cand_ip : NULL;
    s->cfg.local_ip = s->cfg_local_ip[0] ? s->cfg_local_ip : NULL;
    s->cfg.game_token = s->cfg_game_token;
    s->cfg.account_id = s->cfg_account_id;
    s->cfg.sid = s->cfg_sid;
    s->cfg.pod_id = s->cfg_pod_id;
    s->cfg.sdk_version = s->cfg_sdk_version;
}

/* --------------------------------------------------- StartGameReq / Rsp --
 *
 * Why this exists at all is worth writing down, because the old header of
 * this file described a handshake that does not happen.
 *
 * "client hello" alone is NOT enough to make the service send an offer. The
 * reference client sends it only AFTER the game pod has acknowledged that it
 * is booting our session. The order is
 *
 *     WSS up
 *       -> type-2 proxy frame carrying StartGameReq(20001)
 *       -> type-2 proxy frame carrying StartGameRsp(20002), retcode==1
 *       -> type-3 "client hello"
 *       -> type-1 {"type":"offer","sdp":...}
 *
 * If we skip straight to client hello the server replies with exactly one
 * `{"type":"server hello","session_id":1}` -- which is not a prompt, it only
 * carries the id the reference client uses on the separate :5443 control
 * channel -- and then never speaks again. That is the NO_OFFER hang.
 */

typedef struct {
    unsigned char *buf;
    size_t cap;
    size_t len;
    int    ok;
} pb_w;

static void pb_w_bytes(pb_w *w, const void *p, size_t n)
{
    if (!w->ok || w->len + n > w->cap) { w->ok = 0; return; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void pb_w_varint(pb_w *w, unsigned long long v)
{
    unsigned char tmp[10];
    size_t n = 0;

    while (v >= 0x80) {
        tmp[n++] = (unsigned char)((v & 0x7F) | 0x80);
        v >>= 7;
    }
    tmp[n++] = (unsigned char)(v & 0x7F);
    pb_w_bytes(w, tmp, n);
}

static void pb_w_key(pb_w *w, int field, int wire_type)
{
    pb_w_varint(w, ((unsigned long long)field << 3) | (unsigned)wire_type);
}

/* proto3 semantics: empty strings and zero numbers are omitted entirely. */
static void pb_w_str(pb_w *w, int field, const char *s)
{
    size_t n;
    if (!s || !s[0]) return;
    n = strlen(s);
    pb_w_key(w, field, 2);
    pb_w_varint(w, (unsigned long long)n);
    pb_w_bytes(w, s, n);
}

static void pb_w_u32(pb_w *w, int field, unsigned int v)
{
    if (v == 0) return;
    pb_w_key(w, field, 0);
    pb_w_varint(w, v);
}

/* Wrap `msg` in a Packet: magic | cmd_id | head_len | msg_len | msg | tail.
 * Head is always empty here, so the envelope is exactly 12 + msg_len bytes. */
static size_t packet_pack(int cmd, const unsigned char *msg, size_t msg_len,
                          unsigned char *out, size_t cap)
{
    size_t total = SIG_PACKET_MIN_SIZE + msg_len;

    if (!out || total > cap) return 0;

    out[0] = (unsigned char)(SIG_PACKET_MAGIC_HEAD >> 8);
    out[1] = (unsigned char)(SIG_PACKET_MAGIC_HEAD & 0xFF);
    out[2] = (unsigned char)((cmd >> 8) & 0xFF);
    out[3] = (unsigned char)(cmd & 0xFF);
    out[4] = 0;                                   /* head_len = 0 */
    out[5] = 0;
    out[6] = (unsigned char)((msg_len >> 24) & 0xFF);
    out[7] = (unsigned char)((msg_len >> 16) & 0xFF);
    out[8] = (unsigned char)((msg_len >> 8) & 0xFF);
    out[9] = (unsigned char)(msg_len & 0xFF);
    if (msg_len)
        memcpy(out + 10, msg, msg_len);
    out[total - 2] = (unsigned char)(SIG_PACKET_MAGIC_TAIL >> 8);
    out[total - 1] = (unsigned char)(SIG_PACKET_MAGIC_TAIL & 0xFF);
    return total;
}

/* Read the first varint out of a protobuf message; used for StartGameRsp's
 * retcode (field 1, int32). Returns 0 on success. */
static int pb_first_varint_field(const unsigned char *p, size_t n,
                                 int want_field, long long *out)
{
    size_t i = 0;

    while (i < n) {
        unsigned long long key = 0;
        int shift = 0;
        int field, wt;

        while (i < n) {
            unsigned char b = p[i++];
            key |= (unsigned long long)(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
            if (shift > 63) return -1;
        }
        field = (int)(key >> 3);
        wt = (int)(key & 7);

        if (wt == 0) {
            unsigned long long v = 0;
            int s2 = 0;
            while (i < n) {
                unsigned char b = p[i++];
                v |= (unsigned long long)(b & 0x7F) << s2;
                s2 += 7;
                if (!(b & 0x80)) break;
                if (s2 > 63) return -1;
            }
            if (field == want_field) {
                *out = (long long)v;
                return 0;
            }
        } else if (wt == 2) {
            unsigned long long l = 0;
            int s2 = 0;
            while (i < n) {
                unsigned char b = p[i++];
                l |= (unsigned long long)(b & 0x7F) << s2;
                s2 += 7;
                if (!(b & 0x80)) break;
                if (s2 > 63) return -1;
            }
            if (l > (unsigned long long)(n - i)) return -1;
            i += (size_t)l;
        } else if (wt == 5) {
            if (i + 4 > n) return -1;
            i += 4;
        } else if (wt == 1) {
            if (i + 8 > n) return -1;
            i += 8;
        } else {
            return -1;              /* groups are not used by this protocol */
        }
    }
    return -1;
}

/* Build + queue a StartGameReq. Returns 0 on success. */
static int send_start_game(sig_conn_t *s)
{
    unsigned char msg[1024];
    unsigned char *pkt;
    size_t total;
    pb_w w;
    long long now = now_ms();
    long long since_start_sec;
    long long link_ms;
    int rc;

    if (!s || !s->wss) return -1;

    since_start_sec = (s->t_start_game_ms > 0)
        ? (now - s->t_start_game_ms) / 1000 : 0;
    link_ms = (s->t_open_ms > 0) ? (now - s->t_open_ms) : 0;

    w.buf = msg;
    w.cap = sizeof(msg);
    w.len = 0;
    w.ok = 1;

    pb_w_str(&w, 1, s->cfg.game_token);
    pb_w_str(&w, 2, s->cfg.account_id);
    pb_w_str(&w, 3, s->cfg.sdk_version);
    pb_w_u32(&w, 4, (unsigned)s->cfg.terminal_type);
    pb_w_str(&w, 6, s->cfg.sid);
    pb_w_str(&w, 7, s->cfg.pod_id);
    pb_w_u32(&w, 8, 0);            /* has_rtc_connected: no DTLS yet     */
    pb_w_u32(&w, 9, (unsigned)since_start_sec);
    pb_w_u32(&w, 10, (unsigned)link_ms);
    /* field 12 game_config: an empty SdkGameConfig serialises to zero bytes,
     * so the reference client's message would be written as tag+len 0. It is
     * optional in practice; omit it rather than send a degenerate message. */

    if (!w.ok) {
        set_err(s, "StartGameReq: message too large");
        return -1;
    }

    s->t_start_game_ms = now;

    pkt = (unsigned char *)malloc(SIG_PACKET_MIN_SIZE + w.len);
    if (!pkt) {
        set_err(s, "StartGameReq: out of memory");
        return -1;
    }
    total = packet_pack(SIG_CMD_START_GAME_REQ, msg, w.len, pkt,
                        SIG_PACKET_MIN_SIZE + w.len);
    if (total == 0) {
        free(pkt);
        set_err(s, "StartGameReq: packet_pack failed");
        return -1;
    }

    rc = sig_send(s, SIG_TYPE_PROXY, pkt, total);
    free(pkt);
    if (rc != 0) {
        set_err(s, "StartGameReq: %s", wss_error(s->wss));
        set_state(s, SIG_STATE_FAILED);
        return -1;
    }

    ys_logf("sig: StartGameReq sent (%zu bytes, sid=%s pod=%s aid=%s sdk=%s)",
            total, s->cfg.sid, s->cfg.pod_id, s->cfg.account_id,
            s->cfg.sdk_version);
    return 0;
}

/* Send the type-3 client hello. Guarded so it can never go out twice. */
static int send_client_hello(sig_conn_t *s)
{
    static const char hello[] =
        "{\"client_type\":\"web\",\"type\":\"client hello\"}";

    if (!s || !s->wss) return -1;
    if (s->hello_sent) return 0;
    if (sig_send(s, SIG_TYPE_HANDSHAKE, hello, sizeof(hello) - 1) != 0) {
        set_err(s, "client hello: %s", wss_error(s->wss));
        set_state(s, SIG_STATE_FAILED);
        return -1;
    }
    s->hello_sent = 1;
    ys_logf("sig: client hello sent");
    return 0;
}

/* Handle one type-2 payload. Returns 1 if it was a Packet we understood (the
 * caller then swallows it), 0 if it is opaque and should be forwarded. */
static int handle_packet(sig_conn_t *s, const unsigned char *p, size_t n)
{
    unsigned int magic;
    int cmd;
    unsigned long msg_len;

    if (!s || !p || n < SIG_PACKET_MIN_SIZE) return 0;

    magic = ((unsigned)p[0] << 8) | (unsigned)p[1];
    if (magic != SIG_PACKET_MAGIC_HEAD) return 0;

    cmd = ((int)p[2] << 8) | (int)p[3];
    msg_len = ((unsigned long)p[6] << 24) | ((unsigned long)p[7] << 16) |
              ((unsigned long)p[8] << 8) | (unsigned long)p[9];

    {
        size_t head_len = ((size_t)p[4] << 8) | (size_t)p[5];
        size_t want = 12 + head_len + (size_t)msg_len;
        unsigned int tail = ((unsigned)p[n - 2] << 8) | (unsigned)p[n - 1];
        const unsigned char *msg = p + 10 + head_len;

        if (want != n) return 0;
        if (tail != (SIG_PACKET_MAGIC_TAIL & 0xFFFFu)) return 0;

        if (cmd == SIG_CMD_START_GAME_RSP) {
            long long code = 0;
            if (pb_first_varint_field(msg, (size_t)msg_len, 1, &code) != 0)
                code = 0;
            s->start_rsp_code = (int)code;
            ys_logf("sig: StartGameRsp retcode=%d", (int)code);
            if ((int)code == SIG_START_GAME_SUCC) {
                s->start_rsp_ok = 1;
                send_client_hello(s);
            } else {
                set_err(s, "StartGameRsp retcode=%d", (int)code);
                set_state(s, SIG_STATE_FAILED);
            }
            return 1;
        }

        ys_logf("sig: proxy packet cmd=%d len=%lu (ignored)", cmd, msg_len);
        return 1;
    }
}

sig_conn_t *sig_create(const sig_cfg_t *cfg)
{
    sig_conn_t *s;

    if (!cfg || !cfg->url || !cfg->url[0])
        return NULL;

    s = (sig_conn_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;

    copy_cfg_strings(s, cfg);

    if (s->cfg.connect_timeout_ms <= 0) s->cfg.connect_timeout_ms = 8000;
    if (s->cfg.heartbeat_ms <= 0) s->cfg.heartbeat_ms = 10000;
    if (s->cfg.stale_ms <= 0) s->cfg.stale_ms = 30000;

    if (parse_url(s->cfg_url, s->host, sizeof(s->host), &s->port,
                  s->path, sizeof(s->path)) != 0) {
        free(s);
        return NULL;
    }

    s->offer = (char *)malloc(SIG_OFFER_CAP);
    if (!s->offer) {
        free(s);
        return NULL;
    }
    s->offer[0] = 0;
    s->state = SIG_STATE_IDLE;
    s->vpt = -1;
    s->apt = -1;
    return s;
}

int sig_open(sig_conn_t *s)
{
    if (!s) return -1;
    if (s->wss || s->state == SIG_STATE_OPEN) {
        set_err(s, "sig_open: already open");
        return -1;
    }

    set_state(s, SIG_STATE_CONNECTING);

    s->wss = wss_connect(s->host, s->port, s->path, s->cfg.subprotocols,
                         s->cfg.connect_timeout_ms);
    if (!s->wss) {
        set_err(s, "wss_connect(%s:%d%s) failed: %s", s->host, s->port, s->path,
                wss_last_error());
        set_state(s, SIG_STATE_FAILED);
        return -1;
    }

    s->t_open_ms = now_ms();
    s->t_last_rx_ms = s->t_open_ms;
    s->t_last_hb_ms = s->t_open_ms;
    s->hello_sent = 0;
    s->start_rsp_ok = 0;
    s->start_rsp_code = 0;

    if (s->cfg.game_token && s->cfg.game_token[0]) {
        /* Real service: ask the pod to boot our game first. The client hello
         * goes out later, from handle_packet(), once StartGameRsp says OK. */
        if (send_start_game(s) != 0)
            return -1;
    } else {
        /* No token (mock server, or a caller that predates this exchange):
         * keep the old behaviour rather than hanging forever. */
        if (send_client_hello(s) != 0)
            return -1;
    }

    set_state(s, SIG_STATE_OPEN);
    return 0;
}

void sig_destroy(sig_conn_t *s)
{
    if (!s) return;
    if (s->wss) {
        wss_close(s->wss);
        s->wss = NULL;
    }
    free(s->offer);
    free(s);
}

int sig_close(sig_conn_t *s)
{
    if (!s) return -1;
    s->closing = 1;
    if (s->wss) {
        wss_close(s->wss);
        s->wss = NULL;
    }
    set_state(s, SIG_STATE_CLOSED);
    return 0;
}

/* ------------------------------------------------------------------ send */

int sig_send(sig_conn_t *s, int type, const void *payload, size_t len)
{
    unsigned char *buf;
    int n;

    if (!s || !s->wss) return -1;
    if (len > SIG_MAX_FRAME) {
        set_err(s, "sig_send: payload %zu too large", len);
        return -1;
    }

    buf = (unsigned char *)malloc(SIG_FRAME_HEADER + len);
    if (!buf) {
        set_err(s, "sig_send: out of memory");
        return -1;
    }
    n = sig_frame_pack(type, payload, len, buf, SIG_FRAME_HEADER + len);
    if (n < 0) {
        free(buf);
        set_err(s, "sig_send: pack failed");
        return -1;
    }
    if (wss_send_bin(s->wss, buf, (size_t)n) != 0) {
        free(buf);
        set_err(s, "sig_send: %s", wss_error(s->wss));
        return -1;
    }
    free(buf);
    return 0;
}

/* ----------------------------------------------------------------- frames */

int sig_frame_pack(int type, const void *payload, size_t len,
                   unsigned char *out, size_t cap)
{
    size_t need;
    if (!out) return -1;
    if (len && !payload) return -1;
    need = SIG_FRAME_HEADER + len;
    if (need > cap) return -1;

    out[0] = (unsigned char)((type >> 24) & 0xFF);
    out[1] = (unsigned char)((type >> 16) & 0xFF);
    out[2] = (unsigned char)((type >> 8) & 0xFF);
    out[3] = (unsigned char)(type & 0xFF);
    out[4] = (unsigned char)((len >> 24) & 0xFF);
    out[5] = (unsigned char)((len >> 16) & 0xFF);
    out[6] = (unsigned char)((len >> 8) & 0xFF);
    out[7] = (unsigned char)(len & 0xFF);
    if (len)
        memcpy(out + SIG_FRAME_HEADER, payload, len);
    return (int)need;
}

int sig_frame_parse(const unsigned char *buf, size_t len, int *type,
                    const unsigned char **payload, size_t *plen,
                    size_t *consumed)
{
    unsigned int t;
    unsigned long l;

    if (consumed) *consumed = 0;
    if (!buf) return -1;
    if (len < SIG_FRAME_HEADER) return 0;

    t = ((unsigned)buf[0] << 24) | ((unsigned)buf[1] << 16) |
        ((unsigned)buf[2] << 8) | (unsigned)buf[3];
    l = ((unsigned long)buf[4] << 24) | ((unsigned long)buf[5] << 16) |
        ((unsigned long)buf[6] << 8) | (unsigned long)buf[7];

    if (t < SIG_TYPE_SIGNALING || t > SIG_TYPE_KEEPALIVE) return -1;
    if (l > SIG_MAX_FRAME) return -1;
    if (len < SIG_FRAME_HEADER + (size_t)l) return 0;

    if (type) *type = (int)t;
    if (payload) *payload = buf + SIG_FRAME_HEADER;
    if (plen) *plen = (size_t)l;
    if (consumed) *consumed = SIG_FRAME_HEADER + (size_t)l;
    return 1;
}

/* --------------------------------------------------------------- candidates */

int sig_candidate_rewrite(const char *line, const char *ip, int port,
                          char *out, size_t cap)
{
    return sdp_candidate_rewrite(line, ip, port, out, cap);
}

int sig_send_candidate(sig_conn_t *s, const char *line)
{
    char *body, *cand;
    const char *src;
    int n;

    if (!s || !s->wss || !line) return -1;

    src = line;
    while (*src == ' ') src++;
    if (src[0] == 'a' && src[1] == '=') src += 2;

    body = (char *)malloc(strlen(src) * 6 + 256);
    cand = (char *)malloc(strlen(src) * 6 + 16);
    if (!body || !cand) {
        free(body);
        free(cand);
        set_err(s, "sig_send_candidate: out of memory");
        return -1;
    }
    if (json_escape(src, cand, strlen(src) * 6 + 16) < 0) {
        free(body);
        free(cand);
        set_err(s, "sig_send_candidate: escape failed");
        return -1;
    }
    n = snprintf(body, strlen(src) * 6 + 256,
                 "{\"candidate\":\"%s\",\"sdp-mid\":\"0\","
                 "\"sdp-mline-index\":0,\"type\":\"candidates\"}", cand);
    free(cand);
    if (n <= 0) {
        free(body);
        set_err(s, "sig_send_candidate: format failed");
        return -1;
    }
    n = sig_send(s, SIG_TYPE_SIGNALING, body, (size_t)n);
    free(body);
    return n;
}

/* ----------------------------------------------------------------- answer */

int sig_build_answer(sig_conn_t *s, char *out, size_t cap)
{
    sdp_offer_t *o;
    sdp_answer_cfg_t cfg;
    char err[256];
    int n;

    if (!s || !out || cap == 0) return -1;
    if (!s->offer_len) {
        set_err(s, "no offer received yet");
        return -1;
    }
    if (!s->cfg.fingerprint) {
        set_err(s, "no local fingerprint configured (set cfg.fingerprint)");
        return -1;
    }

    o = sdp_offer_parse(s->offer, err, sizeof(err));
    if (!o) {
        set_err(s, "offer parse failed: %s", err);
        return -1;
    }
    {
        /* The ICE layer runs outside this file, so remember the peer's
         * credentials while the parsed offer is still alive. */
        const char *ru = sdp_session_ice_ufrag(o);
        const char *rp = sdp_session_ice_pwd(o);
        snprintf(s->remote_ice_ufrag, sizeof(s->remote_ice_ufrag), "%s",
                 ru ? ru : "");
        snprintf(s->remote_ice_pwd, sizeof(s->remote_ice_pwd), "%s",
                 rp ? rp : "");
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.local_ip = s->cfg.local_ip;
    cfg.fingerprint = s->cfg.fingerprint;
    cfg.setup = "active";           /* we send the ClientHello */
    cfg.codecs = s->cfg.codecs ? s->cfg.codecs
                               : (SDP_CODEC_H264 | SDP_CODEC_OPUS);
    cfg.sendrecv = s->cfg.sendrecv;
    cfg.echo_extmaps = 1;
    cfg.accept_datachannel = s->cfg.accept_datachannel;
    cfg.candidate_ip = s->cfg.candidate_ip;
    cfg.candidate_port = s->cfg.candidate_port;
    cfg.local_cand_ip = s->cfg.local_cand_ip;
    cfg.local_cand_port = s->cfg.local_cand_port;
    cfg.ice_ufrag_out = s->local_ice_ufrag;
    cfg.ice_ufrag_out_cap = sizeof(s->local_ice_ufrag);
    cfg.ice_pwd_out = s->local_ice_pwd;
    cfg.ice_pwd_out_cap = sizeof(s->local_ice_pwd);

    n = sdp_build_answer(o, &cfg, out, cap, err, sizeof(err));
    if (n > 0) {
        /* The answer echoes the offer's payload type numbers, so record them
         * now: the RTP/SRTP layers need them and the offer may not be kept. */
        const sdp_media_t *v = sdp_media_first_kind(o, "video");
        const sdp_media_t *a = sdp_media_first_kind(o, "audio");
        int vmask = s->cfg.codecs ? s->cfg.codecs
                                  : (SDP_CODEC_H264 | SDP_CODEC_OPUS);

        s->vpt = (v && (vmask & SDP_CODEC_H264)) ? sdp_media_pt_for(v, SDP_CODEC_H264) : -1;
        if (s->vpt < 0 && v && (vmask & SDP_CODEC_H265))
            s->vpt = sdp_media_pt_for(v, SDP_CODEC_H265);
        s->apt = (a && (vmask & SDP_CODEC_OPUS)) ? sdp_media_pt_for(a, SDP_CODEC_OPUS) : -1;
    }
    sdp_offer_free(o);
    if (n < 0) {
        set_err(s, "answer build failed: %s", err);
        return -1;
    }
    return n;
}

int sig_send_answer(sig_conn_t *s, const char *answer)
{
    char *body;
    size_t cap;
    int n;

    if (!s || !s->wss || !answer) return -1;

    /* A caller may hand us either a raw SDP or an already formed JSON body. */
    if (answer[0] == '{') {
        if (sig_send(s, SIG_TYPE_SIGNALING, answer, strlen(answer)) != 0)
            return -1;
        set_state(s, SIG_STATE_ANSWERED);
        return 0;
    }

    cap = strlen(answer) * 6 + 128;
    body = (char *)malloc(cap);
    if (!body) {
        set_err(s, "sig_send_answer: out of memory");
        return -1;
    }
    if (json_escape(answer, body, cap) < 0) {
        free(body);
        set_err(s, "sig_send_answer: escape failed");
        return -1;
    }
    {
        char *msg = (char *)malloc(cap + 128);
        if (!msg) {
            free(body);
            set_err(s, "sig_send_answer: out of memory");
            return -1;
        }
        n = snprintf(msg, cap + 128, "{\"sdp\":\"%s\",\"type\":\"answer\"}",
                     body);
        free(body);
        if (n <= 0) {
            free(msg);
            set_err(s, "sig_send_answer: format failed");
            return -1;
        }
        n = sig_send(s, SIG_TYPE_SIGNALING, msg, (size_t)n);
        free(msg);
    }
    if (n != 0) return -1;
    set_state(s, SIG_STATE_ANSWERED);
    return 0;
}

int sig_answer_auto(sig_conn_t *s)
{
    char *ans;
    size_t cap = SIG_ANSWER_CAP;
    int n;

    if (!s) return -1;
    ans = (char *)malloc(cap);
    if (!ans) {
        set_err(s, "sig_answer_auto: out of memory");
        return -1;
    }
    n = sig_build_answer(s, ans, cap);
    if (n < 0) {
        free(ans);
        return -1;
    }
    n = sig_send_answer(s, ans);
    free(ans);
    return n;
}

/* --------------------------------------------------------------- incoming */

static void handle_offer(sig_conn_t *s, const char *json)
{
    int n = json_get_string(json, "sdp", s->offer, SIG_OFFER_CAP);
    if (n <= 0) {
        set_err(s, "offer frame without a usable \"sdp\" field");
        return;
    }
    s->offer_len = (size_t)n;
    set_state(s, SIG_STATE_OFFERED);
    if (s->cfg.cbs.on_offer)
        s->cfg.cbs.on_offer(s, s->offer, s->cfg.user);
}

static void handle_candidate(sig_conn_t *s, const char *json)
{
    char line[1024];
    char rewritten[1200];
    char mid[64];
    const char *emit;

    if (json_get_string(json, "candidate", line, sizeof(line)) <= 0) {
        set_err(s, "candidate frame without a \"candidate\" field");
        return;
    }
    if (json_get_string(json, "sdp-mid", mid, sizeof(mid)) < 0)
        mid[0] = 0;

    emit = line;
    if (s->cfg.candidate_ip && s->cfg.candidate_port > 0) {
        if (sig_candidate_rewrite(line, s->cfg.candidate_ip,
                                  s->cfg.candidate_port, rewritten,
                                  sizeof(rewritten)) == 0) {
            emit = rewritten;
        }
        /* If the rewrite fails the line is malformed; pass it through rather
         * than dropping a candidate silently. */
    }

    set_state(s, SIG_STATE_CONNECTED);
    if (s->cfg.cbs.on_candidate)
        s->cfg.cbs.on_candidate(s, emit, s->cfg.user);
}

static void handle_json(sig_conn_t *s, const char *json)
{
    char type[64];

    if (json_get_string(json, "type", type, sizeof(type)) < 0) {
        set_err(s, "JSON frame without a \"type\" field");
        return;
    }
    ys_logf("sig: json type=%s", type);
    {
        char peek[241];
        snprintf(peek, sizeof(peek), "%s", json);
        ys_logf("sig: json body: %s", peek);
    }

    if (strcmp(type, "offer") == 0) {
        handle_offer(s, json);
    } else if (strcmp(type, "candidates") == 0 ||
               strcmp(type, "candidate") == 0) {
        handle_candidate(s, json);
    } else if (strcmp(type, "answer") == 0) {
        /* We never send an offer, but a peer might echo one. Ignore. */
    } else if (strcmp(type, "error") == 0) {
        char msg[256];
        if (json_get_string(json, "message", msg, sizeof(msg)) < 0)
            snprintf(msg, sizeof(msg), "(no message)");
        set_err(s, "server error: %s", msg);
    }
    /* Unknown types are ignored on purpose: the service adds fields over
     * time and an unknown message must not kill the session. */
}

static void on_wss_msg(wss_t *h, int opcode, const unsigned char *data,
                       size_t len, void *user)
{
    sig_conn_t *s = (sig_conn_t *)user;
    (void)h;

    if (!s) return;

    if (opcode == WSS_OP_CLOSE) {
        set_state(s, SIG_STATE_CLOSED);
        return;
    }
    if (opcode != WSS_OP_TEXT && opcode != WSS_OP_BIN)
        return;                     /* PING/PONG are handled inside wss.c */

    s->t_last_rx_ms = now_ms();
    ys_logf("sig: rx op=%d len=%zu", opcode, len);

    /* A bare empty frame is the peer's keepalive echo: ignore it. */
    if (len == 0)
        return;

    {
        size_t off = 0;
        int frames = 0;
        while (off < len) {
            int type = 0;
            const unsigned char *pl = NULL;
            size_t plen = 0, used = 0;
            int rc = sig_frame_parse(data + off, len - off, &type, &pl,
                                     &plen, &used);
            if (rc == 0) {
                set_err(s, "truncated frame (%zu bytes buffered)", len - off);
                break;
            }
            if (rc < 0) {
                set_err(s, "malformed frame header at offset %zu", off);
                break;
            }
            off += used;
            frames++;
            s->frames_rx++;
            ys_logf("sig: frame #%d type=%d len=%zu", frames, type, plen);

            if (type == SIG_TYPE_SIGNALING || type == SIG_TYPE_HANDSHAKE) {
                /* Both are JSON; the payload is not NUL terminated. */
                char *j = (char *)malloc(plen + 1);
                if (!j) break;
                memcpy(j, pl, plen);
                j[plen] = 0;
                handle_json(s, j);
                free(j);
            } else if (type == SIG_TYPE_PROXY &&
                       handle_packet(s, pl, plen)) {
                /* Consumed: it was a Packet envelope we understand. */
            } else if (s->cfg.cbs.on_bytes) {
                s->cfg.cbs.on_bytes(s, type, pl, plen, s->cfg.user);
            }
        }
        (void)frames;
    }
}

/* ------------------------------------------------------------------- poll */

/* A bare empty WebSocket text frame is the keepalive; wss_send_text("")
 * emits exactly that (FIN|text, zero-length payload, masked). */
static void send_heartbeat(sig_conn_t *s)
{
    if (!s->wss) return;
    if (wss_send_text(s->wss, "") != 0) {
        set_err(s, "heartbeat send failed: %s", wss_error(s->wss));
        set_state(s, SIG_STATE_FAILED);
        return;
    }
    s->hb_sent++;
    s->t_last_hb_ms = now_ms();
}

int sig_next_timeout(const sig_conn_t *s)
{
    long long now, due_hb, due_stale, wait;
    if (!s || !s->wss) return -1;

    now = now_ms();
    due_hb = s->t_last_hb_ms + s->cfg.heartbeat_ms;

    if (s->state != SIG_STATE_STALE) {
        due_stale = s->t_last_rx_ms + s->cfg.stale_ms;
        if (due_stale < due_hb) due_hb = due_stale;
    }

    wait = due_hb - now;
    if (wait <= 0) return 0;
    if (wait > 1000) return 1000;   /* re-evaluate at least once a second */
    return (int)wait;
}

int sig_poll(sig_conn_t *s, int timeout_ms)
{
    int rc, tmo;
    long long now;

    if (!s) return -1;
    if (!s->wss || s->closing) return -1;
    if (s->poll_depth) {
        set_err(s, "sig_poll: re-entered from a callback");
        return -1;
    }

    /* Never block past the next heartbeat. */
    tmo = sig_next_timeout(s);
    if (tmo >= 0 && (timeout_ms < 0 || tmo < timeout_ms))
        timeout_ms = tmo;
    if (timeout_ms < 0) timeout_ms = 1000;

    s->poll_depth++;
    rc = wss_poll(s->wss, timeout_ms, on_wss_msg, s);
    s->poll_depth--;

    if (rc == -2) {
        set_state(s, SIG_STATE_CLOSED);
        return -1;
    }
    if (rc < 0) {
        set_err(s, "wss_poll: %s", wss_error(s->wss));
        set_state(s, SIG_STATE_FAILED);
        return -1;
    }

    now = now_ms();

    if (s->state != SIG_STATE_CLOSED && s->state != SIG_STATE_FAILED) {
        if (now - s->t_last_hb_ms >= s->cfg.heartbeat_ms)
            send_heartbeat(s);

        if (s->state != SIG_STATE_STALE &&
            now - s->t_last_rx_ms >= s->cfg.stale_ms)
            set_state(s, SIG_STATE_STALE);
    }

    return rc;
}

/* ---------------------------------------------------------------- queries */

int sig_state(const sig_conn_t *s)
{
    return s ? s->state : SIG_STATE_FAILED;
}

const char *sig_state_str(const sig_conn_t *s)
{
    return s ? sig_state_name(s->state) : "failed";
}

int sig_last_rx_age_ms(const sig_conn_t *s)
{
    if (!s) return -1;
    return (int)(now_ms() - s->t_last_rx_ms);
}

int sig_heartbeats_sent(const sig_conn_t *s)
{
    return s ? s->hb_sent : -1;
}

int sig_frames_rx(const sig_conn_t *s)
{
    return s ? s->frames_rx : -1;
}

int sig_video_payload_type(const sig_conn_t *s)
{
    return s ? s->vpt : -1;
}

int sig_audio_payload_type(const sig_conn_t *s)
{
    return s ? s->apt : -1;
}

const char *sig_current_offer(const sig_conn_t *s)
{
    return (s && s->offer_len) ? s->offer : "";
}

const char *sig_local_ice_ufrag(const sig_conn_t *s)
{
    return s ? s->local_ice_ufrag : "";
}

const char *sig_local_ice_pwd(const sig_conn_t *s)
{
    return s ? s->local_ice_pwd : "";
}

const char *sig_remote_ice_ufrag(const sig_conn_t *s)
{
    return s ? s->remote_ice_ufrag : "";
}

const char *sig_remote_ice_pwd(const sig_conn_t *s)
{
    return s ? s->remote_ice_pwd : "";
}

int sig_fd(const sig_conn_t *s)
{
    return (s && s->wss) ? wss_fd(s->wss) : -1;
}
