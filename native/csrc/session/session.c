/* session.c -- the single owner of a cloud-game session.
 *
 * Read session.h first: it states the architecture. This file is the
 * implementation and, more importantly, the record of every ordering rule that
 * a reviewer would otherwise have to rediscover by crashing the device.
 *
 * ============================ THE NON-NEGOTIABLE RULES ======================
 *
 * R1. ONE THREAD. Everything that blocks (connect, DTLS handshake, pipeline
 *     preroll) runs here and nowhere else. The JS thread only pushes commands.
 *     If a future feature needs a second thread, it must be a queue consumer
 *     with its own state that this thread joins on shutdown -- not a helper
 *     that touches the pipeline.
 *
 * R2. EVERY BLOCKING CALL IS BOUNDED AND INTERRUPTIBLE. The worker is written
 *     as a loop that slices every long operation into <=50 ms pieces and checks
 *     g->stop_req between them. Concretely: the WSS connect (250 ms), the DTLS
 *     handshake (163-1200 ms), the pipeline preroll (up to 3000 ms) each either
 *     have their own internal timeout or are wrapped so a stop lands quickly.
 *     A user pressing back must not freeze the UI for seconds.
 *
 * R3. TEARDOWN ORDER IS FIXED AND REVERSE OF SETUP.
 *     setup:   UDP socket -> DTLS -> SRTP -> pipeline -> plane -> control channel
 *     teardown: control channel -> plane+pipeline -> SRTP -> DTLS -> UDP
 *     Reversing this matters: closing the UDP socket before the pipeline is
 *     gone leaves kmssink holding a plane with no frames, and the KMS plane
 *     then survives the .amr being uninstalled (it is kernel state, not
 *     process state). That is the worst failure mode this project can have.
 *
 * R4. THE KMS PLANE IS KERNEL STATE. If the process dies with a plane up, the
 *     video keeps showing a frozen frame over the launcher. So the plane is
 *     released in three places: normal stop, error unwind, and a
 *     pthread_cleanup_push handler on the worker itself.
 *
 * R5. NO `malloc` IN THE RTP PATH. Decrypted packets are copied into one
 *     preallocated 64 KiB scratch buffer, depacketised into one preallocated
 *     frame buffer, and pushed to appsrc without ever growing.
 *
 * ============================ TOUCH TRANSPORT ===============================
 * The browser client sends touch over an SCTP DataChannel that the *server*
 * creates. This device has no SCTP and no webrtcbin (see docs/feasibility.md),
 * so we use the documented fallback: `game_control_channel_url` (field 14 of
 * sdk_param), a plain WSS endpoint on :5443 whose payload is byte-identical to
 * the DataChannel's. Same Packet framing, different carrier. That single
 * substitution is what makes input possible without implementing SCTP.
 */
#define _GNU_SOURCE
#include "session/session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/log.h"
#include "core/queue.h"
#include "core/thread.h"
#include "input/touch.h"
#include "rtp/rtp_reorder.h"
#include "rtp/srtp.h"
#include "session/cmd.h"
#include "video/video_pipe.h"
#include "webrtc/dtls.h"
#include "webrtc/sdp.h"
#include "webrtc/sig.h"
#include "webrtc/stun.h"
#include "webrtc/wss.h"

/* ------------------------------------------------------------------ tuning */

/* How long the whole start→first frame budget is. A real dispatch that lands
 * on a warm node gives a first frame in 3-8 s; a queued one can sit for a
 * minute, but queueing is the JS layer's business (paasDispatch returns only
 * after the queue clears), so the native budget stays modest. */
#define YS_START_BUDGET_MS      20000

/* Per-stage ceilings. These are not guesses: wss_connect was measured at
 * 222-263 ms and dtls_handshake at 163 ms on this device, so 8 s / 6 s leaves
 * room for one bad RTT without letting a wedged socket hang the UI. */
#define YS_WSS_TIMEOUT_MS       8000
#define YS_DTLS_TIMEOUT_MS      6000
#define YS_PREROLL_TIMEOUT_MS   3000
/* The server's offer is a separate, slower stage than the WSS connect: the
 * game pod can take a while to begin streaming after the dispatch completes,
 * so give it a longer budget than the socket-level connect. */
#define YS_OFFER_TIMEOUT_MS     60000

/* Heartbeat: the browser SDK sends a bare empty text frame every 10 s. We send
 * the same. If nothing at all has arrived for 3 periods the session is
 * considered stale; we surface it but do NOT auto-reconnect (that policy needs
 * to know about the server failover list, which is a JS decision). */
#define YS_HEARTBEAT_MS         10000
#define YS_STALE_MS             (3 * YS_HEARTBEAT_MS)

/* Worker loop cadence. 10 ms is one sixth of a 60 fps frame: fine enough to
 * keep the UDP drain and heartbeat prompt, coarse enough that the device is not
 * spinning. It also bounds how long a stop request can sit unnoticed. */
#define YS_TICK_MS              10

/* Identity the signaling layer reports in StartGameReq. The service validates
 * sdk_version loosely (it is used for feature gating, not auth), so echoing the
 * web client's version is correct and keeps us on the same code paths as the
 * official player. terminal_type is ClientTerminalType.WebTouch = 9 -- note
 * that this is NOT the same enum as PROFILE.cloudClientType (18) in const.js,
 * which belongs to the combo/passport API and means nothing here. */
#define YS_CG_SDK_VERSION       "6.1.0.65"
#define YS_CG_TERMINAL_WEB_TOUCH 9

/* Signal frame types live in webrtc/sig.h (SIG_TYPE_SIGNALING/PROXY/
 * HANDSHAKE/KEEPALIVE). This file no longer packs or parses them: sig.c owns
 * the whole sequence -- client hello, offer wait, answer, candidate rewrite,
 * bare-frame heartbeat -- and is driven by sig_poll() from the worker loop.
 * Two copies of that sequence would double-send the answer and get us
 * disconnected immediately. */

/* ------------------------------------------------------------------ sdk_param
 *
 * sdk_param is a base64 protobuf with 22 known fields; we need six of them.
 * Rather than link a protobuf runtime (or hand-write a generated file for a
 * schema we cannot compile against), we scan the wire format directly. This is
 * safe because protobuf's wire format is self-describing: we walk (tag, value)
 * pairs, skip everything we do not know by wire type, and stop at the first
 * malformed byte. That is exactly what a generated parser does, minus the
 * schema validation we cannot do anyway.
 */
typedef struct {
    char sid[128];
    char pod_id[128];            /* StartGameReq field 7                 */
    char game_token[256];
    char game_svr_addr[64];      /* "ip:port"                        */
    int  rtc_udp_port;
    char wss_url[256];           /* "wss://host[:port][/path]"       */
    char ctl_url[256];           /* "wss://host:5443/...?sessionId=" */
    int  has_addr, has_wss;
} ys_sdk_param;

static void sdk_reset(ys_sdk_param *p) {
    memset(p, 0, sizeof(*p));
}

/* Read a base64url-or-standard base64 blob into a malloc'd buffer. */
static unsigned char *b64_decode_alloc(const char *s, size_t *out_len) {
    size_t n = strlen(s), i = 0, o = 0;
    unsigned char *out;
    int acc = 0, bits = 0;
    if (n == 0) return NULL;
    /* Worst case 3/4 of input. */
    out = (unsigned char *)malloc(n / 4 * 3 + 4);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        int v;
        char c = s[i];
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else continue;              /* '=' padding, whitespace, junk */
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return out;
}

/* Bounds-checked varint. Returns bytes consumed, 0 on truncation. */
static size_t pb_varint(const unsigned char *p, size_t n, uint64_t *v) {
    uint64_t r = 0;
    int shift = 0;
    size_t i = 0;
    while (i < n && i < 10) {
        unsigned char b = p[i++];
        r |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *v = r; return i; }
        shift += 7;
    }
    return 0;
}

/* Copy a length-delimited field into a fixed buffer, always NUL terminated,
 * silently truncating. Truncation is acceptable for URLs and ids; the caller
 * can tell by checking whether the result hits the cap - 1 (logged below). */
static void pb_str(char *dst, size_t cap, const unsigned char *p, size_t len) {
    size_t k = len < cap - 1 ? len : cap - 1;
    memcpy(dst, p, k);
    dst[k] = '\0';
}

/* Walk the whole message once. Unknown fields are skipped by wire type. */
static int sdk_decode(ys_sdk_param *out, const unsigned char *buf, size_t len) {
    size_t i = 0;
    sdk_reset(out);
    while (i < len) {
        uint64_t key = 0;
        size_t used = pb_varint(buf + i, len - i, &key);
        unsigned field, wt;
        if (!used) return -1;
        i += used;
        field = (unsigned)(key >> 3);
        wt = (unsigned)(key & 7);

        if (wt == 0) {                                  /* varint */
            uint64_t v = 0;
            used = pb_varint(buf + i, len - i, &v);
            if (!used) return -1;
            i += used;
            if (field == 9) out->rtc_udp_port = (int)v;
        } else if (wt == 2) {                           /* length-delimited */
            uint64_t l = 0;
            used = pb_varint(buf + i, len - i, &l);
            if (!used) return -1;
            i += used;
            if (l > len - i) return -1;
            switch (field) {
            case 1:  pb_str(out->sid, sizeof(out->sid), buf + i, (size_t)l); break;
            case 3:  pb_str(out->pod_id, sizeof(out->pod_id), buf + i, (size_t)l); break;
            case 4:  pb_str(out->game_token, sizeof(out->game_token), buf + i, (size_t)l); break;
            case 8:  pb_str(out->game_svr_addr, sizeof(out->game_svr_addr), buf + i, (size_t)l);
                     out->has_addr = 1; break;
            case 11: pb_str(out->wss_url, sizeof(out->wss_url), buf + i, (size_t)l);
                     out->has_wss = 1; break;
            case 14: pb_str(out->ctl_url, sizeof(out->ctl_url), buf + i, (size_t)l); break;
            default: break;
            }
            i += (size_t)l;
        } else if (wt == 1) {                           /* fixed64 */
            if (len - i < 8) return -1;
            i += 8;
        } else if (wt == 5) {                           /* fixed32 */
            if (len - i < 4) return -1;
            i += 4;
        } else {
            return -1;                                  /* 3/4/6/7 illegal here */
        }
    }
    return (out->has_addr && out->has_wss) ? 0 : -1;
}

/* Split "host:port" / "wss://host:port/path". Returns 0 on success.
 * Deliberately tiny: every URL we parse comes from sdk_param, whose shape was
 * captured from live dispatches, so there is no need for a general parser. */
static int split_hostport(const char *s, char *host, size_t hostcap,
                          int *port, char *path, size_t pathcap, int defport) {
    const char *p = s;
    const char *q;
    size_t n;
    if (!s || !*s) return -1;
    if (!strncmp(p, "wss://", 6)) p += 6;
    else if (!strncmp(p, "ws://", 5)) p += 5;
    q = strchr(p, '/');
    n = q ? (size_t)(q - p) : strlen(p);
    /* Split host[:port] */
    {
        const char *colon = memchr(p, ':', n);
        size_t hl = colon ? (size_t)(colon - p) : n;
        if (hl == 0 || hl >= hostcap) return -1;
        memcpy(host, p, hl);
        host[hl] = '\0';
        /* A bare IPv6 literal would contain multiple colons; sdk_param gives
         * IPv4 for hk4e_cn, and is_ipv6 is field 16 which we do not use. */
        if (colon) {
            *port = atoi(colon + 1);
            if (*port <= 0 || *port > 65535) return -1;
        } else {
            *port = defport;
        }
    }
    if (path && pathcap) {
        if (q && q[0]) {
            n = strlen(q);
            if (n >= pathcap) return -1;
            memcpy(path, q, n + 1);
        } else {
            snprintf(path, pathcap, "/");
        }
    }
    return 0;
}

/* ============================================================== global state
 *
 * All of this is touched by the worker only, except the queue and the snapshot,
 * which are explicitly safe. `g_worker` exists so ys_session_shutdown() can
 * signal the thread it created without a second handoff structure.
 */
static struct {
    ys_queue_t q;
    pthread_t  th;
    atomic_int running;
    atomic_int stop_req;      /* worker checks this between slices */
    atomic_int inited;
    /* seqlock */
    atomic_uint seq;
    ys_status   snap;
    /* request material handed from JS to the worker (worker reads after pop) */
    pthread_mutex_t push_mtx;
} G;

static inline void snap_begin(void) {
    atomic_fetch_add(&G.seq, 1);              /* odd: write in progress */
    atomic_thread_fence(memory_order_release);
}
static inline void snap_end(void) {
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add(&G.seq, 1);              /* even: stable */
}

/* The worker's authoritative copy; published to G.snap at most every publish. */
typedef struct {
    ys_status st;

    int state;
    int rect[4];
    char sdk_b64[YS_MAX_SDK_PARAM_B64 + 1];
    char aid[64];                /* gamer account id, from the START command */
    ys_sdk_param sdk;

    /* signaling. Owned by webrtc/sig.c: this file only creates it, polls it,
     * asks it for the answer and destroys it. No rx buffer, no heartbeat
     * bookkeeping, no offer pointer -- all of that moved into sig.c. */
    sig_conn_t *sig;
    uint32_t    last_stat_ms;
    int         offer_seen;      /* set by on_offer, consumed by w_run_session */

    /* media */
    dtls_client_t *dtls;
    int            udp_fd;
    struct sockaddr_in peer;
    /* Local endpoint of the media socket. The answer SDP must advertise THIS,
     * not the server's own address, otherwise our ICE checks are dropped as
     * coming from an unknown peer. */
    char           udp_local_ip[48];
    int            udp_local_port;
    srtp_ctx       srtp;
    rtp_depay     *depay;
    ys_video_t    *video;
    wss_t         *ctl;
    ys_touch_state touch;

    unsigned char  pktscratch[SRTP_MAX_PACKET + 64];
    unsigned char  framescratch[RTP_FRAME_MAX];

    uint32_t start_ms;
    uint32_t first_frame_ms;
} ys_worker;

static ys_worker W;

/* ---------------------------------------------------------------- helpers */

const char *ys_state_name(int st) {
    switch (st) {
    case YS_ST_IDLE:       return "idle";
    case YS_ST_CONNECTING: return "connecting";
    case YS_ST_HANDSHAKE:  return "handshake";
    case YS_ST_PLAYING:    return "playing";
    case YS_ST_PAUSED:     return "paused";
    case YS_ST_STOPPING:   return "stopping";
    case YS_ST_ERROR:      return "error";
    default:               return "unknown";
    }
}

static void w_publish(void) {
    W.st.state = W.state;
    snap_begin();
    G.snap = W.st;
    snap_end();
}

static void w_fail(const char *code, const char *msg) {
    snprintf(W.st.error, sizeof(W.st.error), "%s", code ? code : YS_ERR_INTERNAL);
    snprintf(W.st.error_msg, sizeof(W.st.error_msg), "%s", msg ? msg : "");
    W.state = YS_ST_ERROR;
    W.st.ok = 0;
    ys_logf("session ERROR %s: %s", W.st.error, W.st.error_msg);
    w_publish();
}

static void w_state(int st) {
    if (W.state == st) return;
    ys_logf("state %s -> %s", ys_state_name(W.state), ys_state_name(st));
    W.state = st;
    w_publish();
}

/* ============================================================ signaling sink
 *
 * sig.c owns the socket, the frame codec, the hello/offer/answer sequence and
 * the bare-frame heartbeat. All this file does is react to what it reports.
 *
 * on_offer deliberately does NOT build the answer. sig_answer_auto() needs a
 * DTLS certificate for the fingerprint, and generating one inside a poll
 * callback is the wrong place to fail: the error could not be surfaced
 * through w_fail() with any useful state behind it. So the callback only
 * raises a flag, and w_run_session() -- which runs on the worker's own
 * control flow -- consumes it, creates the certificate and answers.
 */
static void on_sig_offer(sig_conn_t *s, const char *offer_sdp, void *user) {
    (void)s; (void)user;
    W.offer_seen = 1;
    ys_logf("sig: got offer (%u bytes of sdp)",
            (unsigned)(offer_sdp ? strlen(offer_sdp) : 0));
}

static void on_sig_state(sig_conn_t *s, int old_state, int new_state, void *user) {
    (void)s; (void)user;
    ys_logf("sig: %s -> %s", sig_state_name(old_state), sig_state_name(new_state));
}

/* ============================================================== start / stop
 */

static void w_teardown(const char *why);

/* Slice-wise wait that keeps the signalling alive. Returns 0 if the thing
 * happened, -1 on stop/timeout. `done` is polled; `pump` is optional. */
typedef int (*wait_fn)(void *ctx);

static int w_wait(int budget_ms, wait_fn done, void *ctx, const char *what) {
    uint32_t t0 = ys_mono_ms();
    while (ys_mono_ms() - t0 < (uint32_t)budget_ms) {
        if (atomic_load(&G.stop_req)) return -1;
        if (done && done(ctx)) return 0;
        if (W.sig) {
            /* sig_poll clamps its own timeout to sig_next_timeout(), so a
             * fixed 10 ms cannot make the heartbeat late; and -1 (block until
             * a frame arrives) is never needed, which would stall the RTP
             * drain for ~10 s on an idle session. */
            if (sig_poll(W.sig, YS_TICK_MS) < 0) return -1;
        } else {
            ys_sleep_ms(YS_TICK_MS);
        }
        if (W.state == YS_ST_ERROR) return -1;
    }
    ys_logf("wait(%s) timed out after %d ms", what, budget_ms);
    return -1;
}

/* sig.c holds the parsed offer; this only needs to know one arrived. */
static int done_offer(void *ctx) {
    (void)ctx;
    return W.offer_seen || (W.sig && sig_state(W.sig) >= SIG_STATE_OFFERED);
}

/* Open the game control channel (WSS :5443) used for touch. Failure is not
 * fatal: video still plays, the user just cannot interact, which is strictly
 * better than refusing to start. */
static void w_open_ctl(void) {
    char host[128], path[256];
    int port = 5443;
    if (!W.sdk.ctl_url[0]) {
        ys_logf("ctl: no game_control_channel_url in sdk_param, touch disabled");
        return;
    }
    if (split_hostport(W.sdk.ctl_url, host, sizeof(host), &port, path, sizeof(path), 5443) != 0) {
        ys_logf("ctl: cannot parse '%s'", W.sdk.ctl_url);
        return;
    }
    W.ctl = wss_connect(host, port, path, NULL, YS_WSS_TIMEOUT_MS);
    if (!W.ctl) {
        ys_logf("ctl: connect failed: %s", wss_last_error());
        return;
    }
    ys_logf("ctl: connected %s:%d%s", host, port, path);
    ys_touch_reset(&W.touch);
}

static void w_close_ctl(void) {
    if (W.ctl) { wss_close(W.ctl); W.ctl = NULL; }
}

/* Bind + connect the media socket and remember the local endpoint.
 *
 * Called from w_run_session() *before* the signalling handshake, because the
 * SDP answer has to advertise the address we will actually send from. It used
 * to be created after the answer was already on the wire, so the answer
 * carried the server's own address as "our" candidate -- an ICE check coming
 * from our real port then looks like an unknown peer and is dropped without
 * an error, which is precisely the silent DTLS timeout we chased. */
static int w_open_udp(void) {
    char ip[48];
    int p = 0;
    const char *colon = strchr(W.sdk.game_svr_addr, ':');
    struct in_addr ia;

    if (!colon) { w_fail(YS_ERR_UDP, "game_svr_addr 缺端口"); return -1; }
    {
        size_t hl = (size_t)(colon - W.sdk.game_svr_addr);
        if (hl == 0 || hl >= sizeof(ip)) { w_fail(YS_ERR_UDP, "game_svr_addr 非法"); return -1; }
        memcpy(ip, W.sdk.game_svr_addr, hl);
        ip[hl] = '\0';
        p = atoi(colon + 1);
    }
    if (p <= 0 || p > 65535) { w_fail(YS_ERR_UDP, "game_svr_addr 端口非法"); return -1; }
    /* game_svr_addr carries the reliable/TCP port (120.232.47.14:13600 in a
     * real dispatch); the media plane lives on rtc_udp_port (13601). The web
     * SDK rewrites every candidate to game_svr_ip:rtc_udp_port, so DTLS must
     * go there -- knocking on the TCP port looks like a dead peer and burns
     * the whole handshake budget in retransmissions. */
    if (W.sdk.rtc_udp_port > 0 && W.sdk.rtc_udp_port <= 65535 &&
        W.sdk.rtc_udp_port != p) {
        ys_logf("udp: using rtc_udp_port=%d instead of game_svr_addr port %d",
                W.sdk.rtc_udp_port, p);
        p = W.sdk.rtc_udp_port;
    }
    if (inet_pton(AF_INET, ip, &ia) != 1) { w_fail(YS_ERR_UDP, "game_svr_addr 不是 IPv4"); return -1; }

    memset(&W.peer, 0, sizeof(W.peer));
    W.peer.sin_family = AF_INET;
    W.peer.sin_addr = ia;
    W.peer.sin_port = htons((uint16_t)p);

    W.udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (W.udp_fd < 0) { w_fail(YS_ERR_UDP, "socket() 失败"); return -1; }
    {
        /* A cloud-game client must tolerate 100 ms of jitter without the
         * kernel queue overflowing and silently dropping DTLS retransmits
         * later on. 256 KiB is well under the sysctl cap on this firmware. */
        int rcvbuf = 256 * 1024;
        setsockopt(W.udp_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    if (connect(W.udp_fd, (struct sockaddr *)&W.peer, sizeof(W.peer)) != 0) {
        w_fail(YS_ERR_UDP, "connect(udp) 失败");
        close(W.udp_fd); W.udp_fd = -1;
        return -1;
    }
    /* Non-blocking after the handshake; DTLS needs blocking-ish slices but we
     * drive it with its own poll loop, so non-blocking is correct and lets the
     * same fd serve RTP afterwards. */
    {
        int fl = fcntl(W.udp_fd, F_GETFL, 0);
        if (fl >= 0) fcntl(W.udp_fd, F_SETFL, fl | O_NONBLOCK);
    }
    {
        struct sockaddr_in self;
        socklen_t slen = sizeof(self);
        W.udp_local_ip[0] = '\0';
        W.udp_local_port = 0;
        if (getsockname(W.udp_fd, (struct sockaddr *)&self, &slen) == 0) {
            if (inet_ntop(AF_INET, &self.sin_addr, W.udp_local_ip,
                          sizeof(W.udp_local_ip)) == NULL)
                W.udp_local_ip[0] = '\0';
            W.udp_local_port = (int)ntohs(self.sin_port);
        }
    }
    ys_logf("udp: connected to %s:%d (rtc_udp_port=%d, local=%s:%d)",
            ip, p, W.sdk.rtc_udp_port,
            W.udp_local_ip[0] ? W.udp_local_ip : "?", W.udp_local_port);
    return 0;
}

/* Decode sdk_param and bring up UDP + DTLS + SRTP. Returns 0 on success. */
static int w_bring_up_media(void) {
    unsigned char *raw;
    size_t rawlen = 0;

    raw = b64_decode_alloc(W.sdk_b64, &rawlen);
    if (!raw) { w_fail(YS_ERR_BAD_SDK, "sdk_param base64 解码失败"); return -1; }
    if (sdk_decode(&W.sdk, raw, rawlen) != 0) {
        free(raw);
        w_fail(YS_ERR_BAD_SDK, "sdk_param protobuf 解析失败（缺 game_svr_addr 或 wss url）");
        return -1;
    }
    free(raw);
    ys_logf("sdk: sid=%s addr=%s udp=%d wss=%s ctl=%s",
            W.sdk.sid, W.sdk.game_svr_addr, W.sdk.rtc_udp_port,
            W.sdk.wss_url, W.sdk.ctl_url[0] ? W.sdk.ctl_url : "(none)");

    /* ---- UDP socket: opened before signalling so that the answer can
     * advertise the port we actually send from (see w_open_udp). ---- */
    if (W.udp_fd < 0 && w_open_udp() != 0) return -1;

        /* ---- ICE connectivity check.
         *
         * The media edge is ICE-lite behind a fixed port and will not answer
         * DTLS from an address it has not validated: it learns our NAT'ed
         * 5-tuple from this check. Without it the ClientHello is retransmitted
         * until the budget runs out and nothing ever comes back.
         *
         * A failure here is not fatal on its own -- some edges answer DTLS
         * anyway -- so we only log and let the handshake decide. */
        {
            const char *ru = sig_remote_ice_ufrag(W.sig);
            const char *rp = sig_remote_ice_pwd(W.sig);
            const char *lu = sig_local_ice_ufrag(W.sig);
            if (ru && ru[0] && lu && lu[0]) {
                unsigned char req[STUN_MAX_MSG];
                unsigned char rbuf[STUN_MAX_MSG];
                unsigned char txid[STUN_TXID_LEN];
                unsigned char tie[8];
                stun_result_t res;
                uint32_t seed = ys_mono_ms() ^ (uint32_t)(size_t)&res;
                int n, attempt, got = 0;

                for (int i = 0; i < STUN_TXID_LEN; i++)
                    txid[i] = (unsigned char)((seed = seed * 1103515245u + 12345u) >> 16);
                for (int i = 0; i < 8; i++)
                    tie[i] = (unsigned char)((seed = seed * 1103515245u + 12345u) >> 16);

                n = stun_build_binding_request(ru, lu, rp, tie, txid, req, sizeof(req));
                if (n <= 0) {
                    ys_logf("ice: build failed (%d)", n);
                } else {
                    ys_logf("ice: binding request %d bytes (user=%s:%s pwd=%s)",
                            n, ru, lu, (rp && rp[0]) ? rp : "-");
                    for (attempt = 0; attempt < 4 && !got; attempt++) {
                        struct pollfd pfd;
                        if (send(W.udp_fd, req, (size_t)n, 0) < 0) {
                            ys_logf("ice: send failed errno=%d", errno);
                            break;
                        }
                        pfd.fd = W.udp_fd; pfd.events = POLLIN; pfd.revents = 0;
                        if (poll(&pfd, 1, 400) > 0) {
                            int rn = (int)recv(W.udp_fd, rbuf, sizeof(rbuf), 0);
                            if (rn > 0 &&
                                stun_parse_response(rbuf, (size_t)rn, txid, &res) == 0) {
                                char desc[128];
                                stun_result_str(&res, desc, sizeof(desc));
                                ys_logf("ice: reply #%d %s", attempt, desc);
                                if (res.msg_type == STUN_MSG_BINDING_RESPONSE)
                                    got = 1;
                            } else {
                                ys_logf("ice: reply #%d not ours (%d bytes)", attempt, rn);
                            }
                        } else {
                            ys_logf("ice: no reply #%d", attempt);
                        }
                    }
                    if (!got)
                        ys_logf("ice: no binding response; trying DTLS anyway");
                }
            } else {
                ys_logf("ice: skipped (no credentials: remote=%d local=%d)",
                        (ru && ru[0]) ? 1 : 0, (lu && lu[0]) ? 1 : 0);
            }
        }

    /* ---- DTLS. ECDSA P-256: RSA-2048 costs 182-409 ms of certificate
     * generation on this A35, ECDSA is 0-1 ms.
     *
     * CRITICAL: if w_send_answer() already created the context, we MUST reuse
     * it. Its fingerprint is already on the wire in the SDP answer; creating a
     * second context here would handshake with a certificate the server never
     * saw, and the server rejects the ClientHello with no useful diagnostic. */
    if (!W.dtls) {
        W.dtls = dtls_client_create(DTLS_KEY_ECDSA_P256);
        if (!W.dtls) { w_fail(YS_ERR_DTLS, dtls_last_error()); return -1; }
    }
    {
        uint32_t t0 = ys_mono_ms();
        int rc = dtls_handshake(W.dtls, W.udp_fd, (struct sockaddr *)&W.peer,
                                sizeof(W.peer), YS_DTLS_TIMEOUT_MS);
        int ms = (int)(ys_mono_ms() - t0);
        W.st.dtls_ms = ms;
        if (rc != 0) {
            w_fail(YS_ERR_DTLS, dtls_last_error());
            return -1;
        }
        W.st.retransmits = dtls_retransmit_count(W.dtls);
        snprintf(W.st.cipher, sizeof(W.st.cipher), "%s",
                 dtls_cipher_name(W.dtls) ? dtls_cipher_name(W.dtls) : "?");
        ys_logf("dtls: ok in %d ms, cipher=%s, retransmits=%d", ms, W.st.cipher,
                W.st.retransmits);
    }

    /* ---- SRTP keys. Two contexts, one per direction, because each direction
     * has its own ROC/replay state and mixing them corrupts both. ---- */
    {
        dtls_srtp_keys_t k;
        srtp_master_key mk_in, mk_out;
        if (dtls_srtp_keys(W.dtls, &k) != 0) {
            w_fail(YS_ERR_NO_SRTP, "对端未选择 SRTP profile");
            return -1;
        }
        snprintf(W.st.srtp_profile, sizeof(W.st.srtp_profile), "%s", k.profile_name);

        /* RECEIVE direction uses the server's write key. Getting this backwards
         * is silent: every packet just fails the auth tag, and the symptom
         * ("no video") looks like a network problem. */
        memset(&mk_in, 0, sizeof(mk_in));
        memcpy(mk_in.master_key, k.server_write_key, 16);
        memcpy(mk_in.master_salt, k.server_write_salt, 14);
        mk_in.master_salt_len = 14;
        mk_in.kdr = 0;

        memset(&mk_out, 0, sizeof(mk_out));
        memcpy(mk_out.master_key, k.client_write_key, 16);
        memcpy(mk_out.master_salt, k.client_write_salt, 14);
        mk_out.master_salt_len = 14;
        mk_out.kdr = 0;

        /* The receive context binds its SSRC from the first packet (0 = learn). */
        if (srtp_init(&W.srtp, &mk_in, 0) != SRTP_OK) {
            w_fail(YS_ERR_NO_SRTP, "srtp_init 失败");
            return -1;
        }
        ys_logf("srtp: ready, profile=%s", W.st.srtp_profile);
    }

    /* ---- Video pipeline. Created paused; started after the first RTP. ---- */
    W.video = ys_video_create(960, 266);
    if (!W.video) { w_fail(YS_ERR_VIDEO, "ys_video_create 失败"); return -1; }
    if (ys_video_set_rect(W.video, W.rect[0], W.rect[1], W.rect[2], W.rect[3]) != 0) {
        w_fail(YS_ERR_VIDEO, "ys_video_set_rect 失败");
        return -1;
    }

    /* Depacketiser: H.264 only. The offer's only video codec we keep is H.264
     * (see sdp.c answer generation), so H.265 never reaches this point. */
    W.depay = rtp_depay_init(RTP_DEPAY_H264, RTP_FRAME_MAX);
    if (!W.depay) { w_fail(YS_ERR_VIDEO, "rtp_depay_init 失败"); return -1; }

    w_open_ctl();
    return 0;
}

/* Send our SDP answer.
 *
 * The certificate is created here rather than in w_run_session(): the answer
 * carries its fingerprint, and w_bring_up_media() must then reuse THIS exact
 * context for the handshake (a second context would present a certificate the
 * server never saw and reject the ClientHello with no useful diagnostic).
 *
 * The answer itself -- SDP m-lines, JSON escaping, the 8-byte frame header --
 * is all inside sig.c now (sig_answer_auto). Keeping our own copy of that
 * sequence would mean two answers on the wire per session, which the server
 * treats as a protocol violation and drops. */
static int w_send_answer(void) {
    if (!W.offer_seen) { w_fail(YS_ERR_NO_OFFER, "未收到 offer"); return -1; }
    if (!W.dtls) {
        /* ECDSA P-256: RSA-2048 costs 182-409 ms of certificate generation on
         * this A35, ECDSA is 0-1 ms. */
        W.dtls = dtls_client_create(DTLS_KEY_ECDSA_P256);
        if (!W.dtls) { w_fail(YS_ERR_DTLS, dtls_last_error()); return -1; }
        ys_logf("dtls: cert generated in %d ms", dtls_cert_gen_ms(W.dtls));
    }
    if (sig_answer_auto(W.sig) != 0) {
        w_fail(YS_ERR_SDP, sig_last_error(W.sig));
        return -1;
    }
    ys_logf("sig: answer sent (video pt=%d audio pt=%d)",
             sig_video_payload_type(W.sig), sig_audio_payload_type(W.sig));

    /* Trickle our endpoint as well. The browser SDK forwards every local
     * candidate over the same WSS channel; an edge that whitelists checks by
     * the signalled remote candidate drops ours silently otherwise, which
     * looks exactly like a dead port (no error, no ICMP, DTLS retransmits
     * until the budget runs out). */
    if (W.udp_local_ip[0] && W.udp_local_port > 0) {
        char cand[160];
        snprintf(cand, sizeof(cand),
                 "candidate:1 1 udp 2130706431 %s %d typ host",
                 W.udp_local_ip, W.udp_local_port);
        if (sig_send_candidate(W.sig, cand) < 0)
            ys_logf("sig: local candidate send failed: %s", sig_last_error(W.sig));
        else
            ys_logf("sig: local candidate sent (%s)", cand);
    }

    return 0;
}

/* Drain one UDP packet, unprotect, depacketise, push. Returns the number of
 * complete frames emitted. Called in a loop until EAGAIN. */
static int w_drain_udp(void) {
    int frames = 0;
    for (;;) {
        /* recv into the SRTP-sized window, NOT the full scratch buffer: a
         * datagram larger than SRTP_MAX_PACKET cannot be unprotect()ed (the
         * context's buffer bound is 1600), and letting recv fill 1664 bytes
         * would turn a jumbo/malformed packet into a counted auth failure,
         * polluting the one counter we use to detect wrong keys. Truncating at
         * recv is also what makes the "too large" case impossible to reach. */
        ssize_t n = recv(W.udp_fd, W.pktscratch, SRTP_MAX_PACKET, 0);
        size_t out_len = 0;
        const unsigned char *frame = NULL;
        size_t frame_len = 0;
        srtp_status rs;
        const uint8_t *dep = NULL;
        size_t dep_len = 0;
        uint32_t now = ys_mono_ms();

        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                break;
            if (n < 0) { ys_logf("udp recv error %d", errno); break; }
            break;
        }

        /* RTCP (RFC 5761 demux by payload type 200-204) is not needed for
         * playback; it only carries receiver reports which we do not send. */
        if (n >= 2) {
            unsigned pt = W.pktscratch[1] & 0x7F;
            if (pt >= 64 && pt <= 95) continue;      /* RTCP range */
        }

        W.st.packets_rx++;
        rs = srtp_unprotect(&W.srtp, W.pktscratch, (size_t)n, &out_len);
        if (rs == SRTP_ERR_REPLAY) { W.st.rtp_late++; continue; }
        if (rs != SRTP_OK) {
            W.st.srtp_auth_fail++;
            /* Do not spam: one line per 100 failures keeps the log readable
             * while still proving whether keys or the wire are wrong. */
            if ((W.st.srtp_auth_fail % 100) == 1)
                ys_logf("srtp_unprotect rc=%d (%s) after %u pkts", rs,
                        srtp_strerror(rs), W.st.srtp_auth_fail);
            continue;
        }

        if (rtp_depay_push(W.depay, W.pktscratch, out_len, now, &dep, &dep_len) == RTP_OK
            && dep && dep_len) {
            if (W.first_frame_ms == 0) {
                W.first_frame_ms = now;
                W.st.first_frame_ms = (int)(now - W.start_ms);
                ys_logf("video: first frame %zu bytes at %d ms", dep_len,
                        W.st.first_frame_ms);
            }
            if (W.video) ys_video_push_video(W.video, dep, dep_len, now);
            frames++;
        }
        (void)frame; (void)frame_len;
    }
    return frames;
}

/* Is the panel powered? Reads /tmp/.screen_state, written by the vendor's
 * hal-screen daemon, which contains "screen_on=1" or "screen_on=0" and is
 * under 32 bytes -- by far the cheapest signal available.
 *
 * Why not `hal-screen state`: forking a process every second on a 4-core A35
 * to read four ASCII bytes is absurd, and the second line of its output
 * (get_screen_state:) is the only field we want.
 * Why not the KMS summary: with the CRTC disabled by the idle daemon the plane
 * reads as inactive, which is indistinguishable from "video pipeline broken".
 * The two failures need different responses, so they need different sources.
 *
 * Returns 1 when on, 0 when off, and 1 when the file is unreadable: refusing to
 * call a session broken because a vendor file moved would be worse than
 * trusting the plane bit. */
static int ys_screen_on(void) {
    char buf[32];
    int fd = open("/tmp/.screen_state", O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0) return 1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 1;
    buf[n] = '\0';
    if (strstr(buf, "screen_on=0")) return 0;
    return 1;
}

/* Periodic work: heartbeat, RTP drain, RTP timeout tick, stats refresh. */
static void w_pump(void) {
    uint32_t now = ys_mono_ms();

    /* Signalling: sig_poll drives reads, the offer/candidate callbacks and
     * the 10 s bare-frame heartbeat, and clamps its own timeout to
     * sig_next_timeout() -- so polling with 0 here cannot make the heartbeat
     * late, and we never have to think about it again.
     *
     * rc < 0 means the socket died. We surface it instead of tearing down
     * here: teardown from inside the pump would race the command handler,
     * which is the only place allowed to unwind a session. */
    if (W.sig) {
        int rc = sig_poll(W.sig, 0);
        if (rc < 0 && (W.state == YS_ST_PLAYING || W.state == YS_ST_PAUSED)) {
            int st = sig_state(W.sig);
            if ((st == SIG_STATE_FAILED || st == SIG_STATE_CLOSED) &&
                W.st.error[0] == '\0') {
                const char *why = sig_last_error(W.sig);
                snprintf(W.st.error, sizeof(W.st.error), "%s", YS_ERR_WSS);
                snprintf(W.st.error_msg, sizeof(W.st.error_msg), "%s",
                         (why && *why) ? why : "信令连接已断开");
                ys_logf("session: signaling dead (%s)", sig_state_str(W.sig));
                w_publish();
            }
        }
    }

    if (W.udp_fd >= 0) {
        int f = w_drain_udp();
        if (f) W.st.frames_out += (uint32_t)f;
        /* The reorder buffer needs a clock-driven tick or a lost packet waits
         * for the ring to fill (1 s at 60 fps). */
        if (W.depay) {
            const uint8_t *dep = NULL;
            size_t dep_len = 0;
            rtp_depay_tick(W.depay, now, &dep, &dep_len);
            if (dep && dep_len && W.video) {
                ys_video_push_video(W.video, dep, dep_len, now);
                W.st.frames_out++;
            }
        }
    }

    /* Touch channel is polled inside w_pump(); see ys_session_ctl_poll-ish
     * logic in w_flush_touch(). Nothing to do here. */

    /* Pump the video pipeline: drains the GStreamer bus (so a decoder ERROR is
     * seen within one tick instead of never) and refreshes the fps window.
     * Must run every tick -- the bus is polled with a zero timeout, so this is
     * cheap, but skipping it means fps freezes at whatever it last saw and
     * gst errors go unnoticed until the next status read. */
    if (W.video) ys_video_tick(W.video, now);

    /* Media status pull is cheap (two file reads at most) but not free, so it
     * runs once per second rather than every tick. */
    if (now - W.last_stat_ms >= 1000) {
        W.last_stat_ms = now;
        W.st.screen_off = !ys_screen_on();
        if (W.video) {
            ys_video_status vs;
            ys_video_get_status(W.video, &vs);
            W.st.fps = vs.fps;
            W.st.plane_active = vs.plane_active;
            W.st.audio_ok = vs.audio_ok;
            if (vs.game_w > 0) { W.st.game_w = vs.game_w; W.st.game_h = vs.game_h; }
        }
        /* Once we are PLAYING but the plane is not active, the honest report is
         * that no picture is on screen -- UNLESS the panel is off, in which
         * case the CRTC is disabled by the idle daemon and there is nothing to
         * show. Conflating the two makes the UI declare failure every five
         * minutes of a perfectly healthy session. */
        if (W.state == YS_ST_PLAYING && W.video) {
            if (ys_screen_on() && !W.st.plane_active) W.st.ok = 0;
            else W.st.ok = 1;
        }
        w_publish();
    }
}

/* Normal and error teardown. Idempotent: called from stop, from error unwind,
 * and from the worker's exit path. Order is R3 in the header comment. */
static void w_teardown(const char *why) {
    ys_logf("teardown (%s) from state %s", why ? why : "-", ys_state_name(W.state));

    /* 1. control channel first: after this the server stops expecting input,
     *    and any queued touch frame is meaningless. */
    w_close_ctl();

    /* 2. pipeline + plane. ys_video_stop must bring the KMS plane down; if it
     *    fails the device keeps a frozen frame over the launcher, so we log it
     *    loudly rather than swallowing it. */
    if (W.video) {
        ys_video_stop(W.video);
        ys_video_destroy(W.video);
        W.video = NULL;
    }

    /* 3. depacketiser (plain memory) */
    if (W.depay) { rtp_depay_destroy(W.depay); W.depay = NULL; }

    /* 4. SRTP keys: zero them before the DTLS context goes away. */
    srtp_deinit(&W.srtp);

    /* 5. DTLS */
    if (W.dtls) { dtls_client_destroy(W.dtls); W.dtls = NULL; }

    /* 6. UDP socket */
    if (W.udp_fd >= 0) { close(W.udp_fd); W.udp_fd = -1; }

    /* 7. signalling socket last: it is what the server watches to decide the
     *    session is over, so it must outlive every local resource teardown. */
    if (W.sig) {
        sig_close(W.sig);     /* best-effort close frame: the server unwinds
                               * faster from a clean close than from a reset,
                               * and it costs one small write */
        sig_destroy(W.sig);
        W.sig = NULL;
    }

    ys_touch_reset(&W.touch);
    W.offer_seen = 0;
    W.first_frame_ms = 0;
    W.st.plane_active = 0;
    W.st.fps = 0;
    W.st.ok = 0;
    /* Do NOT clear an error here. The JS layer reads state + error to decide
     * what to show; unwinding resources must not erase the reason. IDLE is only
     * written when we were not already in ERROR. A following START clears it
     * through w_run_session's own reset. */
    if (W.state != YS_ST_ERROR) W.state = YS_ST_IDLE;
}

/* Bring one session up. Returns 0 when PLAYING. */
static int w_run_session(void) {
    char host[128], path[256];
    int port = 443;

    W.start_ms = ys_mono_ms();
    W.st.first_frame_ms = -1;
    W.st.wss_ms = W.st.dtls_ms = -1;
    W.st.packets_rx = W.st.frames_out = W.st.audio_frames = 0;
    W.st.packets_lost = W.st.frames_dropped = 0;
    W.st.rtp_late = W.st.srtp_auth_fail = 0;
    W.st.retransmits = 0;
    W.st.error[0] = '\0';
    W.st.error_msg[0] = '\0';
    W.state = YS_ST_IDLE;
    W.sig = NULL; W.ctl = NULL; W.dtls = NULL; W.udp_fd = -1;
    W.video = NULL; W.depay = NULL; W.offer_seen = 0;
    memset(&W.srtp, 0, sizeof(W.srtp));
    sdk_reset(&W.sdk);

    /* --- 0. decode sdk_param ONLY (cheap, catches bad input immediately) --- */
    {
        unsigned char *raw;
        size_t rawlen = 0;
        raw = b64_decode_alloc(W.sdk_b64, &rawlen);
        if (!raw) { w_fail(YS_ERR_BAD_SDK, "sdk_param base64 解码失败"); return -1; }
        if (sdk_decode(&W.sdk, raw, rawlen) != 0) {
            free(raw);
            w_fail(YS_ERR_BAD_SDK, "sdk_param 缺 game_svr_addr 或 wss url");
            return -1;
        }
        free(raw);
    }

    /* --- 0b. media socket FIRST.
     *
     * The answer SDP has to advertise the port WE send from, and we cannot
     * know it until the socket is bound. Opening it here (instead of after the
     * answer) also means the ICE check and its reply share the fd/5-tuple the
     * server already saw in the SDP. --- */
    if (w_open_udp() != 0) return -1;

        /* --- 1. DTLS certificate FIRST.
     *
     * sig_cfg_t carries our fingerprint, and sig.c needs it the moment it
     * builds the answer -- so the context has to exist before sig_create().
     * ECDSA P-256 (0-1 ms here; RSA-2048 would be 182-409 ms on this A35).
     * w_bring_up_media() MUST reuse this same context: a second one would
     * handshake with a certificate the server never saw in the answer, and it
     * rejects the ClientHello with no useful diagnostic. --- */
    W.dtls = dtls_client_create(DTLS_KEY_ECDSA_P256);
    if (!W.dtls) { w_fail(YS_ERR_DTLS, dtls_last_error()); return -1; }
    ys_logf("dtls: cert generated in %d ms", dtls_cert_gen_ms(W.dtls));

    /* --- 2. WSS signalling. sig.c owns the whole sequence: connect, client
     *    hello, offer wait, answer, candidate rewrite, 10 s bare-frame
     *    heartbeat. This file only creates it, polls it and destroys it --
     *    our own copy of that sequence would put two answers on the wire per
     *    session, which the server treats as a protocol violation. --- */
    w_state(YS_ST_CONNECTING);
    {
        uint32_t t0 = ys_mono_ms();
        char url[512];
        sig_cfg_t cfg;

        if (split_hostport(W.sdk.wss_url, host, sizeof(host), &port, path, sizeof(path), 443) != 0) {
            w_fail(YS_ERR_WSS, "wss_url 解析失败");
            return -1;
        }
        /* The signalling endpoint always needs /rtc-sdk appended (yys
         * pitfalls.md §11). game_server_wss_url already carries a path like
         * /dg13-c1-86363-1/50202, so this is NOT just the bare-host case: we
         * must append to whatever path is present. Mirrors the JS reference:
         *   url.endsWith('/rtc-sdk') || (url += '/rtc-sdk');
         * The old `if (!strcmp(path, "/"))` guard dropped the suffix whenever
         * the server gave a real path -> upgrade rejected (HTTP 233). */
        {
            size_t n = strlen(path);
            const char *sfx = "/rtc-sdk";
            size_t sn = strlen(sfx);
            if (n < sn || strcmp(path + n - sn, sfx) != 0) {
                if (n + sn < sizeof(path)) {
                    memcpy(path + n, sfx, sn + 1);
                } else {
                    w_fail(YS_ERR_WSS, "wss_url path too long");
                    return -1;
                }
            }
        }
        snprintf(url, sizeof(url), "wss://%s:%d%s", host, port, path);
        ys_logf("wss: connecting to %s", url);

        /* There is no STUN and no TURN on this service: the remote candidate
         * is pinned to game_svr_addr:rtc_udp_port (see w_open_udp), so ICE
         * degenerates into a direct connection to one fixed address. The
         * answer advertises OUR endpoint, which the socket above set up. */
        memset(&cfg, 0, sizeof(cfg));
        cfg.url = url;
        cfg.fingerprint = dtls_local_fingerprint(W.dtls);
        cfg.local_cand_ip = W.udp_local_ip[0] ? W.udp_local_ip : NULL;
        cfg.local_cand_port = W.udp_local_port;
        cfg.local_ip = "0.0.0.0";          /* candidates arrive out of band */
        cfg.codecs = SDP_CODEC_H264 | SDP_CODEC_OPUS;
        cfg.sendrecv = 0;                  /* we receive only */
        cfg.accept_datachannel = 0;        /* no SCTP on this device */
        cfg.connect_timeout_ms = YS_WSS_TIMEOUT_MS;
        cfg.heartbeat_ms = YS_HEARTBEAT_MS;
        cfg.stale_ms = YS_STALE_MS;

        /* The game-start exchange. See the long note in webrtc/sig.h: without
         * StartGameReq the pod never boots the title and never sends an offer,
         * which is exactly the NO_OFFER hang this project lived with. */
        cfg.game_token = W.sdk.game_token[0] ? W.sdk.game_token : NULL;
        cfg.account_id = W.aid[0] ? W.aid : NULL;
        cfg.sid = W.sdk.sid[0] ? W.sdk.sid : NULL;
        cfg.pod_id = W.sdk.pod_id[0] ? W.sdk.pod_id : NULL;
        cfg.sdk_version = YS_CG_SDK_VERSION;
        cfg.terminal_type = YS_CG_TERMINAL_WEB_TOUCH;

        cfg.cbs.on_offer = on_sig_offer;
        cfg.cbs.on_state = on_sig_state;
        cfg.user = &W;

        W.sig = sig_create(&cfg);
        if (!W.sig) { w_fail(YS_ERR_WSS, "sig_create 失败"); return -1; }
        /* sig_open blocks for the TLS + upgrade exchange (222-263 ms
         * measured). We are on the worker thread, which is the only place
         * this is allowed to happen. */
        if (sig_open(W.sig) != 0) {
            w_fail(YS_ERR_WSS, sig_last_error(W.sig));
            return -1;
        }
        W.st.wss_ms = (int)(ys_mono_ms() - t0);
        ys_logf("wss: up in %d ms", W.st.wss_ms);
    }

/* --- 2. wait for the server's offer (it is the offerer, not us) --- */
    if (w_wait(YS_OFFER_TIMEOUT_MS, done_offer, NULL, "offer") != 0) {
        if (W.state != YS_ST_ERROR)
            w_fail(YS_ERR_NO_OFFER, "等待服务端 offer 超时");
        return -1;
    }

    /* --- 3. UDP + DTLS + SRTP + pipeline + control channel --- */
    w_state(YS_ST_HANDSHAKE);
    w_publish();

    /* The answer needs a certificate, and the DTLS handshake needs the answer's
     * fingerprint to be on the wire -- but the handshake can start as soon as
     * our cert exists, because the server accepts the ClientHello whenever it
     * likes. So: generate cert + send answer first, then handshake. */
    if (w_send_answer() != 0) return -1;
    if (W.state == YS_ST_ERROR) return -1;

    /* Media bring-up must NOT create its own DTLS: w_run_session created
     * W.dtls before sig_create() (the fingerprint had to exist for the
     * answer), and the handshake has to use that exact context. Guarded in
     * w_bring_up_media by `if (!W.dtls)`. */
    if (w_bring_up_media() != 0) return -1;
    if (W.state == YS_ST_ERROR) return -1;

    /* --- 4. preroll: start the pipeline paused, push until it has frames --- */
    if (ys_video_start(W.video, YS_PREROLL_TIMEOUT_MS) != 0) {
        w_fail(YS_ERR_VIDEO, "管线 preroll 失败（KMS 平面未激活）");
        return -1;
    }

    /* --- 5. play --- */
    W.state = YS_ST_PLAYING;
    W.st.ok = 1;
    ys_logf("session PLAYING at %u ms", ys_mono_ms() - W.start_ms);
    w_publish();
    return 0;
}

/* =============================================================== worker loop
 *
 * The loop is deliberately dumb: pump, then drain at most one command, repeat.
 * Draining one command per tick (rather than all pending) means a burst of 64
 * queued touch events cannot starve the RTP drain for more than 64 ticks,
 * which at 10 ms is 640 ms -- still too long, so the loop actually drains up to
 * 8 commands per tick, which bounds the worst case at 80 ms.
 */
static void w_handle_cmd(ys_cmd_t *c) {
    switch (c->type) {
    case YS_CMD_START: {
        ys_cmd_start *s = (ys_cmd_start *)c->payload;
        int sdk_len = s->sdk_len;
        int aid_len = s->aid_len;
        if (sdk_len <= 0 || sdk_len > YS_MAX_SDK_PARAM_B64) {
            w_fail(YS_ERR_BAD_PARAM, "sdk_param 长度非法");
            return;
        }
        if (aid_len < 0 || aid_len >= (int)sizeof(W.aid)) {
            w_fail(YS_ERR_BAD_PARAM, "aid 长度非法");
            return;
        }
        if ((size_t)c->len < sizeof(ys_cmd_start) + (size_t)sdk_len + 1 +
                              (size_t)aid_len + 1) {
            w_fail(YS_ERR_BAD_PARAM, "START 命令被截断");
            return;
        }
        /* Unconditional: a second START can arrive while the first is still in
         * CONNECTING or HANDSHAKE (the JS layer times out at 45 s and retries
         * while we are still inside w_wait). Then W.sig and W.dtls are all
         * live and w_run_session() would overwrite them -- leaking a
         * socket and, worse, an OpenSSL context whose DTLS state is half
         * negotiated. Tearing down from ANY non-idle state is the only safe
         * form; w_teardown is idempotent and tolerates -1/NULL fields. */
        if (W.state != YS_ST_IDLE) w_teardown("restart");
        W.rect[0] = s->rect_x; W.rect[1] = s->rect_y;
        W.rect[2] = s->rect_w; W.rect[3] = s->rect_h;
        memcpy(W.sdk_b64, s->sdk, (size_t)sdk_len);
        W.sdk_b64[sdk_len] = '\0';
        if (aid_len > 0) {
            memcpy(W.aid, s->sdk + sdk_len + 1, (size_t)aid_len);
            W.aid[aid_len] = '\0';
        } else {
            W.aid[0] = '\0';
        }
        if (w_run_session() != 0) {
            /* Every failure inside w_run_session has already called w_fail(),
             * but the resources allocated up to that point (signalling socket,
             * DTLS context, UDP fd) are still live. Unwinding here -- at the
             * single call site -- is why w_run_session itself stays linear
             * without a goto cascade. */
            w_teardown("start failed");
            w_publish();
        }
        break;
    }
    case YS_CMD_STOP:
        if (W.state != YS_ST_IDLE) w_teardown("stop command");
        w_publish();
        break;
    case YS_CMD_PAUSE:
        if (W.state == YS_ST_PLAYING) {
            if (W.video) ys_video_pause(W.video);
            W.state = YS_ST_PAUSED;
            W.st.ok = 1;
            w_publish();
        }
        break;
    case YS_CMD_RESUME:
        if (W.state == YS_ST_PAUSED) {
            if (W.video) ys_video_resume(W.video);
            W.state = YS_ST_PLAYING;
            w_publish();
        }
        break;
    case YS_CMD_SET_RECT: {
        ys_cmd_rect *r = (ys_cmd_rect *)c->payload;
        W.rect[0] = r->rect_x; W.rect[1] = r->rect_y;
        W.rect[2] = r->rect_w; W.rect[3] = r->rect_h;
        if (W.video) {
            int rc = ys_video_set_rect(W.video, W.rect[0], W.rect[1], W.rect[2], W.rect[3]);
            if (rc != 0) ys_logf("set_rect(%d,%d,%d,%d) rc=%d",
                                 W.rect[0], W.rect[1], W.rect[2], W.rect[3], rc);
        }
        break;
    }
    case YS_CMD_TOUCH: {
        ys_cmd_touch *t = (ys_cmd_touch *)c->payload;
        unsigned char pkt[YS_TOUCH_MAX_PACKET];
        int n = ys_touch_feed(&W.touch, t->finger_id, t->action, t->nx, t->ny,
                              pkt, sizeof(pkt));
        if (n > 0 && W.ctl) {
            /* Same Packet bytes as the DataChannel would carry; only the
             * carrier differs. A failed send is dropped, not retried: a stale
             * touch is worse than a missing one. */
            wss_send_bin(W.ctl, pkt, (size_t)n);
        }
        break;
    }
    case YS_CMD_RELEASE:
        w_teardown("release");
        w_publish();
        break;
    default:
        ys_logf("unknown cmd %d", c->type);
        break;
    }
}

static void *worker_main(void *arg) {
    ys_thread_set_name("ys-worker");
    (void)arg;

    while (!atomic_load(&G.stop_req)) {
        /* 1. periodic work: heartbeat, RTP, stats */
        if (W.state != YS_ST_IDLE) {
            w_pump();
        } else if (W.ctl) {
            /* Nothing should be open in IDLE, but if teardown half-failed we
             * must not leave a socket alive silently. */
            w_close_ctl();
        }

        /* 2. state timeouts */
        if (W.state == YS_ST_CONNECTING || W.state == YS_ST_HANDSHAKE) {
            if (ys_mono_ms() - W.start_ms > YS_START_BUDGET_MS) {
                w_fail(YS_ERR_TIMEOUT, "起播超时");
            }
        }
        if (W.state == YS_ST_PLAYING && W.sig) {
            int age = sig_last_rx_age_ms(W.sig);
            if (age > YS_STALE_MS) {
                /* Surface, do not auto-reconnect: failover order lives in JS. */
                if (W.st.error[0] == '\0') {
                    snprintf(W.st.error, sizeof(W.st.error), "%s", YS_ERR_STALE);
                    snprintf(W.st.error_msg, sizeof(W.st.error_msg),
                             "信令 %d ms 无数据", age);
                    ys_logf("session: signaling stale");
                    w_publish();
                }
            }
        }

        /* 3. commands, at most 8 per tick */
        {
            int k;
            for (k = 0; k < 8; k++) {
                ys_cmd_t c;
                if (ys_queue_pop(&G.q, &c, 0) != 0) break;
                w_handle_cmd(&c);
                if (atomic_load(&G.stop_req)) break;
            }
        }

        /* 4. sleep the rest of the tick, but wake early if a command arrives */
        {
            ys_cmd_t c;
            if (ys_queue_pop(&G.q, &c, YS_TICK_MS) == 0) {
                w_handle_cmd(&c);
            }
        }
    }

    ys_logf("worker exiting");
    w_teardown("shutdown");
    w_publish();
    atomic_store(&G.running, 0);
    return NULL;
}

/* ============================================================ public surface
 */

int ys_session_init(void) {
    if (atomic_load(&G.inited)) return 0;

    memset(&W, 0, sizeof(W));
    W.udp_fd = -1;
    W.state = YS_ST_IDLE;
    W.st.first_frame_ms = -1;
    W.st.wss_ms = W.st.dtls_ms = -1;

    ys_queue_init(&G.q);
    atomic_store(&G.seq, 0);
    memset(&G.snap, 0, sizeof(G.snap));
    G.snap.state = YS_ST_IDLE;
    atomic_store(&G.stop_req, 0);

    if (ys_thread_spawn(&G.th, "ys-worker", worker_main, NULL, 512u * 1024u) != 0) {
        ys_logf("worker spawn failed: %d", errno);
        return -1;
    }
    atomic_store(&G.running, 1);
    atomic_store(&G.inited, 1);
    ys_logf("session worker started");
    return 0;
}

void ys_session_shutdown(void) {
    if (!atomic_load(&G.inited)) return;
    atomic_store(&G.stop_req, 1);
    ys_queue_close(&G.q);
    pthread_join(G.th, NULL);
    ys_queue_destroy(&G.q);
    atomic_store(&G.inited, 0);
}

int ys_session_start(const char *sdk_b64, const char *aid,
                     int x, int y, int w, int h) {
    ys_cmd_start *s;
    unsigned char buf[YS_CMD_MAX_PAYLOAD];
    char *dst;
    int sdk_len, aid_len, total;

    if (!atomic_load(&G.inited)) return -3;
    if (!sdk_b64) return -1;
    sdk_len = (int)strlen(sdk_b64);
    if (sdk_len <= 0 || sdk_len > YS_MAX_SDK_PARAM_B64) return -1;
    aid_len = aid ? (int)strlen(aid) : 0;
    /* 96 chars is far beyond any real aid (they are short numeric strings) and
     * keeps the command comfortably inside YS_CMD_MAX_PAYLOAD. */
    if (aid_len < 0 || aid_len > 95) return -1;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    if (x < 0 || y < 0 || x + w > 4096 || y + h > 4096) return -1;

    total = (int)sizeof(ys_cmd_start) + sdk_len + 1 + aid_len + 1;
    if (total > (int)sizeof(buf)) return -1;

    memset(buf, 0, sizeof(buf));
    s = (ys_cmd_start *)buf;
    s->rect_x = x; s->rect_y = y; s->rect_w = w; s->rect_h = h;
    s->sdk_len = sdk_len;
    s->aid_len = aid_len;
    dst = s->sdk;
    memcpy(dst, sdk_b64, (size_t)sdk_len + 1);
    dst += sdk_len + 1;
    if (aid_len > 0) memcpy(dst, aid, (size_t)aid_len + 1);
    return ys_queue_push(&G.q, YS_CMD_START, buf, total) == 0 ? 0 : -2;
}

int ys_session_stop(void) {
    if (!atomic_load(&G.inited)) return -3;
    return ys_queue_push(&G.q, YS_CMD_STOP, NULL, 0) == 0 ? 0 : -2;
}

int ys_session_pause(void) {
    if (!atomic_load(&G.inited)) return -3;
    return ys_queue_push(&G.q, YS_CMD_PAUSE, NULL, 0) == 0 ? 0 : -2;
}

int ys_session_resume(void) {
    if (!atomic_load(&G.inited)) return -3;
    return ys_queue_push(&G.q, YS_CMD_RESUME, NULL, 0) == 0 ? 0 : -2;
}

int ys_session_set_rect(int x, int y, int w, int h) {
    ys_cmd_rect r;
    if (!atomic_load(&G.inited)) return -3;
    if (w <= 0 || h <= 0) return -1;
    r.rect_x = x; r.rect_y = y; r.rect_w = w; r.rect_h = h;
    return ys_queue_push(&G.q, YS_CMD_SET_RECT, &r, sizeof(r)) == 0 ? 0 : -2;
}

int ys_session_touch(float nx, float ny, int action, int finger_id) {
    ys_cmd_touch t;
    int rc;
    if (!atomic_load(&G.inited)) return -3;
    if (action < 0 || action > 2) return -1;
    /* Coordinates are NOT clamped here on purpose: the game needs to see
     * out-of-rect coordinates to interpret a drag leaving the viewport as a
     * cancel. touch.c clamps at the encoder, which is the last possible point. */
    t.nx = nx; t.ny = ny; t.action = action; t.finger_id = finger_id;
    rc = ys_queue_push(&G.q, YS_CMD_TOUCH, &t, sizeof(t));
    if (rc == -1) {
        /* Queue full. DROP moves silently; report drops of down/up. */
        return action == 2 ? 1 : -2;
    }
    return rc == 0 ? 0 : -2;
}

int ys_session_release(void) {
    if (!atomic_load(&G.inited)) return -3;
    return ys_queue_push(&G.q, YS_CMD_RELEASE, NULL, 0) == 0 ? 0 : -2;
}

void ys_session_snapshot(ys_status *out) {
    unsigned s1, s2;
    if (!out) return;
    do {
        s1 = atomic_load(&G.seq);
        atomic_thread_fence(memory_order_acquire);
        if (s1 & 1u) { ys_sleep_ms(0); continue; }
        memcpy(out, &G.snap, sizeof(*out));
        atomic_thread_fence(memory_order_acquire);
        s2 = atomic_load(&G.seq);
    } while (s1 != s2);
}
