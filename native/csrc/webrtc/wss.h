/* wss.h -- minimal RFC6455 WebSocket client (TLS + optional subprotocols).
 *
 * Design goals for the X6 Pro cloud-gaming client:
 *   - no heap churn while running: every buffer is allocated once in
 *     wss_connect() and reused for the life of the handle;
 *   - single-threaded, non-blocking: wss_poll() either returns immediately or
 *     blocks at most timeout_ms in poll(2); it never busy-waits;
 *   - no permessage-deflate, no extensions, no redirects, no auth.
 *
 * Mandatory device lesson baked in: getaddrinfo() is called with
 * ai_family = AF_INET. With AF_UNSPEC the resolver tries IPv6 first and the
 * TCP connect goes from ~120 ms to ~15000 ms on this firmware.
 */
#ifndef WEBRTC_WSS_H
#define WEBRTC_WSS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WebSocket opcodes as delivered to the callback. */
#define WSS_OP_CONT   0x0
#define WSS_OP_TEXT   0x1
#define WSS_OP_BIN    0x2
#define WSS_OP_CLOSE  0x8
#define WSS_OP_PING   0x9
#define WSS_OP_PONG   0xA

typedef struct wss wss_t;

/* Delivered once per *complete* message: fragmented frames (opcode 0x1/0x2
 * with FIN=0 followed by continuations) are reassembled first, and the
 * callback receives the opcode of the first frame. Control frames are
 * delivered as they arrive (they may legally interleave a fragmented
 * message). PING is answered with a PONG automatically before the callback
 * runs. When the peer closes, opcode is WSS_OP_CLOSE and len is 0.
 *
 * `data` is only valid for the duration of the callback. */
typedef void (*wss_msg_cb)(wss_t *h, int opcode,
                           const unsigned char *data, size_t len, void *user);

/* Connect, TLS-handshake, then perform the RFC6455 upgrade.
 *
 * host           hostname or IP literal. Used for both DNS and SNI and Host:.
 * port           TCP port (443 typical).
 * path           request target, must start with '/', e.g. "/wsspingsvr/echo".
 * subprotocols   NULL, or a comma-separated Sec-WebSocket-Protocol value such
 *                as "binary, base64". The server's choice is available via
 *                wss_selected_subprotocol().
 * timeout_ms     budget for the whole connect+TLS+upgrade phase.
 *
 * Returns NULL on failure; wss_last_error() then describes why. */
wss_t *wss_connect(const char *host, int port, const char *path,
                   const char *subprotocols, int timeout_ms);

/* Send one binary/text message. Frames are always masked (client role) and
 * FIN=1: no fragmentation on the send path. Returns 0 on success, -1 on
 * error. Internally loops on SSL_ERROR_WANT_READ/WRITE, blocking at most the
 * handle's io timeout per iteration. */
int wss_send_bin(wss_t *h, const void *data, size_t len);
int wss_send_text(wss_t *h, const char *s);

/* Send a raw ping. The matching pong is reported to the callback with
 * WSS_OP_PONG only if the peer sends it before the next poll returns. */
int wss_send_ping(wss_t *h, const void *data, size_t len);

/* The event-loop pump.
 *
 * timeout_ms  -1 blocks until data arrives (up to the internal cap), 0 polls
 * without blocking.
 *
 * Returns >0 the number of frames processed, 0 if the timeout expired with
 * nothing to do, -1 on a fatal error, -2 if the peer closed (after the
 * callback has already received WSS_OP_CLOSE). After -1 or -2 call wss_close(). */
int wss_poll(wss_t *h, int timeout_ms, wss_msg_cb cb, void *user);

/* Send a close frame (best effort, 1000/going-away), then tear everything
 * down. Safe on NULL. */
void wss_close(wss_t *h);

/* Error detail. wss_error() returns the message captured on this handle (valid
 * while the handle lives); wss_last_error() returns the most recent error from
 * any handle, and stays valid after wss_close(). Neither is ever NULL. */
const char *wss_error(const wss_t *h);
const char *wss_last_error(void);

/* Phase timings measured by wss_connect(), in milliseconds. Any pointer may
 * be NULL. */
void wss_timing(const wss_t *h, int *tcp_ms, int *tls_ms, int *upgrade_ms,
                int *total_ms);

/* Subprotocol chosen by the server, or "" if none was negotiated. */
const char *wss_selected_subprotocol(const wss_t *h);

/* Underlying fd, for an external event loop that wants to register it. */
int wss_fd(const wss_t *h);

#ifdef __cplusplus
}
#endif
#endif /* WEBRTC_WSS_H */
