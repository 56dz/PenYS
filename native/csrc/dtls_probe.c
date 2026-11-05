// DTLS-SRTP feasibility probe (runs ON the dictionary pen).
// Server mode: DTLS listen + SRTP profile negotiation + export keying material.
// Client mode: DTLS connect + export. Prints hex of EXTRACTOR-dtls_srtp output.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ossl_min.h"
#include <errno.h>

static const char *SRTP_PROFILE = "SRTP_AES128_CM_SHA1_80";

static X509 *make_cert(EVP_PKEY **pk) {
    EVP_PKEY *pkey = EVP_PKEY_new();
    RSA *rsa = RSA_new();
    BIGNUM *e = BN_new(); BN_set_word(e, RSA_F4);
    if (RSA_generate_key_ex(rsa, 2048, e, NULL) != 1) { printf("RSA keygen FAIL"); exit(1); }
    if (EVP_PKEY_assign(pkey, 6 /*EVP_PKEY_RSA*/, rsa) != 1) { printf("assign FAIL"); exit(1); }
    X509 *x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME *nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char*)"pen", -1, -1, 0);
    X509_set_issuer_name(x, nm);
    X509_sign(x, pkey, EVP_sha256());
    *pk = pkey;
    return x;
}

static int setup_srtp(SSL *ssl, int is_server, unsigned char *km, unsigned int *kmlen) {
    static const char ext[] = "EXTRACTOR-dtls_srtp";
    if (!SSL_export_keying_material(ssl, km, 60, ext, sizeof(ext)-1, NULL, 0, 0)) return -2;
    SRTP_PROTECTION_PROFILE *p = SSL_get_selected_srtp_profile(ssl);
    printf("  selected_srtp_profile = %s\n", p ? p->name : "(null)");
    *kmlen = 60;
    return p ? 0 : -3;
}

static void hexdump(const char *tag, const unsigned char *b, int n) {
    printf("  %s = ", tag);
    for (int i=0;i<n;i++) printf("%02x", b[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: dtls_probe server|client [port]\n"); return 2; }
    int is_server = !strcmp(argv[1], "server");
    int port = argc > 2 ? atoi(argv[2]) : 18443;
    OPENSSL_init_ssl(0, NULL);

    SSL_CTX *ctx = SSL_CTX_new(is_server ? DTLS_server_method() : DTLS_client_method());
    if (!ctx) { printf("SSL_CTX_new FAIL\n"); return 1; }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    if (is_server) {
        EVP_PKEY *pk=NULL; X509 *cert = make_cert(&pk);
        if (SSL_CTX_use_certificate(ctx, cert) != 1) { printf("use_certificate FAIL\n"); return 1; }
        if (SSL_CTX_use_PrivateKey(ctx, pk) != 1) { printf("use_privkey FAIL\n"); return 1; }
    }

    /* Offer the SRTP profile up front so it travels in the ClientHello/ServerHello
     * use_srtp extension; setting it after the handshake selects nothing. */
    if (SSL_CTX_set_tlsext_use_srtp(ctx, SRTP_PROFILE) != 0) {
        printf("[%s] set_tlsext_use_srtp FAIL\n", is_server?"server":"client");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa; memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons(port);
    sa.sin_addr.s_addr = is_server ? htonl(INADDR_ANY) : inet_addr("127.0.0.1");
    if (is_server) {
        if (bind(fd,(struct sockaddr*)&sa,sizeof(sa))<0){perror("bind");return 1;}
        /* DTLS server over a plain UDP socket: learn the peer first, then connect()
         * so the socket carries a 5-tuple (OpenSSL dgram BIO without pktinfo needs it). */
        char peek[2048];
        struct sockaddr_in peer; socklen_t plen = sizeof(peer);
        printf("[server] waiting for first datagram...\n");
        int n = recvfrom(fd, peek, sizeof(peek), MSG_PEEK, (struct sockaddr*)&peer, &plen);
        if (n <= 0) { perror("recvfrom"); return 1; }
        char ipb[32]; inet_ntop(AF_INET, &peer.sin_addr, ipb, sizeof(ipb));
        printf("[server] peer = %s:%d (%d bytes)\n", ipb, ntohs(peer.sin_port), n);
        if (connect(fd,(struct sockaddr*)&peer,sizeof(peer))<0){perror("connect(peer)");return 1;}
    } else {
        if (connect(fd,(struct sockaddr*)&sa,sizeof(sa))<0){perror("connect");return 1;}
    }
    printf("[%s] udp ready port=%d\n", is_server?"server":"client", port);

    BIO *bio = BIO_new_dgram(fd, BIO_NOCLOSE);
    /* Tell the dgram BIO the peer; without this OpenSSL sendto()s an AF_UNSPEC
     * sockaddr and the kernel answers EINVAL. */
    {
        struct sockaddr_in peer;
        socklen_t pl;

        if (is_server) {
            pl = sizeof(peer);
            if (getsockname(fd, (struct sockaddr *)&peer, &pl) < 0) {
                printf("[%s] getsockname FAIL\n", is_server ? "server" : "client");
                return 1;
            }
            pl = sizeof(peer);
            if (getpeername(fd, (struct sockaddr *)&peer, &pl) == 0) {
                BIO_ctrl(bio, 44 /*BIO_CTRL_DGRAM_SET_PEER*/, 0, &peer);
                BIO_ctrl(bio, 32 /*BIO_CTRL_DGRAM_SET_CONNECTED*/, 0, &peer);
            }
        } else {
            pl = sizeof(peer);
            if (getpeername(fd, (struct sockaddr *)&peer, &pl) == 0) {
                BIO_ctrl(bio, 44 /*BIO_CTRL_DGRAM_SET_PEER*/, 0, &peer);
                BIO_ctrl(bio, 32 /*BIO_CTRL_DGRAM_SET_CONNECTED*/, 0, &peer);
            }
        }
    }

    SSL *ssl = SSL_new(ctx);
    SSL_set_bio(ssl, bio, bio);
    if (!is_server) SSL_set_connect_state(ssl); else SSL_set_accept_state(ssl);

    int r = is_server ? SSL_accept(ssl) : SSL_connect(ssl);
    if (r != 1) {
        int se = SSL_get_error(ssl, r);
        printf("[%s] HANDSHAKE FAIL r=%d ssl_err=%d errno=%d (%s)\n",
               is_server?"server":"client", r, se, errno, strerror(errno));
        unsigned long e;
        char buf[256];
        while ((e = ERR_get_error()) != 0) {
            ERR_error_string_n(e, buf, sizeof(buf));
            printf("  ossl: %s\n", buf);
        }
        return 1;
    }
    printf("[%s] HANDSHAKE OK, version=%s\n", is_server?"server":"client",
           SSL_get_version(ssl));

    unsigned char km[64]; unsigned int kmlen=0;
    int sr = setup_srtp(ssl, is_server, km, &kmlen);
    printf("[%s] setup_srtp rc=%d\n", is_server?"server":"client", sr);
    if (sr==0) { hexdump("keying_material(60)", km, 60);
        // Split per RFC 5764: client_key(16) server_key(16) client_salt(14) server_salt(14)
        hexdump("client_write_key", km+0, 16);
        hexdump("server_write_key", km+16, 16);
        hexdump("client_write_salt", km+32, 14);
        hexdump("server_write_salt", km+46, 14);
    }
    printf("[%s] DONE\n", is_server?"server":"client");
    return sr==0?0:1;
}
