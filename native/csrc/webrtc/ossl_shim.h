/* ossl_shim.h -- hand-written OpenSSL 1.1.1 declarations for the X6 Pro.
 *
 * The device ships libssl.so.1.1 / libcrypto.so.1.1 but NO headers, and a
 * number of symbols are absent from the .so files (see NOTES at the bottom).
 * This is an EXTENSION of .tmp/feas/srtp/ossl_min.h (that file stays untouched)
 * adding everything the wss/dtls/sdp/stun modules need.
 *
 * ABI rule: opaque structs are only ever used through pointers.
 */
#ifndef WEBRTC_OSSL_SHIM_H
#define WEBRTC_OSSL_SHIM_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- opaque */
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ssl_method_st SSL_METHOD;
typedef struct ssl_cipher_st SSL_CIPHER;
typedef struct bio_st BIO;
typedef struct bio_method_st BIO_METHOD;
typedef struct x509_st X509;
typedef struct x509_name_st X509_NAME;
typedef struct x509_store_ctx_st X509_STORE_CTX;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_md_st EVP_MD;
typedef struct rsa_st RSA;
typedef struct bignum_st BIGNUM;
typedef struct ec_key_st EC_KEY;
typedef struct asn1_string_st ASN1_INTEGER;
typedef struct asn1_string_st ASN1_TIME;
typedef struct srtp_protection_profile_st {
    const char *name;
    unsigned long id;
} SRTP_PROTECTION_PROFILE;

/* -------------------------------------------------------------- constants */
#define BIO_NOCLOSE              0x00
#define BIO_CLOSE                0x01
#define SSL_VERIFY_NONE          0x00
#define SSL_VERIFY_PEER          0x01
#define SSL_VERIFY_FAIL_IF_NO_PEER_CERT 0x02

#define RSA_F4                   0x10001
#define MBSTRING_ASC             0x1000
#define NID_X9_62_prime256v1     415
#define EVP_PKEY_RSA             6
#define EVP_PKEY_EC              408
#define OPENSSL_EC_NAMED_CURVE   0x0040

#define SSL_ERROR_NONE           0
#define SSL_ERROR_SSL            1
#define SSL_ERROR_WANT_READ      2
#define SSL_ERROR_WANT_WRITE     3
#define SSL_ERROR_SYSCALL        5
#define SSL_ERROR_ZERO_RETURN    6

#define SSL_CTRL_SET_MTU                 17
#define SSL_CTRL_SET_TLSEXT_HOSTNAME     55
#define SSL_OP_NO_QUERY_MTU              0x00001000U
#define SSL_OP_NO_TICKET                 0x00004000U
#define SSL_OP_NO_COMPRESSION            0x00020000U
#define SSL_OP_NO_RENEGOTIATION          0x40000000U

#define BIO_CTRL_FLUSH                   11
#define BIO_CTRL_DGRAM_SET_RECV_TIMEOUT  33
#define BIO_CTRL_DGRAM_SET_SEND_TIMEOUT  35
#define BIO_CTRL_DGRAM_GET_RECV_TIMER_EXP 37
#define BIO_CTRL_DGRAM_SET_PEER          44
#define BIO_CTRL_DGRAM_SET_CONNECTED     32
#define BIO_CTRL_DGRAM_GET_MTU           41
#define BIO_CTRL_DGRAM_SET_MTU           42
#define BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT  45

/* DTLS controls are NOT handled by SSL_ctrl()'s own switch -- it forwards the
 * unknown cmd to s->method->ssl_ctrl (== dtls1_ctrl for DTLS_method), which
 * implements these two. So SSL_ctrl(ssl, 73/74, ...) works even though the
 * DTLSv1_get_timeout()/DTLSv1_handle_timeout() symbols are missing from the
 * shipped .so. */
#define DTLS_CTRL_GET_TIMEOUT            73
#define DTLS_CTRL_HANDLE_TIMEOUT         74

/* --------------------------------------------------------------- methods */
const SSL_METHOD *DTLS_method(void);
const SSL_METHOD *DTLS_client_method(void);
const SSL_METHOD *DTLS_server_method(void);
const SSL_METHOD *TLS_method(void);
const SSL_METHOD *TLS_client_method(void);
const SSL_METHOD *TLS_server_method(void);

int OPENSSL_init_ssl(unsigned long, const void *);
int OPENSSL_init_crypto(unsigned long, const void *);

/* ------------------------------------------------------------------- ctx */
SSL_CTX *SSL_CTX_new(const SSL_METHOD *);
void SSL_CTX_free(SSL_CTX *);
void SSL_CTX_set_verify(SSL_CTX *, int, int (*)(int, X509_STORE_CTX *));
unsigned long SSL_CTX_set_options(SSL_CTX *, unsigned long);
int SSL_CTX_set_cipher_list(SSL_CTX *, const char *);
int SSL_CTX_set_tlsext_use_srtp(SSL_CTX *, const char *);
int SSL_CTX_use_certificate(SSL_CTX *, X509 *);
int SSL_CTX_use_PrivateKey(SSL_CTX *, EVP_PKEY *);
int SSL_CTX_check_private_key(const SSL_CTX *);
void SSL_CTX_set_security_level(SSL_CTX *, int);

/* ------------------------------------------------------------------- ssl */
SSL *SSL_new(SSL_CTX *);
void SSL_free(SSL *);
void SSL_set_bio(SSL *, BIO *, BIO *);
/* SSL_get_rbio/SSL_get_wbio are macros in 1.1.1 (they touch struct fields),
 * so they are NOT callable here. Use SSL_get_fd() instead. */
void SSL_set_connect_state(SSL *);
void SSL_set_accept_state(SSL *);
int SSL_do_handshake(SSL *);
int SSL_connect(SSL *);
int SSL_accept(SSL *);
int SSL_read(SSL *, void *, int);
int SSL_write(SSL *, const void *, int);
int SSL_pending(const SSL *);
int SSL_shutdown(SSL *);
int SSL_get_error(const SSL *, int);
int SSL_get_fd(const SSL *);
const char *SSL_get_version(const SSL *);
const SSL_CIPHER *SSL_get_current_cipher(const SSL *);
const char *SSL_CIPHER_get_name(const SSL_CIPHER *);
unsigned long SSL_CIPHER_get_id(const SSL_CIPHER *);
X509 *SSL_get_peer_certificate(const SSL *);
int SSL_set_tlsext_use_srtp(SSL *, const char *);
SRTP_PROTECTION_PROFILE *SSL_get_selected_srtp_profile(SSL *);
int SSL_export_keying_material(SSL *, unsigned char *, size_t, const char *,
                               size_t, const unsigned char *, size_t, int);
long SSL_ctrl(SSL *, int, long, void *);
unsigned long SSL_set_options(SSL *, unsigned long);

/* ------------------------------------------------------------------- bio */
BIO *BIO_new(const BIO_METHOD *);
BIO *BIO_new_dgram(int fd, int close_flag);   /* exported by libcrypto, not libssl */
BIO *BIO_new_socket(int fd, int close_flag);
int BIO_ctrl(BIO *, int, long, void *);
long BIO_int_ctrl(BIO *, int, int, long);
void BIO_free(BIO *);
void BIO_free_all(BIO *);
long BIO_read(BIO *, void *, int);
long BIO_write(BIO *, const void *, int);
size_t BIO_ctrl_pending(BIO *);
int BIO_test_flags(const BIO *, int);
int BIO_set_flags(BIO *, int);
int BIO_clear_flags(BIO *, int);
void BIO_set_data(BIO *, void *);
void *BIO_get_data(BIO *);
void BIO_set_init(BIO *, int);
void BIO_set_shutdown(BIO *, int);
#define BIO_FLAGS_READ        0x01
#define BIO_FLAGS_WRITE       0x02
#define BIO_FLAGS_IO_SPECIAL  0x04
#define BIO_FLAGS_RWS         (BIO_FLAGS_READ | BIO_FLAGS_WRITE | BIO_FLAGS_IO_SPECIAL)
#define BIO_FLAGS_SHOULD_RETRY 0x08

/* --------------------------------------------------------------- digests */
const EVP_MD *EVP_sha1(void);
const EVP_MD *EVP_sha256(void);
int SHA1(const unsigned char *d, size_t n, unsigned char *md);
int SHA256(const unsigned char *d, size_t n, unsigned char *md);
int RAND_bytes(unsigned char *, int);
int RAND_poll(void);

/* ------------------------------------------------------- keys / x509make */
EVP_PKEY *EVP_PKEY_new(void);
void EVP_PKEY_free(EVP_PKEY *);
int EVP_PKEY_assign(EVP_PKEY *, int, void *);
EC_KEY *EVP_PKEY_get1_EC_KEY(EVP_PKEY *);
const EC_KEY *EVP_PKEY_get0_EC_KEY(EVP_PKEY *);
int EVP_PKEY_id(const EVP_PKEY *);
int EVP_PKEY_bits(const EVP_PKEY *);

RSA *RSA_new(void);
void RSA_free(RSA *);
int RSA_generate_key_ex(RSA *, int, BIGNUM *, void *);
BIGNUM *BN_new(void);
void BN_free(BIGNUM *);
int BN_set_word(BIGNUM *, unsigned long);

EC_KEY *EC_KEY_new_by_curve_name(int);
void EC_KEY_free(EC_KEY *);
int EC_KEY_generate_key(EC_KEY *);
void EC_KEY_set_asn1_flag(EC_KEY *, int);

X509 *X509_new(void);
void X509_free(X509 *);
int X509_set_version(X509 *, long);
ASN1_INTEGER *X509_get_serialNumber(X509 *);
int ASN1_INTEGER_set(ASN1_INTEGER *, long);
int X509_set_pubkey(X509 *, EVP_PKEY *);
X509_NAME *X509_get_subject_name(X509 *);
int X509_set_issuer_name(X509 *, X509_NAME *);
int X509_NAME_add_entry_by_txt(X509_NAME *, const char *, int,
                               const unsigned char *, int, int, int);
int X509_sign(X509 *, EVP_PKEY *, const EVP_MD *);
int X509_digest(const X509 *, const EVP_MD *, unsigned char *, unsigned int *);
int i2d_X509(const X509 *, unsigned char **);
void X509_gmtime_adj(void * /*ASN1_TIME **/, long);
/* These ARE real exported functions in this build (unlike the usual 1.1.1
 * macro-only story), verified with `syms.py libcrypto.so.1.1`. */
void *X509_getm_notBefore(X509 *);
void *X509_getm_notAfter(X509 *);

/* ------------------------------------------------------------------ errs */
unsigned long ERR_get_error(void);
void ERR_error_string_n(unsigned long, char *, size_t);
void ERR_clear_error(void);
void ERR_print_errors_fp(void *);

#ifdef __cplusplus
}
#endif

/* NOTES -- symbols verified ABSENT from the shipped .so files:
 *   DTLSv1_get_timeout, DTLSv1_handle_timeout, SSL_CTX_set_read_ahead,
 *   SSL_CTX_set_min/max_proto_version, SSL_set_mode, BIO_get_mem_data,
 *   BIO_s_mem is present but BIO_new_socket lives in libcrypto.
 * Replacements used by this shim:
 *   DTLS timeouts   -> SSL_ctrl(ssl, 73/74, ...)  (see WEBRTC_DTLS_* in dtls.c)
 *   BIO_set_retry_* -> declared here; libcrypto exports the real functions.
 */
#endif /* WEBRTC_OSSL_SHIM_H */
