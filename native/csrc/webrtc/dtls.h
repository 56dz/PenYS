/* dtls.h -- DTLS 1.2 client for WebRTC data/media transport (no libsrtp).
 *
 * Scope: the client half of a DTLS-SRTP handshake, exactly what a WebRTC
 * "answerer" has to do before any RTP can flow:
 *
 *   1. dtls_client_create()   generate a self-signed certificate locally and
 *                            expose its fingerprint (the SDP answer carries
 *                            that fingerprint; the peer checks our cert
 *                            against it, we do NOT check theirs).
 *   2. dtls_handshake()       run the handshake over an already-connected UDP
 *                            socket, driving poll() and retransmissions.
 *   3. dtls_srtp_keys()       export EXTRACTOR-dtls_srtp keying material and
 *                            split it per RFC 5764 section 4.2.
 *
 * Everything happens on the caller's thread. dtls_handshake() blocks for at
 * most timeout_ms and needs no timer thread, because OpenSSL's DTLS retransmit
 * timer is polled through SSL_ctrl(DTLS_CTRL_*), not via a callback.
 *
 * NOTE on the transported key sizes: for SRTP_AES128_CM_SHA1_80 the master
 * key is 16 bytes and the master SALT is 14 bytes (112 bits), not 12.
 * 12 bytes is the salt of AES_128_GCM profiles in other suites. The 60-byte
 * export splits as 16+16+14+14.
 */
#ifndef WEBRTC_DTLS_H
#define WEBRTC_DTLS_H

#include <stddef.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Certificate key algorithm. Measured on the RK3562 (4x A35):
 * ECDSA P-256 ~1 ms, RSA-2048 ~100-400 ms. Prefer ECDSA unless the peer is
 * known to choke on it; both are wired identically. */
#define DTLS_KEY_ECDSA_P256 0
#define DTLS_KEY_RSA2048    1

/* SRTP protection profiles we can negotiate (RFC 5764 registry ids). */
#define DTLS_SRTP_AES128_CM_SHA1_80   0x0001
#define DTLS_SRTP_AES128_CM_SHA1_32   0x0002
#define DTLS_SRTP_NULL_SHA1_80        0x0005

typedef struct dtls_client dtls_client_t;

typedef struct {
    unsigned char client_write_key[16];
    unsigned char server_write_key[16];
    unsigned char client_write_salt[14];
    unsigned char server_write_salt[14];
    /* Raw 60-byte export, kept for debugging against the peer's log. */
    unsigned char keying_material[60];
    unsigned int profile_id;        /* DTLS_SRTP_* */
    char profile_name[64];          /* e.g. "SRTP_AES128_CM_SHA1_80" */
} dtls_srtp_keys_t;

/* Create the client context and its self-signed certificate.
 * key_alg is DTLS_KEY_ECDSA_P256 or DTLS_KEY_RSA2048. Returns NULL on failure
 * (dtls_last_error() explains). The certificate is valid for one year and
 * carries CN=webrtc; nothing about it needs to be trusted, only hashed. */
dtls_client_t *dtls_client_create(int key_alg);

/* Same, but with an explicit SRTP profile list (colon or comma separated, in
 * preference order, e.g. "SRTP_AES128_CM_SHA1_80"). NULL = the default
 * "SRTP_AES128_CM_SHA1_80". Must be called before dtls_handshake(); OpenSSL
 * only puts use_srtp in the ClientHello if the profile is set on the CTX
 * before SSL_new(). */
dtls_client_t *dtls_client_create_ex(int key_alg, const char *srtp_profiles);

/* The certificate fingerprint exactly as it belongs in SDP:
 *   "sha-256 AB:CD:EF:..." (uppercase hex, colon separated).
 * The returned pointer belongs to the handle. */
const char *dtls_local_fingerprint(const dtls_client_t *h);

/* Fingerprint algorithm name, "sha-256". */
const char *dtls_fingerprint_algorithm(const dtls_client_t *h);

/* The local certificate's key algorithm in SDP terms: "ecdsa" or "rsa". */
const char *dtls_local_key_type(const dtls_client_t *h);

/* Milliseconds spent generating the certificate (for boot-time budgeting). */
int dtls_cert_gen_ms(const dtls_client_t *h);

/* Run the client handshake.
 *
 * udp_fd       a UDP socket. If it is not connected yet, this connects it to
 *              peer/peerlen (required: OpenSSL's dgram BIO needs a 5-tuple).
 * peer         destination address, may be NULL when the socket is already
 *              connected.
 * timeout_ms   overall budget. On expiry the handshake fails and the caller
 *              may retry with a fresh call.
 *
 * Returns 0 on success, negative on failure. */
int dtls_handshake(dtls_client_t *h, int udp_fd,
                   const struct sockaddr *peer, socklen_t peerlen,
                   int timeout_ms);

/* Retrieve the exported SRTP keys. Only valid after a successful handshake.
 * Returns 0 on success, -1 if the handshake is incomplete, -2 if the peer
 * selected no SRTP profile (then no media can be protected). */
int dtls_srtp_keys(dtls_client_t *h, dtls_srtp_keys_t *out);

/* Negotiated TLS cipher suite, e.g. "ECDHE-ECDSA-AES128-GCM-SHA256". May be
 * NULL before the handshake completes. */
const char *dtls_cipher_name(const dtls_client_t *h);

/* Negotiated protocol version string, e.g. "DTLSv1.2". */
const char *dtls_version(const dtls_client_t *h);

/* The peer's certificate fingerprint, or NULL if the peer sent none.
 * This is informational only: by design we do not validate it, because the
 * SDP offer of a cloud-gaming service is delivered over a channel we already
 * trust (the WSS control connection). */
const char *dtls_peer_fingerprint(const dtls_client_t *h);
int dtls_peer_had_cert(const dtls_client_t *h);

/* Number of handshake retransmissions performed (diagnostics). */
int dtls_retransmit_count(const dtls_client_t *h);

/* Human-readable detail for the last failure. Never NULL. */
const char *dtls_last_error(void);

/* Free the handle. Safe on NULL. */
void dtls_client_destroy(dtls_client_t *h);

#ifdef __cplusplus
}
#endif
#endif /* WEBRTC_DTLS_H */
