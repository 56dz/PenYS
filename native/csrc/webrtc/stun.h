/* stun.h -- the minimum ICE/STUN needed to open the cloud-game media path.
 *
 * Why this exists: the service's media edge is an ICE-lite peer on a fixed
 * UDP port (dispatch's rtc_udp_port). It never answers a DTLS ClientHello that
 * arrives from an address it has not yet validated: it only learns our
 * NAT-translated address from an ICE connectivity check. A real browser gets
 * that for free from RTCPeerConnection; we have to send the check ourselves.
 *
 * Symptom without it: ClientHello is retransmitted six times over six seconds
 * and nothing ever comes back ("handshake timed out ... 6 retransmissions"),
 * on every server, no matter which port we pick.
 *
 * Scope: only Binding Request/Response with short-term credentials
 * (RFC 5389 section 10.1.2 + RFC 8445). No TURN, no allocation, no ICE
 * restarts, no trickle state machine -- one check and its reply.
 */
#ifndef WEBRTC_STUN_H
#define WEBRTC_STUN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STUN_MSG_BINDING_REQUEST   0x0001
#define STUN_MSG_BINDING_RESPONSE  0x0101
#define STUN_MSG_BINDING_ERROR     0x0111
#define STUN_MAGIC_COOKIE          0x2112A442u
#define STUN_TXID_LEN              12
#define STUN_MAX_MSG               512

/* Everything a single check can tell us. */
typedef struct {
    int  msg_type;          /* STUN_MSG_* actually received, 0 = unparsable */
    int  error_code;        /* from ERROR-CODE, 0 when absent               */
    char mapped_ip[48];     /* XOR-MAPPED-ADDRESS as text, "" when absent   */
    int  mapped_port;       /* 0 when absent                                */
    int  has_integrity;     /* MESSAGE-INTEGRITY attribute present          */
} stun_result_t;

/* Build a Binding Request.
 *
 *   remote_ufrag  the peer's ice-ufrag (from its offer)
 *   local_ufrag   ours (from the answer we sent)
 *   remote_pwd    the peer's ice-pwd; NULL/"" skips MESSAGE-INTEGRITY, which
 *                 an ICE-lite peer is allowed to reject outright
 *   tie_breaker   8 bytes, constant for the whole session (ICE-CONTROLLING)
 *   txid          12 random bytes chosen by the caller; echoed in the message
 *                 so the reply can be matched
 *
 * Returns the message length, or -1 when `cap` is too small. */
int stun_build_binding_request(const char *remote_ufrag, const char *local_ufrag,
                               const char *remote_pwd,
                               const unsigned char tie_breaker[8],
                               const unsigned char txid[STUN_TXID_LEN],
                               unsigned char *out, size_t cap);

/* Parse one datagram. Returns 0 when it is a STUN message carrying our txid
 * (then *out is filled), -1 otherwise -- including plain RTP, which shares the
 * same socket. */
int stun_parse_response(const unsigned char *pkt, size_t n,
                        const unsigned char txid[STUN_TXID_LEN],
                        stun_result_t *out);

/* One-line description of a result, for logs. Always NUL terminated. */
void stun_result_str(const stun_result_t *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WEBRTC_STUN_H */
