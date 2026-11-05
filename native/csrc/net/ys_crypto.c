/* ys_crypto.c -- HMAC-SHA256, SHA256 and RSA-1024 public-key encryption.
 *
 * WHY NATIVE, NOT JS
 * The mini-app's package.json pins "quickjs": { "bigNum": false }, and the
 * device's libquickjs.so has no BigInt support compiled in. RSA PKCS#1 v1.5
 * needs 1024-bit modular exponentiation, so a pure-JS implementation is off the
 * table. HMAC-SHA256 is theoretically doable in JS (32-bit words only) but it
 * would run on the app thread every dispatch, so it lives here as well.
 *
 * Both are backed by the device's libcrypto.so.1.1, whose symbols were checked
 * on-device (.tmp/feas/crypto.syms): HMAC_CTX_new/HMAC_Init_ex/HMAC_Update/
 * HMAC_Final, SHA256_Init/Update/Final, PEM_read_bio_PUBKEY, EVP_PKEY_get1_RSA,
 * RSA_public_encrypt -- all present.
 *
 * Note: `SHA256()` (the one-shot) is NOT exported; only the Init/Update/Final
 * trio. HMAC_CTX_* are the 1.1 names, and EVP_MAC_* (3.x) does not exist.
 */
#include "ys_net.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------- libcrypto declarations ------------------------- */

typedef struct hmackey_st HmacCtx;   /* HMAC_CTX in 1.1 */
typedef struct evp_md_st EVP_MD;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct rsa_st RSA;
typedef struct bio_st BIO;
typedef struct bignum_st BIGNUM;
typedef struct rsa_meth_st RSA_METHOD;

extern HmacCtx *HMAC_CTX_new(void);
extern void HMAC_CTX_free(HmacCtx *);
extern int HMAC_Init_ex(HmacCtx *, const void *key, int keylen, const EVP_MD *, void *engine);
extern int HMAC_Update(HmacCtx *, const unsigned char *, size_t);
extern int HMAC_Final(HmacCtx *, unsigned char *, unsigned int *);
extern const EVP_MD *EVP_sha256(void);

typedef struct sha256_ctx_st {
    unsigned int h[8];
    unsigned int Nl, Nh;
    unsigned int data[16];
    unsigned int num, md_len;
} Sha256Ctx;
extern int SHA256_Init(Sha256Ctx *);
extern int SHA256_Update(Sha256Ctx *, const void *, size_t);
extern int SHA256_Final(unsigned char *, Sha256Ctx *);

extern BIO *BIO_new_mem_buf(const void *buf, int len);
extern void BIO_free(BIO *);
extern EVP_PKEY *PEM_read_bio_PUBKEY(BIO *, EVP_PKEY **, void *cb, void *u);
extern void EVP_PKEY_free(EVP_PKEY *);
extern int EVP_PKEY_base_id(const EVP_PKEY *);
extern RSA *EVP_PKEY_get1_RSA(EVP_PKEY *);
extern int RSA_public_encrypt(int flen, const unsigned char *from,
                              unsigned char *to, RSA *rsa, int padding);
extern void RSA_free(RSA *);
extern unsigned long ERR_get_error(void);
extern void ERR_error_string_n(unsigned long e, char *buf, size_t len);
extern void ERR_clear_error(void);

#define EVP_PKEY_RSA 6
#define RSA_PKCS1_PADDING 1

/* ------------------------------------------------------------------ base64 */

static const char B64TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *ys_b64_encode(const void *p, size_t n) {
    const unsigned char *s = (const unsigned char *)p;
    size_t outlen = ((n + 2) / 3) * 4;
    char *out = (char *)malloc(outlen + 1);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i + 2 < n) {
        unsigned v = ((unsigned)s[i] << 16) | ((unsigned)s[i + 1] << 8) | s[i + 2];
        out[o++] = B64TAB[(v >> 18) & 63];
        out[o++] = B64TAB[(v >> 12) & 63];
        out[o++] = B64TAB[(v >> 6) & 63];
        out[o++] = B64TAB[v & 63];
        i += 3;
    }
    if (i < n) {
        unsigned v = (unsigned)s[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2) v |= (unsigned)s[i + 1] << 8;
        out[o++] = B64TAB[(v >> 18) & 63];
        out[o++] = B64TAB[(v >> 12) & 63];
        out[o++] = rem == 2 ? B64TAB[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return out;
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

unsigned char *ys_b64_decode(const char *s, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!s) return NULL;
    size_t slen = strlen(s);
    unsigned char *out = (unsigned char *)malloc(slen / 4 * 3 + 4);
    if (!out) return NULL;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < slen; i++) {
        int v = b64val((unsigned char)s[i]);
        if (v < 0) {
            if (s[i] == '=') break;
            continue; /* skip newlines/spaces */
        }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    if (out_len) *out_len = o;
    return out;
}

/* ------------------------------------------------------------------- hex */

static char *hex_of(const unsigned char *d, size_t n) {
    static const char H[] = "0123456789abcdef";
    char *out = (char *)malloc(n * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = H[d[i] >> 4];
        out[i * 2 + 1] = H[d[i] & 15];
    }
    out[n * 2] = 0;
    return out;
}

/* ---------------------------------------------------------------- HMAC/SHA */

char *ys_hmac_sha256_hex(const char *key, size_t key_len,
                         const void *data, size_t data_len) {
    if (!key) return NULL;
    HmacCtx *ctx = HMAC_CTX_new();
    if (!ctx) return NULL;
    unsigned char mac[32];
    unsigned int maclen = 0;
    char *out = NULL;
    if (HMAC_Init_ex(ctx, key, (int)key_len, EVP_sha256(), NULL) == 1 &&
        HMAC_Update(ctx, (const unsigned char *)data, data_len) == 1 &&
        HMAC_Final(ctx, mac, &maclen) == 1 && maclen == 32) {
        out = hex_of(mac, 32);
    }
    HMAC_CTX_free(ctx);
    return out;
}

char *ys_sha256_hex(const void *data, size_t data_len) {
    Sha256Ctx ctx;
    if (SHA256_Init(&ctx) != 1) return NULL;
    if (SHA256_Update(&ctx, data, data_len) != 1) return NULL;
    unsigned char dig[32];
    if (SHA256_Final(dig, &ctx) != 1) return NULL;
    return hex_of(dig, 32);
}

/* -------------------------------------------------------------------- RSA */

static void set_err(char **err, const char *fmt, const char *arg, unsigned long code) {
    if (!err) return;
    char tmp[256];
    if (code) {
        char es[160];
        ERR_error_string_n(code, es, sizeof(es));
        snprintf(tmp, sizeof(tmp), fmt, arg ? arg : "", es);
    } else {
        snprintf(tmp, sizeof(tmp), fmt, arg ? arg : "", "");
    }
    free(*err);
    *err = strdup(tmp);
}

/* mihoyo passport RSA-1024 public key (X.509 SubjectPublicKeyInfo, base64).
 * Source: .tmp/yys/src/crypto.js, the four concatenated PUBLIC_KEY_B64 chunks.
 * Kept in native so the JS never carries a 216-char literal around. */
static const char YS_MIHOYO_PASSPORT_PUBKEY[] =
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDDvekdPMHN3AYhm/vktJT+YJr7cI5DcsNKqdsx5DZY"
    "0gDuWFuIjzdwButrIYPNmRJ1G8ybDIF7oDW2eEpm5sMbL9zs9ExXCdvqrn51qELbqj0XxtMTIpaCHFSI5"
    "0PfPpTFV9Xt/hmyVwokoOXFlAEgCn+QCgGs52bFoYMtyi+xEQIDAQAB";

char *ys_rsa_encrypt_b64(const char *pubkey_b64, const char *plain,
                         size_t plain_len, char **err) {
    if (err) { free(*err); *err = NULL; }
    /* NULL/"" selects the mihoyo passport key. The JS layer normally omits it
     * (there is only one key for this chain), so defaulting here keeps the
     * call site short and keeps the key in exactly one place. */
    if (!pubkey_b64 || !*pubkey_b64) pubkey_b64 = YS_MIHOYO_PASSPORT_PUBKEY;
    if (!plain) {
        set_err(err, "rsa: empty input%s%s", NULL, 0);
        return NULL;
    }

    /* The X.509 SPKI that mihoyo ships also parses as a PEM body; wrapping it
     * lets us reuse PEM_read_bio_PUBKEY instead of hand-rolling d2i_PUBKEY. */
    size_t b64len = strlen(pubkey_b64);
    size_t pemcap = b64len + 64;
    char *pem = (char *)malloc(pemcap);
    if (!pem) return NULL;
    snprintf(pem, pemcap, "-----BEGIN PUBLIC KEY-----\n%s\n-----END PUBLIC KEY-----\n",
             pubkey_b64);

    BIO *bio = BIO_new_mem_buf(pem, -1);
    if (!bio) {
        set_err(err, "rsa: BIO_new_mem_buf failed%s%s", NULL, ERR_get_error());
        free(pem);
        return NULL;
    }
    EVP_PKEY *pk = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    free(pem);
    if (!pk) {
        set_err(err, "rsa: PEM_read_bio_PUBKEY failed: %s%s", NULL, ERR_get_error());
        ERR_clear_error();
        return NULL;
    }
    if (EVP_PKEY_base_id(pk) != EVP_PKEY_RSA) {
        set_err(err, "rsa: key is not RSA (type %d)%s", NULL, 0);
        EVP_PKEY_free(pk);
        return NULL;
    }
    RSA *rsa = EVP_PKEY_get1_RSA(pk);
    EVP_PKEY_free(pk);
    if (!rsa) {
        set_err(err, "rsa: EVP_PKEY_get1_RSA failed%s%s", NULL, ERR_get_error());
        ERR_clear_error();
        return NULL;
    }

    /* 1024-bit key: 128-byte output, max plaintext 128-11 = 117 bytes. */
    unsigned char enc[512];
    int rc = RSA_public_encrypt((int)plain_len, (const unsigned char *)plain,
                                enc, rsa, RSA_PKCS1_PADDING);
    RSA_free(rsa);
    if (rc <= 0) {
        set_err(err, "rsa: RSA_public_encrypt failed (len=%s): %s%s",
                NULL, ERR_get_error());
        ERR_clear_error();
        return NULL;
    }
    char *b64 = ys_b64_encode(enc, (size_t)rc);
    if (!b64) set_err(err, "rsa: out of memory%s%s", NULL, 0);
    return b64;
}
