/* ys_curl.h -- libcurl ABI declarations for the device's libcurl 7.79.1.
 *
 * The device ships libcurl.so.4 but no curl headers at all, exactly like PenBili's
 * httpjson.c found. Rather than trust a copied constant table, every numeric
 * option below was verified on the device with .tmp/feas/net/optprobe.c, which
 * asks libcurl for the id of each option *by name*
 * (curl_easy_option_by_name) and compares. Result: 30/31 match; the only
 * correction vs. the classic header set is
 *
 *     CURLOPT_COOKIELIST is 10135, not 10091.
 *
 * (10091 is CURLOPT_COOKIESESSION in some old trees; libcurl never had
 * COOKIELIST there. PenBili's table was copied from 10091 and is wrong, but it
 * never called it, so the bug was invisible.)
 *
 * CURLINFO_* values are not in libcurl's option table at all; they were proven
 * by running a real HTTPS request and checking curl_easy_getinfo() returned
 * correct data through these exact ids (optprobe part 2).
 */
#ifndef YS_CURL_H
#define YS_CURL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void CURL;
typedef void CURLSH;
struct curl_slist { char *data; struct curl_slist *next; };
struct curl_easyoption { const char *name; int id; int type; unsigned int flags; };

/* ---- CURLcode ---- */
#define CURLE_OK 0
#define CURLE_UNSUPPORTED_PROTOCOL 1
#define CURLE_FAILED_INIT 2
#define CURLE_COULDNT_RESOLVE_HOST 6
#define CURLE_COULDNT_CONNECT 7
#define CURLE_PARTIAL_FILE 18
#define CURLE_OPERATION_TIMEDOUT 28
#define CURLE_SSL_CONNECT_ERROR 35
#define CURLE_SSL_CACERT 60       /* same value as CURLE_PEER_FAILED_VERIFICATION */
#define CURLE_SSL_ENGINE_NOTFOUND 53
#define CURLE_SEND_ERROR 55
#define CURLE_RECV_ERROR 56

/* ---- CURLOPT: verified on device ---- */
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_HEADERDATA 10029
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_COOKIEFILE 10031
#define CURLOPT_CUSTOMREQUEST 10036
#define CURLOPT_USERAGENT 10018
#define CURLOPT_CAINFO 10065
#define CURLOPT_COOKIEJAR 10082
#define CURLOPT_COOKIELIST 10135   /* <-- corrected, see header comment */
#define CURLOPT_ACCEPT_ENCODING 10102
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_HEADERFUNCTION 20079
#define CURLOPT_TIMEOUT 13
#define CURLOPT_TIMEOUT_MS 155
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_CONNECTTIMEOUT_MS 156
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_MAXREDIRS 68
#define CURLOPT_SSL_VERIFYPEER 64
#define CURLOPT_SSL_VERIFYHOST 81
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_POST 47
#define CURLOPT_POSTFIELDSIZE 60
#define CURLOPT_HTTPGET 80
#define CURLOPT_VERBOSE 41
#define CURLOPT_DEBUGFUNCTION 20094
#define CURLOPT_DEBUGDATA 10095
#define CURLINFO_HEADER_OUT 0x100002
#define CURLOT_HEADER_OUT 4
#define CURLOPT_FRESH_CONNECT 74
#define CURLOPT_FORBID_REUSE 75
#define CURLOPT_NOPROGRESS 43
#define CURLOPT_IPRESOLVE 113

/* CURL_IPRESOLVE_V4 */
#define CURL_IPRESOLVE_WHATEVER 0
#define CURL_IPRESOLVE_V4 1

/* ---- CURLINFO: verified by live request ---- */
#define CURLINFO_RESPONSE_CODE 0x200002
#define CURLINFO_CONTENT_TYPE 0x100012
#define CURLINFO_COOKIELIST 0x40001C
#define CURLINFO_TOTAL_TIME 0x300003
#define CURLINFO_CONNECT_TIME 0x300005

/* ---- CURL_GLOBAL ---- */
#define CURL_GLOBAL_ALL 3

extern int curl_global_init(long flags);
extern CURL *curl_easy_init(void);
extern void curl_easy_cleanup(CURL *h);
extern void curl_easy_reset(CURL *h);
extern int curl_easy_setopt(CURL *h, int opt, ...);
extern int curl_easy_getinfo(CURL *h, int info, ...);
extern int curl_easy_perform(CURL *h);
extern const char *curl_easy_strerror(int code);
extern char *curl_version(void);
extern struct curl_slist *curl_slist_append(struct curl_slist *l, const char *s);
extern void curl_slist_free_all(struct curl_slist *l);
extern void curl_free(void *p);
extern const struct curl_easyoption *curl_easy_option_by_name(const char *name);

#ifdef __cplusplus
}
#endif
#endif /* YS_CURL_H */
