/* sig.h -- cloud-gaming signaling state machine over WSS.
 *
 * Owns the whole handshake sequence the service expects:
 *
 *   WSS connect -> send StartGameReq  (type 2 proxy frame, protobuf cmd 20001)
 *              -> receive StartGameRsp (type 2 proxy frame, cmd 20002,
 *                                       retcode must be 1 / RtcSucc)
 *              -> send "client hello"  (type 3)
 *              -> receive an SDP offer (type 1)
 *              -> build + send the answer (type 1)
 *              -> receive remote candidates (type 1), rewrite them to the
 *                 fixed game-server address, hand them to the caller
 *              -> send our own candidates
 *              -> keep the session alive with a bare empty frame every 10 s
 *
 * The first two steps are the ones that matter and the ones this project got
 * wrong for a long time: with only "client hello" on the wire the server
 * answers with a single `{"type":"server hello","session_id":1}` and then goes
 * completely silent, so the client sits in NO_OFFER forever. server hello is
 * NOT a prompt to continue -- the official SDK only uses its session_id to
 * open the separate :5443 control channel. The conversation cannot start until
 * the game pod is told which game to start, and that is StartGameReq.
 *
 * There is NO internal thread and NO timer thread. Everything is driven by
 * the caller's single worker loop:
 *
 *   for (;;) {
 *       int tmo = sig_next_timeout(s);
 *       if (tmo < 0) tmo = 1000;
 *       rc = sig_poll(s, tmo);
 *       if (rc < 0) break;
 *       ... the caller also polls its own fds ...
 *   }
 *
 * sig_poll() blocks for at most `timeout_ms`, does whatever is due (reads,
 * heartbeat), and returns. It never sleeps longer than asked.
 */

#ifndef PEN_WEBRTC_SIG_H
#define PEN_WEBRTC_SIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------- frame format --
 *
 * Every application frame on the signaling socket is
 *
 *    [0..3] type   (big endian)
 *    [4..7] length (big endian, payload bytes only)
 *    [8..]  payload
 *
 * The keepalive is the ONLY frame without this header: a bare empty
 * WebSocket frame (the reference client does `websocket.send("")`).
 */
#define SIG_TYPE_SIGNALING 1   /* JSON: SDP + ICE, both directions          */
#define SIG_TYPE_PROXY     2   /* Packet() frames: StartGameReq/Rsp, etc.   */
#define SIG_TYPE_HANDSHAKE 3   /* JSON: client hello / server hello         */
#define SIG_TYPE_KEEPALIVE 4   /* header-carrying explicit keepalive        */

/* ------------------------------------------------------- StartGame exchange
 *
 * A type-2 payload is not opaque here: it is a `Packet` protobuf envelope
 * (game_sdk/protocol.cpp in the reference client):
 *
 *   [0..1]  magic head 0x4567 (17767)
 *   [2..3]  cmd_id, big endian
 *   [4..5]  head_len, big endian (0 for everything we send)
 *   [6..9]  msg_len, 32-bit big endian
 *   [10..]  PacketHead (head_len bytes; empty for us) followed by msg
 *   [-2..]  magic tail 0x89AB (35243)
 *
 * so a headless packet is exactly 12 + msg_len bytes.
 *
 * StartGameReq (cmd 20001), proto3 so empty/zero fields are simply omitted:
 *   1 game_token (string)   2 aid (string)        3 sdk_version (string)
 *   4 terminal_type (uint32) 5 offline_mock_info   6 sid (string)
 *   7 pod_id (string)        8 has_rtc_connected   9 since_start_sec
 *  10 link_tasks_ms         11 mock_game_mode     12 game_config (message)
 *
 * StartGameRsp (cmd 20002): 1 retcode (int32), 2 game_app_id (uint32),
 *   3 game_config (message). retcode == 1 (RtcSucc) means "the pod is booting
 *   your game, go ahead and negotiate".
 */
#define SIG_PACKET_MAGIC_HEAD 0x4567
#define SIG_PACKET_MAGIC_TAIL 0x89AB
#define SIG_PACKET_MIN_SIZE   12
#define SIG_CMD_START_GAME_REQ 20001
#define SIG_CMD_START_GAME_RSP 20002
#define SIG_START_GAME_SUCC    1       /* ResultCode::RtcSucc */

#define SIG_FRAME_HEADER 8

/* Pack one frame. Returns the total bytes written (8 + len) or -1 if the
 * buffer is too small. `out` may alias nothing; payload is copied. */
int sig_frame_pack(int type, const void *payload, size_t len,
                   unsigned char *out, size_t cap);

/* Parse one frame out of `buf`.
 * Returns  1 = one complete frame (payload points INSIDE buf, no copy)
 *          0 = need more bytes
 *         -1 = malformed (unknown type, length beyond any sane limit)
 * `consumed` receives the number of bytes the frame occupies; it may be NULL.
 * `type`, `payload` and `plen` may each be NULL if not wanted. */
int sig_frame_parse(const unsigned char *buf, size_t len, int *type,
                    const unsigned char **payload, size_t *plen,
                    size_t *consumed);

/* ---------------------------------------------------------------- states */
#define SIG_STATE_IDLE       0  /* created, not opened                      */
#define SIG_STATE_CONNECTING 1  /* wss_connect() in flight                  */
#define SIG_STATE_OPEN       2  /* WSS up, client hello sent                */
#define SIG_STATE_OFFERED    3  /* offer received, answer not yet sent      */
#define SIG_STATE_ANSWERED   4  /* answer sent, waiting for candidates      */
#define SIG_STATE_CONNECTED  5  /* at least one remote candidate received   */
#define SIG_STATE_STALE      6  /* nothing received for stale_ms            */
#define SIG_STATE_CLOSED     7  /* peer closed cleanly                      */
#define SIG_STATE_FAILED     8  /* protocol/socket error                    */

const char *sig_state_name(int state);

/* Named sig_conn_t, not sig_t: glibc's <signal.h> already typedefs sig_t
 * (to __sighandler_t), so any translation unit that includes both would fail
 * to compile. Keep this hand-rolled connection object's handle name distinct
 * from the libc one. */
typedef struct sig sig_conn_t;

/* Callbacks. All are optional. `s` is valid inside the callback only;
 * do not call sig_destroy() from one. */
typedef struct {
    /* A remote SDP offer arrived and parsed successfully. The caller should
     * call sig_answer_auto() (or build its own answer with the sdp module and
     * call sig_send_answer()) -- usually from inside this callback. */
    void (*on_offer)(sig_conn_t *s, const char *offer_sdp, void *user);

    /* A remote candidate arrived and has ALREADY been rewritten to
     * cfg.candidate_ip:cfg.candidate_port. `line` is the rewritten candidate
     * line, ready to hand straight to the ICE/DTLS layer. */
    void (*on_candidate)(sig_conn_t *s, const char *line, void *user);

    /* Any non-JSON payload (type 2 proxy frames) reached us verbatim. */
    void (*on_bytes)(sig_conn_t *s, int type, const unsigned char *data, size_t len,
                     void *user);

    /* State transition, invoked AFTER the state field is updated. */
    void (*on_state)(sig_conn_t *s, int old_state, int new_state, void *user);
} sig_cbs_t;

typedef struct {
    /* wss://host[:port]/path  -- parsed internally. Required. */
    const char *url;

    /* Optional Sec-WebSocket-Protocol value. */
    const char *subprotocols;

    /* Our DTLS fingerprint, WITH the "sha-256 " prefix (as returned by
     * dtls_local_fingerprint()). Required for an automatic answer. */
    const char *fingerprint;

    /* Candidate rewriting target. When the service sends a candidate we
     * replace its ip:port with these, because the game server rewrites ICE
     * anyway and the real media address is known out of band. Set
     * candidate_port to 0 to disable rewriting (candidates pass through). */
    const char *candidate_ip;
    int         candidate_port;

    /* Local address to advertise in the answer (c= line). May be NULL. */
    const char *local_ip;

    int codecs;              /* SDP_CODEC_* mask, 0 = accept what is offered */
    int sendrecv;            /* 1 = a=sendrecv, 0 = a=recvonly (default)     */
    int accept_datachannel;  /* 1 = accept m=application (we have no SCTP)   */

    int connect_timeout_ms;  /* wss_connect() budget, default 8000           */
    int heartbeat_ms;        /* bare-frame keepalive period, default 10000   */
    int stale_ms;            /* silence before SIG_STATE_STALE, default 30000*/

    /* ---- StartGameReq fields (see the note above SIG_TYPE_PROXY) ----
     *
     * The reference client does NOT send "client hello" as its first action on
     * the signaling socket. The first thing it sends is a type-2 proxy frame
     * carrying a protobuf StartGameReq(20001); only after the server answers
     * StartGameRsp(20002) with retcode==RtcSucc does it create the RTCPeer-
     * Connection and send "client hello" (frame type 3) -- and only then does
     * the server push an offer.
     *
     * If game_token is empty the machine keeps the OLD behaviour (immediate
     * client hello). That path is still useful against a mock/test server, and
     * it keeps this change from breaking anything that cannot supply a token.
     */
    const char *game_token;  /* sdk_param field 4, "game_token"        */
    const char *account_id;  /* gamer/api/login .aid (StartGameReq 2)   */
    const char *sid;         /* sdk_param field 1                       */
    const char *pod_id;      /* sdk_param field 3                       */
    const char *sdk_version; /* CG_SDK_VERSION, default "6.1.0.65"      */
    int         terminal_type;/* ClientTerminalType, default 9 (WebTouch)*/

    sig_cbs_t cbs;
    void *user;
} sig_cfg_t;

/* --------------------------------------------------------------- lifecycle */

/* Create the machine. Returns NULL on bad arguments (cfg->url required).
 * Does no I/O. */
sig_conn_t *sig_create(const sig_cfg_t *cfg);

/* Connect the WSS socket and kick off the game-start exchange.
 *
 * With cfg->game_token set this sends StartGameReq and WAITS (asynchronously,
 * from sig_poll()) for StartGameRsp before the client hello goes out. Without
 * a token it does the old thing: client hello immediately.
 *
 * BLOCKS for the duration of the TLS + upgrade exchange (measured 222-263 ms
 * against the real service), so call it from the worker thread, not from a
 * latency-critical callback. Returns 0 on success, -1 on failure
 * (sig_last_error() explains). */
int sig_open(sig_conn_t *s);

/* Drive the machine. Blocks at most `timeout_ms` (0 = pure poll).
 * Returns >=0 (usually the number of frames processed), or -1 on error. */
int sig_poll(sig_conn_t *s, int timeout_ms);

/* Milliseconds until the machine next has something to do (heartbeat due,
 * stale deadline), capped at 1000. Returns -1 only when the machine has no
 * socket (before sig_open() or after sig_destroy()); it never returns -1
 * while open, so it is always safe to pass the result straight to sig_poll().
 *
 * sig_poll() additionally clamps its own timeout to this value, so passing a
 * fixed 10 ms -- the natural choice when the RTP path also needs servicing --
 * cannot make the heartbeat late. Never pass -1 to sig_poll(): that would
 * block until a frame arrives, which on an idle session is ~10 s and stalls
 * whatever else the worker thread drives. */
int sig_next_timeout(const sig_conn_t *s);

void sig_destroy(sig_conn_t *s);

/* ------------------------------------------------------------------- send */

/* Send a raw application frame. type is SIG_TYPE_*. */
int sig_send(sig_conn_t *s, int type, const void *payload, size_t len);

/* Build the SDP answer for the offer we hold, using the fingerprint and
 * options from the config. Writes a NUL-terminated answer into `out`.
 * Returns the answer length or -1 (sig_last_error() explains). No I/O. */
int sig_build_answer(sig_conn_t *s, char *out, size_t cap);

/* Send an already built answer as `{"type":"answer","sdp":"..."}`.
 * `answer` may also be a raw JSON object (leading '{'), in which case it is
 * sent verbatim. Returns 0 or -1. */
int sig_send_answer(sig_conn_t *s, const char *answer);

/* Build and send, in one call. Returns 0 or -1. */
int sig_answer_auto(sig_conn_t *s);

/* Send one local candidate to the peer. The line may include the leading
 * "a=" or "candidate:"; it is normalised before encoding. */
int sig_send_candidate(sig_conn_t *s, const char *line);

/* Request a clean shutdown of the signaling socket. */
int sig_close(sig_conn_t *s);

/* ---------------------------------------------------------------- queries */

int         sig_state(const sig_conn_t *s);
const char *sig_state_str(const sig_conn_t *s);
int         sig_last_rx_age_ms(const sig_conn_t *s);
int         sig_heartbeats_sent(const sig_conn_t *s);
int         sig_frames_rx(const sig_conn_t *s);
const char *sig_last_error(const sig_conn_t *s);
/* The offer most recently received, or "" if none. */
/* Payload types actually agreed to in the answer we generated, or -1 if no
 * answer has been built yet. The answer echoes the offer's numbers, so these
 * match the offer -- but read them from here and you never have to keep the
 * offer alive or re-parse it. Feed them straight into the RTP depacketiser
 * and the SRTP payload-type filter. */
int sig_video_payload_type(const sig_conn_t *s);
int sig_audio_payload_type(const sig_conn_t *s);

const char *sig_current_offer(const sig_conn_t *s);

/* ICE short-term credentials of the current negotiation, valid once an answer
 * has been built. The STUN layer needs them: USERNAME is
 * "<remote_ufrag>:<local_ufrag>", requests carry MESSAGE-INTEGRITY keyed with
 * the remote pwd, responses with the local one. Empty strings before that. */
const char *sig_local_ice_ufrag(const sig_conn_t *s);
const char *sig_local_ice_pwd(const sig_conn_t *s);
const char *sig_remote_ice_ufrag(const sig_conn_t *s);
const char *sig_remote_ice_pwd(const sig_conn_t *s);

/* Underlying socket, for an external poll() loop. -1 when not connected. */
int sig_fd(const sig_conn_t *s);

/* Rewrite the ip:port in a candidate line. Semantics match the reference
 * client: split the line on spaces and replace fields 4 (ip) and 5 (port),
 * keeping everything else byte for byte. Returns 0 or -1. */
int sig_candidate_rewrite(const char *line, const char *ip, int port,
                          char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PEN_WEBRTC_SIG_H */
