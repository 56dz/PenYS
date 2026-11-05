/* sdp.h -- SDP offer parser + answer generator for the cloud-gaming client.
 *
 * Role reminder: this stack is the ANSWERER. The service sends an offer over
 * the WSS signalling channel and we must produce an answer that
 *
 *   - keeps the offer's m-line order and mid values (BUNDLE matching),
 *   - advertises exactly one H.264 payload type and one Opus payload type,
 *   - carries OUR ice-ufrag / ice-pwd (we are the ICE controller),
 *   - carries OUR DTLS certificate fingerprint (dtls_local_fingerprint()),
 *   - sets a=setup:active, because we send the DTLS ClientHello,
 *   - keeps a=rtcp-mux if the offer had it.
 *
 * The parser is defensive: it never writes outside caller buffers, it accepts
 * both CRLF and LF, unknown attributes are preserved into the "unknown" list
 * only where that is cheap, and the whole offer is capped at SDP_MAX_LEN.
 *
 * Everything here is one-shot at session setup, so small allocations are fine;
 * nothing in this file allocates at steady state.
 */
#ifndef WEBRTC_SDP_H
#define WEBRTC_SDP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Codec bits for sdp_answer_cfg_t.codecs. */
#define SDP_CODEC_H264 0x01
#define SDP_CODEC_OPUS 0x02
#define SDP_CODEC_H265 0x04
#define SDP_CODEC_ALL  0x07

/* Direction, RFC 4566/3264. */
#define SDP_DIR_NONE      0      /* no direction attribute present */
#define SDP_DIR_SENDRECV  1
#define SDP_DIR_SENDONLY  2
#define SDP_DIR_RECVONLY  3
#define SDP_DIR_INACTIVE  4

/* a=setup value from the offer. */
#define SDP_SETUP_NONE    0
#define SDP_SETUP_ACTIVE  1
#define SDP_SETUP_PASSIVE 2
#define SDP_SETUP_ACTPASS 3

#define SDP_MAX_MEDIA    8
#define SDP_MAX_FORMATS  24
#define SDP_MAX_EXTMAPS  8
#define SDP_MAX_SSRCS    8
#define SDP_MAX_LEN      65536

typedef struct {
    int  id;
    char uri[144];
} sdp_extmap_t;

typedef struct {
    int  index;                     /* 0-based m-line index */
    char mid[64];
    char media[16];                 /* "video" | "audio" | "application" */
    char proto[48];                 /* "UDP/TLS/RTP/SAVPF" | "UDP/DTLS/SCTP" */
    int  port;
    int  rejected;                  /* port == 0 in the offer */
    int  payloads[SDP_MAX_FORMATS];
    int  n_payloads;
    int  has_rtcp_mux;
    int  direction;                 /* SDP_DIR_* */
    int  setup;                     /* SDP_SETUP_* at media level, 0 = none */
    int  is_application;            /* m=application (SCTP data channel) */
    char sctp_port[16];             /* a=sctp-port:n */
    char app_fmt[40];               /* m=application format token */

    /* H.264 (RFC 6184) detail. h264_pt is -1 when the offer has none. */
    int  h264_pt;
    char h264_profile_level_id[16]; /* e.g. "42e01f" */
    int  h264_packetization_mode;   /* 0/1/2, -1 unknown */
    char h264_fmtp[320];

    /* H.265 (RFC 7798), parsed only so we can echo it when enabled. */
    int  h265_pt;
    char h265_fmtp[320];

    /* Opus (RFC 7587). */
    int  opus_pt;
    int  opus_clock;
    int  opus_channels;
    char opus_fmtp[256];

    sdp_extmap_t extmaps[SDP_MAX_EXTMAPS];
    int  n_extmaps;

    unsigned int ssrcs[SDP_MAX_SSRCS];
    int  n_ssrcs;

    /* Media-level overrides; empty means "inherit from session". */
    char ice_ufrag[64];
    char ice_pwd[128];
    char fingerprint_alg[32];
    char fingerprint[160];
} sdp_media_t;

typedef struct sdp_offer sdp_offer_t;

/* Parse an offer. Returns NULL on a hard error (then err holds the reason).
 * A structurally valid SDP with zero usable codecs still returns a handle:
 * check sdp_media_count()/sdp_media_at(). */
sdp_offer_t *sdp_offer_parse(const char *sdp, char *err, size_t errcap);
void sdp_offer_free(sdp_offer_t *o);

/* Session level ---------------------------------------------------------- */
const char *sdp_session_fingerprint(const sdp_offer_t *o);   /* "AA:BB:.." */
const char *sdp_session_fingerprint_alg(const sdp_offer_t *o); /* "sha-256" */
const char *sdp_session_ice_ufrag(const sdp_offer_t *o);
const char *sdp_session_ice_pwd(const sdp_offer_t *o);
int         sdp_session_setup(const sdp_offer_t *o);          /* SDP_SETUP_* */
int         sdp_bundle_count(const sdp_offer_t *o);
const char *sdp_bundle_mid(const sdp_offer_t *o, int i);

/* Media level ------------------------------------------------------------ */
int                  sdp_media_count(const sdp_offer_t *o);
const sdp_media_t   *sdp_media_at(const sdp_offer_t *o, int i);
const sdp_media_t   *sdp_media_by_mid(const sdp_offer_t *o, const char *mid);
const sdp_media_t   *sdp_media_first_kind(const sdp_offer_t *o,
                                          const char *kind);

/* Which payload type of this media carries `codec` (SDP_CODEC_*), or -1. */
int sdp_media_pt_for(const sdp_media_t *m, int codec);

/* Human-readable one-line-per-media dump, for logs and the test harness.
 * Returns the number of bytes written (always NUL terminated). */
int sdp_offer_summary(const sdp_offer_t *o, char *out, size_t cap);

/* Answer generation ------------------------------------------------------ */

typedef struct {
    /* Our IPv4 address as a literal ("192.168.0.198"). Used for c= and a=rtcp.
     * "0.0.0.0" is legal and what browsers answer with when the candidate is
     * delivered separately; prefer the real address so the peer can reach us
     * without waiting for trickle ICE. */
    const char *local_ip;

    /* Full fingerprint INCLUDING the algorithm, as produced by
     * dtls_local_fingerprint(): "sha-256 AB:CD:...". Required. */
    const char *fingerprint;

    /* "active" (we initiate DTLS, the normal client case), "passive", or
     * "actpass". NULL = "active". */
    const char *setup;

    /* Bitmask of SDP_CODEC_* we are willing to receive. 0 = accept every
     * codec we understand. */
    int codecs;

    /* 1 = a=sendrecv, 0 = a=recvonly (default: recvonly, we send no media). */
    int sendrecv;

    /* Echo the offer's a=extmap lines. Default 0.
     *
     * Turning this ON makes the peer put extension headers into every RTP
     * packet. The RTP layer can skip them (they are length-delimited), but
     * only if it actually parses the extension block -- and declaring an
     * extension we then fail to honour is worse than not declaring it. Set to
     * 1 once the RTP module copies the extension block through correctly. */
    int echo_extmaps;

    /* Accept the offer's m=application (SCTP data channel) stream. Default 0:
     * this stack has no SCTP, so we reject it with port 0 and the client uses
     * the GameControlChannel (WSS :5443) for input instead. Set to 1 only if
     * an SCTP implementation is linked in. */
    int accept_datachannel;

    /* Optional single host candidate to embed in the answer. Set port 0 to
     * omit and send candidates over signalling instead (trickle). */
    const char *candidate_ip;
    int         candidate_port;

    /* Session id / version for o=. 0 = derive from wall clock. */
    unsigned long session_id;
    unsigned long session_version;

    /* Optional sinks for the ICE credentials we generated for this answer.
     * The ICE layer needs both: our ufrag forms the STUN USERNAME
     * ("remote_ufrag:local_ufrag"), our pwd verifies inbound responses.
     * NULL/0 = the caller does not need them. */
    char  *ice_ufrag_out;
    size_t ice_ufrag_out_cap;
    char  *ice_pwd_out;
    size_t ice_pwd_out_cap;
} sdp_answer_cfg_t;

/* Fill `cfg` with the documented defaults. Call this before setting the
 * fields you care about: a plain memset(0) leaves `setup` NULL and
 * `echo_extmaps` off, which happens to be correct today but would silently
 * change meaning if a default ever moves. */
void sdp_answer_cfg_init(sdp_answer_cfg_t *cfg);

/* Build the answer into `out` (NUL terminated). Returns the answer length on
 * success, -1 on error (err holds the reason). `out` must hold the offer's
 * length at least; a safe caller buffer is SDP_MAX_LEN.
 *
 * The generated answer always has exactly as many m-lines as the offer, in
 * the same order, with the same mid values: m-lines we refuse are emitted
 * with port 0 so BUNDLE numbering stays intact. */
int sdp_build_answer(const sdp_offer_t *offer, const sdp_answer_cfg_t *cfg,
                     char *out, size_t cap, char *err, size_t errcap);

/* Rewrite the ip:port of a candidate line to a fixed address.
 *
 * This is the "ICE degenerates to a direct connection" rule of the service:
 * every candidate the server sends is redirected to
 * game_svr_addr.ip / rtc_udp_port. `line` may or may not carry the leading
 * "a=" and "candidate:" prefix. Fields 4 and 5 (ip, port) are replaced, every
 * other byte is preserved verbatim, including the trailing " typ host ...".
 *
 * Returns 0 on success, -1 if the line does not look like a candidate.
 * `out` needs strlen(line)+1 bytes. */
int sdp_candidate_rewrite(const char *line, const char *ip, int port,
                          char *out, size_t cap);

/* Extract ip and port from a candidate line, for diagnostics.
 * Returns 0 on success. */
int sdp_candidate_parse(const char *line, char *ip, size_t ipcap,
                        int *port, char *foundation, size_t fcap,
                        char *type, size_t tcap);

#ifdef __cplusplus
}
#endif
#endif /* WEBRTC_SDP_H */
