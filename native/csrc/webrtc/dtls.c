/* dtls.c -- DTLS 1.2 client + DTLS-SRTP key export for WebRTC (no libsrtp).
 *
 * Derived from the verified probe (.tmp/feas/srtp/dtls_probe.c, ran on the
 * device) and extended to a poll()-driven, non-blocking handshake because the
 * shipped libssl.so.1.1 does NOT export DTLSv1_get_timeout/DTLSv1_handle_timeout.
 *
 * The four device landmines inherited from the probe:
 *   1. BIO_new_dgram() does not learn the peer. connect(2) alone is not enough:
 *      OpenSSL then sendto()s with an AF_UNSPEC sockaddr and the kernel returns
 *      EINVAL. We must set BIO_CTRL_DGRAM_SET_PEER *and*
 *      BIO_CTRL_DGRAM_SET_CONNECTED ourselves.
 *   2. SSL_CTX_set_tlsext_use_srtp() must run BEFORE SSL_new(), otherwise the
 *      use_srtp extension never enters the ClientHello and the peer selects
 *      nothing.
 *   3. SRTP_PROTECTION_PROFILE is {const char *name; unsigned long id;} --
 *      name first. Getting this wrong reads a bogus pointer and segfaults.
 *   4. There is no SSL_get_cipher symbol; use SSL_get_version + the cipher.
 *
 * Non-blocking handshake: SSL_do_handshake() is retried on WANT_READ/WANT_WRITE
 * and the wait is taken from SSL_ctrl(ssl, DTLS_CTRL_GET_TIMEOUT). SSL_ctrl
 * forwards unknown commands to s->method->ssl_ctrl (dtls1_ctrl), which does
 * implement 73/74 even though the wrapper symbols are missing. When the timer
 * fires we call SSL_ctrl(ssl, DTLS_CTRL_HANDLE_TIMEOUT) to retransmit.
 */
#include "dtls.h"
#include "ossl_shim.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SRTP_EXTRACTOR_LABEL   "EXTRACTOR-dtls_srtp"
#define SRTP_EXTRACTOR_LEN     60      /* 16+16+14+14 for AES128_CM_SHA1_80 */
#define SRTP_DEFAULT_PROFILES  "SRTP_AES128_CM_SHA1_80"
#define CERT_VALID_SECONDS     (365L * 24L * 3600L)
#define MAX_HANDSHAKE_RETX     24
/* SHA-256 output size; the shim does not declare EVP_MAX_MD_SIZE. */
#define EVP_MAX_MD_SIZE_       64

struct dtls_client {
    SSL_CTX *ctx;
    SSL *ssl;
    X509 *cert;
    EVP_PKEY *pkey;
    int key_alg;
    int cert_gen_ms;
    int retransmits;
    int have_peer_cert;
    char fp[128];          /* our  "sha-256 AB:CD:.." */
    char peer_fp[128];
    char cipher[80];
    char err[256];
};

static char g_last_error[256] = "no error";

static void set_err(dtls_client_t *h, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->err, sizeof(h->err), fmt, ap);
    va_end(ap);
    snprintf(g_last_error, sizeof(g_last_error), "%s", h->err);
}

/* Drain the OpenSSL error queue into h->err (caller already set a headline). */
static void append_ossl_errors(dtls_client_t *h)
{
    unsigned long e;
    char buf[160];
    size_t used = strlen(h->err);
    int n = 0;

    while ((e = ERR_get_error()) != 0 && n < 3) {
        ERR_error_string_n(e, buf, sizeof(buf));
        used += (size_t)snprintf(h->err + used, sizeof(h->err) - used,
                                 "; ossl: %s", buf);
        if (used >= sizeof(h->err))
            break;
        n++;
    }
    ERR_clear_error();
    snprintf(g_last_error, sizeof(g_last_error), "%s", h->err);
}

static int ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* "sha-256 AA:BB:..." exactly as SDP wants it. */
static void format_fingerprint(const unsigned char *md, unsigned int mdlen,
                               char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t p = 0;
    unsigned int i;

    p += (size_t)snprintf(out, outsz, "sha-256 ");
    for (i = 0; i < mdlen && p + 3 < outsz; i++) {
        out[p++] = hex[(md[i] >> 4) & 0xF];
        out[p++] = hex[md[i] & 0xF];
        if (i + 1 < mdlen)
            out[p++] = ':';
    }
    out[p] = '\0';
}

/* ------------------------------------------------------------- certificate */

/* Self-signed cert. ECDSA P-256 takes ~1 ms on the RK3562; RSA-2048 takes
 * hundreds of ms, which matters at session start. Both are exposed because a
 * peer that cannot do ECDSA is easier to satisfy than a client that stalls. */
static X509 *make_cert(dtls_client_t *h, EVP_PKEY **out_pkey)
{
    EVP_PKEY *pkey = NULL;
    X509 *x = NULL;
    X509_NAME *nm;

    if (h->key_alg == DTLS_KEY_RSA2048) {
        RSA *rsa = RSA_new();
        BIGNUM *e = BN_new();
        if (!rsa || !e) {
            set_err(h, "RSA alloc failed");
            return NULL;
        }
        BN_set_word(e, RSA_F4);
        if (RSA_generate_key_ex(rsa, 2048, e, NULL) != 1) {
            set_err(h, "RSA_generate_key_ex(2048) failed");
            BN_free(e);
            RSA_free(rsa);
            append_ossl_errors(h);
            return NULL;
        }
        BN_free(e);
        pkey = EVP_PKEY_new();
        if (!pkey || EVP_PKEY_assign(pkey, EVP_PKEY_RSA, rsa) != 1) {
            set_err(h, "EVP_PKEY_assign(RSA) failed");
            RSA_free(rsa);
            EVP_PKEY_free(pkey);
            return NULL;
        }
    } else {
        EC_KEY *ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        if (!ec) {
            set_err(h, "EC_KEY_new_by_curve_name(P-256) failed");
            return NULL;
        }
        if (EC_KEY_generate_key(ec) != 1) {
            set_err(h, "EC_KEY_generate_key failed");
            EC_KEY_free(ec);
            append_ossl_errors(h);
            return NULL;
        }
        /* Named curve, not explicit parameters: smaller cert, and browsers /
         * SFUs accept it far more readily. */
        EC_KEY_set_asn1_flag(ec, OPENSSL_EC_NAMED_CURVE);
        pkey = EVP_PKEY_new();
        if (!pkey || EVP_PKEY_assign(pkey, EVP_PKEY_EC, ec) != 1) {
            set_err(h, "EVP_PKEY_assign(EC) failed");
            EC_KEY_free(ec);
            EVP_PKEY_free(pkey);
            return NULL;
        }
    }

    x = X509_new();
    if (!x) {
        set_err(h, "X509_new failed");
        EVP_PKEY_free(pkey);
        return NULL;
    }
    X509_set_version(x, 2);                     /* v3 */
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);   /* tolerate clock skew */
    X509_gmtime_adj(X509_getm_notAfter(x), CERT_VALID_SECONDS);
    X509_set_pubkey(x, pkey);
    nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               (const unsigned char *)"webrtc", -1, -1, 0);
    X509_set_issuer_name(x, nm);                /* self-signed */
    if (X509_sign(x, pkey, EVP_sha256()) == 0) {
        set_err(h, "X509_sign failed");
        append_ossl_errors(h);
        X509_free(x);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    *out_pkey = pkey;
    return x;
}

/* ------------------------------------------------------------------ create */

dtls_client_t *dtls_client_create_ex(int key_alg, const char *srtp_profiles)
{
    dtls_client_t *h;
    unsigned char md[EVP_MAX_MD_SIZE_];
    unsigned int mdlen = 0;
    int t0;

    if (!srtp_profiles)
        srtp_profiles = SRTP_DEFAULT_PROFILES;

    h = (dtls_client_t *)calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->key_alg = key_alg;

    OPENSSL_init_ssl(0, NULL);

    h->ctx = SSL_CTX_new(DTLS_client_method());
    if (!h->ctx) {
        set_err(h, "SSL_CTX_new(DTLS_client_method) failed");
        goto fail;
    }
    /* We never authenticate the server by certificate. The offer arrived over
     * the WSS control channel, which is already authenticated by TLS; the
     * fingerprint we publish is what lets *the server* check *us*. */
    SSL_CTX_set_verify(h->ctx, SSL_VERIFY_NONE, NULL);

    t0 = ms_now();
    h->cert = make_cert(h, &h->pkey);
    h->cert_gen_ms = ms_now() - t0;
    if (!h->cert)
        goto fail;

    if (SSL_CTX_use_certificate(h->ctx, h->cert) != 1 ||
        SSL_CTX_use_PrivateKey(h->ctx, h->pkey) != 1) {
        set_err(h, "SSL_CTX_use_certificate/use_PrivateKey failed");
        append_ossl_errors(h);
        goto fail;
    }
    if (SSL_CTX_check_private_key(h->ctx) != 1) {
        set_err(h, "SSL_CTX_check_private_key failed");
        goto fail;
    }

    /* LANDMINE 2: must be on the CTX before SSL_new(). */
    if (SSL_CTX_set_tlsext_use_srtp(h->ctx, srtp_profiles) != 0) {
        set_err(h, "SSL_CTX_set_tlsext_use_srtp(\"%s\") failed", srtp_profiles);
        goto fail;
    }

    if (X509_digest(h->cert, EVP_sha256(), md, &mdlen) != 1 || mdlen != 32) {
        set_err(h, "X509_digest(sha256) failed");
        append_ossl_errors(h);
        goto fail;
    }
    format_fingerprint(md, mdlen, h->fp, sizeof(h->fp));
    return h;

fail:
    dtls_client_destroy(h);
    return NULL;
}

dtls_client_t *dtls_client_create(int key_alg)
{
    return dtls_client_create_ex(key_alg, SRTP_DEFAULT_PROFILES);
}

const char *dtls_local_fingerprint(const dtls_client_t *h)
{
    return h ? h->fp : "";
}

const char *dtls_fingerprint_algorithm(const dtls_client_t *h)
{
    (void)h;
    return "sha-256";
}

const char *dtls_local_key_type(const dtls_client_t *h)
{
    if (!h)
        return "";
    return h->key_alg == DTLS_KEY_RSA2048 ? "rsa" : "ecdsa";
}

int dtls_cert_gen_ms(const dtls_client_t *h)
{
    return h ? h->cert_gen_ms : -1;
}

/* -------------------------------------------------------------- handshake */

static int sockaddr_set_peer(BIO *bio, int fd, dtls_client_t *h)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);

    if (getpeername(fd, (struct sockaddr *)&ss, &sl) < 0) {
        set_err(h, "getpeername failed: %s", strerror(errno));
        return -1;
    }
    /* LANDMINE 1: both controls, with a real sockaddr of matching family. */
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_PEER, 0, &ss);
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &ss);
    return 0;
}

int dtls_handshake(dtls_client_t *h, int udp_fd,
                   const struct sockaddr *peer, socklen_t peerlen,
                   int timeout_ms)
{
    BIO *bio;
    struct timeval tv;
    struct pollfd pfd;
    int deadline, orig_flags, rc, err, wait_ms, timed_out = 0;

    if (!h) {
        snprintf(g_last_error, sizeof(g_last_error), "null handle");
        return -1;
    }
    if (h->ssl) {
        set_err(h, "handshake already attempted on this handle");
        return -1;
    }
    if (udp_fd < 0) {
        set_err(h, "invalid udp fd");
        return -1;
    }

    /* The dgram BIO needs a connected 5-tuple. */
    if (peer && peerlen > 0) {
        struct sockaddr_storage cur;
        socklen_t cl = sizeof(cur);
        if (getpeername(udp_fd, (struct sockaddr *)&cur, &cl) < 0) {
            if (connect(udp_fd, peer, peerlen) < 0) {
                set_err(h, "connect(udp) failed: %s", strerror(errno));
                return -1;
            }
        }
    }

    bio = BIO_new_dgram(udp_fd, BIO_NOCLOSE);
    if (!bio) {
        set_err(h, "BIO_new_dgram failed");
        append_ossl_errors(h);
        return -1;
    }
    if (sockaddr_set_peer(bio, udp_fd, h) < 0) {
        BIO_free(bio);
        return -1;
    }

    h->ssl = SSL_new(h->ctx);       /* SRTP profile already on the CTX */
    if (!h->ssl) {
        set_err(h, "SSL_new failed");
        append_ossl_errors(h);
        BIO_free(bio);
        return -1;
    }
    SSL_set_bio(h->ssl, bio, bio); /* same object: SSL_set_bio refcounts once */
    SSL_set_connect_state(h->ssl);

    /* Drive the handshake with poll(); the socket must not block or our
     * timeout budget becomes a kernel SO_RCVTIMEO guess. */
    orig_flags = fcntl(udp_fd, F_GETFL, 0);
    if (orig_flags >= 0)
        fcntl(udp_fd, F_SETFL, orig_flags | O_NONBLOCK);

    deadline = ms_now() + (timeout_ms > 0 ? timeout_ms : 10000);

    for (;;) {
        int now = ms_now();

        rc = SSL_do_handshake(h->ssl);
        if (rc == 1)
            break;

        err = SSL_get_error(h->ssl, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (now >= deadline) {
                timed_out = 1;
                break;
            }
            /* Ask OpenSSL how long until its DTLS retransmit timer fires. */
            wait_ms = -1;
            memset(&tv, 0, sizeof(tv));
            if (SSL_ctrl(h->ssl, DTLS_CTRL_GET_TIMEOUT, 0, &tv) == 1) {
                wait_ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
                if (wait_ms < 0)
                    wait_ms = 0;
                if (wait_ms > 1000)     /* never sleep on a stale timer */
                    wait_ms = 1000;
            } else {
                wait_ms = 1000;
            }
            if (now + wait_ms > deadline)
                wait_ms = deadline - now;
            if (wait_ms < 0)
                wait_ms = 0;

            pfd.fd = udp_fd;
            pfd.events = (err == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;
            pfd.revents = 0;
            rc = poll(&pfd, 1, wait_ms);
            if (rc < 0) {
                if (errno == EINTR)
                    continue;
                set_err(h, "poll failed: %s", strerror(errno));
                goto bail;
            }
            if (rc == 0) {
                /* Timer expired with nothing to read: retransmit. */
                if (h->retransmits >= MAX_HANDSHAKE_RETX) {
                    set_err(h, "handshake gave up after %d retransmissions",
                            h->retransmits);
                    goto bail;
                }
                if (SSL_ctrl(h->ssl, DTLS_CTRL_HANDLE_TIMEOUT, 0, NULL) < 0) {
                    set_err(h, "handshake retransmit failed");
                    append_ossl_errors(h);
                    goto bail;
                }
                h->retransmits++;
            }
            continue;
        }

        if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK
                                         || errno == EINTR))
            continue;

        set_err(h, "handshake failed rc=%d ssl_err=%d errno=%d (%s)",
                rc, err, errno, strerror(errno));
        append_ossl_errors(h);
        goto bail;
    }

    if (orig_flags >= 0)
        fcntl(udp_fd, F_SETFL, orig_flags);

    if (timed_out || rc != 1) {
        set_err(h, "handshake timed out after %d ms (%d retransmissions)",
                timeout_ms, h->retransmits);
        return -1;
    }

    {
        const char *v = SSL_get_version(h->ssl);
        const SSL_CIPHER *c = SSL_get_current_cipher(h->ssl);
        snprintf(h->cipher, sizeof(h->cipher), "%s",
                 c ? SSL_CIPHER_get_name(c) : "(none)");
        if (v)
            snprintf(h->err, sizeof(h->err), "ok %s", v);
    }

    /* Informational only -- we deliberately do not validate the peer chain. */
    {
        X509 *peer_cert = SSL_get_peer_certificate(h->ssl);
        if (peer_cert) {
            unsigned char pmd[EVP_MAX_MD_SIZE_];
            unsigned int plen = 0;
            h->have_peer_cert = 1;
            if (X509_digest(peer_cert, EVP_sha256(), pmd, &plen) == 1)
                format_fingerprint(pmd, plen, h->peer_fp, sizeof(h->peer_fp));
            X509_free(peer_cert);
        }
    }
    return 0;

bail:
    if (orig_flags >= 0)
        fcntl(udp_fd, F_SETFL, orig_flags);
    return -1;
}

/* ------------------------------------------------------------------- keys */

int dtls_srtp_keys(dtls_client_t *h, dtls_srtp_keys_t *out)
{
    SRTP_PROTECTION_PROFILE *p;

    if (!h || !h->ssl) {
        snprintf(g_last_error, sizeof(g_last_error), "no handshake");
        return -1;
    }
    if (!out) {
        snprintf(g_last_error, sizeof(g_last_error), "null out");
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (!SSL_export_keying_material(h->ssl, out->keying_material,
                                    SRTP_EXTRACTOR_LEN,
                                    SRTP_EXTRACTOR_LABEL,
                                    sizeof(SRTP_EXTRACTOR_LABEL) - 1,
                                    NULL, 0, 0)) {
        set_err(h, "SSL_export_keying_material(%s) failed",
                SRTP_EXTRACTOR_LABEL);
        append_ossl_errors(h);
        return -1;
    }

    p = SSL_get_selected_srtp_profile(h->ssl);
    /* LANDMINE 3: name is the FIRST field. */
    if (p) {
        out->profile_id = (unsigned int)p->id;
        snprintf(out->profile_name, sizeof(out->profile_name), "%s",
                 p->name ? p->name : "(unnamed)");
    }

    /* RFC 5764 4.2 split order: client key, server key, client salt,
     * server salt. For AES128_CM_SHA1_80 that is 16/16/14/14. */
    memcpy(out->client_write_key, out->keying_material + 0, 16);
    memcpy(out->server_write_key, out->keying_material + 16, 16);
    memcpy(out->client_write_salt, out->keying_material + 32, 14);
    memcpy(out->server_write_salt, out->keying_material + 46, 14);

    if (!p) {
        set_err(h, "peer selected no SRTP protection profile");
        return -2;
    }
    return 0;
}

/* ------------------------------------------------------------- accessors */

const char *dtls_cipher_name(const dtls_client_t *h)
{
    return h && h->cipher[0] ? h->cipher : "";
}

const char *dtls_version(const dtls_client_t *h)
{
    if (!h || !h->ssl)
        return "";
    {
        const char *v = SSL_get_version(h->ssl);
        return v ? v : "";
    }
}

const char *dtls_peer_fingerprint(const dtls_client_t *h)
{
    return h && h->have_peer_cert ? h->peer_fp : NULL;
}

int dtls_peer_had_cert(const dtls_client_t *h)
{
    return h ? h->have_peer_cert : 0;
}

int dtls_retransmit_count(const dtls_client_t *h)
{
    return h ? h->retransmits : 0;
}

const char *dtls_last_error(void)
{
    return g_last_error;
}

void dtls_client_destroy(dtls_client_t *h)
{
    if (!h)
        return;
    if (h->ssl) {
        SSL_free(h->ssl);       /* frees the BIO too (BIO_NOCLOSE: fd stays) */
        h->ssl = NULL;
    }
    if (h->ctx)
        SSL_CTX_free(h->ctx);
    if (h->cert)
        X509_free(h->cert);
    if (h->pkey)
        EVP_PKEY_free(h->pkey);
    free(h);
}
