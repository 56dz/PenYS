/* session.h -- the one state machine, the one worker thread, the one owner.
 *
 * ARCHITECTURE IN ONE PARAGRAPH
 * A cloud-game session is a linear sequence of blocking resources: an HTTP
 * dispatch (done in JS), a WSS signalling socket, a UDP socket shared by DTLS
 * and SRTP, a GStreamer pipeline, and a KMS overlay plane. Every one of them
 * must be created and destroyed in order, exactly once, or the device is left
 * with a plane stuck on screen or a socket the server still thinks is alive.
 * So exactly ONE thread owns all of them and exactly ONE state machine moves
 * between them. The JS thread can only push commands (core/queue.h) and read a
 * lock-free snapshot (ys_session_snapshot). Nothing else crosses the boundary.
 *
 * STATE MACHINE (single direction, idempotent; see the table in session.c)
 *
 *   IDLE ──start──> CONNECTING ──> HANDSHAKE ──> PLAYING <──> PAUSED
 *     ^                 │              │            │
 *     └────stop/error───┴──────────────┴────────────┘
 *
 * WHY `stop` IS ACCEPTED IN EVERY STATE
 * The user can press back at any moment: during the WSS connect, during the
 * DTLS handshake, one frame after the plane came up. Every blocking call in the
 * worker is therefore bounded and re-checks `g->stop_req` between slices; a
 * stop request never has to wait for a long operation to finish on its own.
 *
 * WHY THE SNAPSHOT IS A SEQLOCK AND NOT A MUTEX
 * The JS thread reads status() at 5 Hz to drive the UI. A mutex shared with a
 * worker that may be inside a 3-second pipeline preroll would let that read
 * block for the whole preroll, which is exactly the kind of stall the 5 ms
 * p99 rule forbids. A seqlock read retries instead of blocking, so worst case
 * it spins for the duration of one memcpy.
 */
#ifndef YS_SESSION_SESSION_H
#define YS_SESSION_SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Session states, in the order they occur. The strings are part of the JS
 * contract (src/services/player.js switches on them), so do not rename. */
typedef enum {
    YS_ST_IDLE = 0,
    YS_ST_CONNECTING,      /* WSS connect + client hello                       */
    YS_ST_HANDSHAKE,       /* got offer: UDP + DTLS + SDP answer               */
    YS_ST_PLAYING,
    YS_ST_PAUSED,
    YS_ST_STOPPING,
    YS_ST_ERROR
} ys_state;

/* Error codes reported through the snapshot. Kept as strings so the UI can map
 * them to Chinese copy without a shared enum header on both sides. */
#define YS_ERR_NONE          ""
#define YS_ERR_BAD_PARAM     "BAD_PARAM"
#define YS_ERR_BAD_SDK       "BAD_SDK_PARAM"
#define YS_ERR_NO_OFFER      "NO_OFFER"
#define YS_ERR_SDP           "SDP_FAIL"
#define YS_ERR_UDP           "UDP_FAIL"
#define YS_ERR_DTLS          "DTLS_FAIL"
#define YS_ERR_NO_SRTP       "NO_SRTP_PROFILE"
#define YS_ERR_VIDEO         "VIDEO_FAIL"
#define YS_ERR_WSS           "WSS_FAIL"
#define YS_ERR_STALE         "SIGNALING_STALE"
#define YS_ERR_TIMEOUT       "TIMEOUT"
#define YS_ERR_INTERNAL      "INTERNAL"

/* Everything the JS layer can observe. Filled by the worker, read by JS. */
typedef struct {
    int      state;               /* ys_state                         */
    int      ok;
    /* media */
    double   fps;
    int      latency_ms;          /* last measured glass-to-glass proxy */
    int      bitrate_kbps;
    int      audio_ok;
    int      plane_active;
    int      screen_off;
    /* negotiated */
    char     video_codec[32];
    char     audio_codec[32];
    int      game_w, game_h;
    int      video_w, video_h;
    /* counters */
    uint32_t packets_rx;
    uint32_t packets_lost;
    uint32_t frames_out;
    uint32_t frames_dropped;
    uint32_t audio_frames;
    uint32_t rtp_late;
    uint32_t srtp_auth_fail;
    /* diagnostics */
    int      wss_ms, dtls_ms, first_frame_ms;   /* -1 = not reached */
    int      retransmits;
    char     error[32];           /* YS_ERR_*                         */
    char     error_msg[160];      /* human text, safe to show         */
    char     cipher[48];
    char     srtp_profile[48];
} ys_status;

/* Create the worker. Idempotent; safe to call from onLaunch.
 * Returns 0 on success. */
int ys_session_init(void);

/* Tear the worker down. Called from app destroy only; blocking but bounded. */
void ys_session_shutdown(void);

/* --- commands: all non-blocking, all safe from the JS thread --- */

/* sdk_b64 is the base64 `sdk_param` from paasDispatch. `aid` is the gamer
 * account id (may be NULL/empty): it is required by StartGameReq and is the one
 * value the dispatch response does not carry, so the JS layer has to hand it
 * over explicitly. Returns 0 if queued, -1 on bad parameters, -2 if the queue
 * is full, -3 if not initialised. */
int ys_session_start(const char *sdk_b64, const char *aid,
                     int x, int y, int w, int h);
int ys_session_stop(void);
int ys_session_pause(void);
int ys_session_resume(void);
int ys_session_set_rect(int x, int y, int w, int h);
/* nx/ny are normalized inside the video rect; action 0=down 1=up 2=move.
 * Returns 0 queued, 1 dropped (queue full, move only), -1 bad param. */
int ys_session_touch(float nx, float ny, int action, int finger_id);
int ys_session_release(void);

/* Copy the latest snapshot. Never blocks; may spin briefly. */
void ys_session_snapshot(ys_status *out);

/* Convert a state number to its JS-visible string ("idle", "playing", ...). */
const char *ys_state_name(int st);

#ifdef __cplusplus
}
#endif
#endif /* YS_SESSION_SESSION_H */
