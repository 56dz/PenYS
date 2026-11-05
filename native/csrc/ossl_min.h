/* Minimal OpenSSL 1.1 declarations for cross-compiling against the device's
 * libssl.so.1.1 / libcrypto.so.1.1. The device ships no headers, so only the
 * symbols we actually call are declared here. ABI: opaque pointers only. */
#ifndef OSSL_MIN_H
#define OSSL_MIN_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ssl_method_st SSL_METHOD;
typedef struct bio_st BIO;
typedef struct bio_method_st BIO_METHOD;
typedef struct x509_st X509;
typedef struct x509_name_st X509_NAME;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct rsa_st RSA;
typedef struct bignum_st BIGNUM;
typedef struct asn1_string_st ASN1_INTEGER;
typedef struct evp_md_st EVP_MD;
typedef struct srtp_protection_profile_st { const char *name; unsigned long id; } SRTP_PROTECTION_PROFILE;

#define BIO_NOCLOSE 0x00
#define SSL_VERIFY_NONE 0x00
#define SSL_FILETYPE_PEM 1
#define RSA_F4 0x10001
#define MBSTRING_ASC 0x1000
#define X509_VERSION_3 2

const SSL_METHOD *DTLS_method(void);
const SSL_METHOD *DTLS_client_method(void);
const SSL_METHOD *DTLS_server_method(void);
SSL_CTX *SSL_CTX_new(const SSL_METHOD *);
void SSL_CTX_free(SSL_CTX *);
void SSL_CTX_set_verify(SSL_CTX *, int, int (*)(int, void *));
SSL *SSL_new(SSL_CTX *);
void SSL_free(SSL *);
void SSL_set_bio(SSL *, BIO *, BIO *);
void SSL_set_connect_state(SSL *);
void SSL_set_accept_state(SSL *);
int SSL_accept(SSL *);
int SSL_connect(SSL *);
int SSL_get_error(const SSL *, int);
const char *SSL_get_version(const SSL *);
const char *SSL_get_cipher(const SSL *);
SSL_CTX *SSL_get_SSL_CTX(const SSL *);
int SSL_read(SSL *, void *, int);
int SSL_write(SSL *, const void *, int);
int SSL_pending(const SSL *);
int SSL_CTX_set_tlsext_use_srtp(SSL_CTX *, const char *);
int SSL_set_tlsext_use_srtp(SSL *, const char *);
SRTP_PROTECTION_PROFILE *SSL_get_selected_srtp_profile(SSL *);
void *SSL_get_srtp_profiles(SSL *);
int SSL_export_keying_material(SSL *, unsigned char *, size_t, const char *, size_t,
                               const unsigned char *, size_t, int);
int SSL_shutdown(SSL *);
void SSL_CTX_set_cipher_list(SSL_CTX *, const char *);
long SSL_ctrl(SSL *, int, long, void *);

BIO *BIO_new(const BIO_METHOD *);
const BIO_METHOD *BIO_s_mem(void);
BIO *BIO_new_dgram(int, int);
int BIO_ctrl(BIO *, int, long, void *);
void BIO_free(BIO *);
long BIO_read(BIO *, void *, int);
long BIO_write(BIO *, const void *, int);

int RSA_generate_key_ex(RSA *, int, BIGNUM *, void *);
RSA *RSA_new(void);
void RSA_free(RSA *);
BIGNUM *BN_new(void);
int BN_set_word(BIGNUM *, unsigned long);
X509 *X509_new(void);
void X509_free(X509 *);
int X509_set_version(X509 *, long);
ASN1_INTEGER *X509_get_serialNumber(X509 *);
void *X509_getm_notBefore(X509 *);
void *X509_getm_notAfter(X509 *);
int OPENSSL_init_ssl(unsigned long, const void *);
int ASN1_INTEGER_set(ASN1_INTEGER *, long);
int X509_set_pubkey(X509 *, EVP_PKEY *);
X509_NAME *X509_get_subject_name(X509 *);
int X509_set_issuer_name(X509 *, X509_NAME *);
int X509_NAME_add_entry_by_txt(X509_NAME *, const char *, int, const unsigned char *, int, int, int);
int X509_sign(X509 *, EVP_PKEY *, const EVP_MD *);
/* X509_getm_notBefore/After are macros in 1.1; the real symbols are setters. */
void X509_gmtime_adj(void *, long);
EVP_PKEY *EVP_PKEY_new(void);
void EVP_PKEY_free(EVP_PKEY *);
int EVP_PKEY_assign(EVP_PKEY *, int, void *);
int EVP_PKEY_assign_RSA(EVP_PKEY *, RSA *);
const EVP_MD *EVP_sha256(void);
void ERR_print_errors_fp(void *);
unsigned long ERR_get_error(void);
void ERR_error_string_n(unsigned long, char *, size_t);
void ERR_clear_error(void);
int SSL_CTX_use_certificate(SSL_CTX *, X509 *);
int SSL_CTX_use_PrivateKey(SSL_CTX *, EVP_PKEY *);

#ifdef __cplusplus
}
#endif
#endif
