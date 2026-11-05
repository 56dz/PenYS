/* ys_http.c -- libcurl HTTP client with readable response headers and a
 * per-key cookie jar. See ys_net.h for why this module exists at all.
 *
 * COOKIE JAR DESIGN
 * libcurl's cookie engine is per-easy-handle, and handles are not thread safe,
 * so a handle cannot be parked in a shared jar. Instead each request creates a
 * fresh handle, enables the engine with CURLOPT_COOKIEFILE "", imports the
 * bucket's cookies with CURLOPT_COOKIELIST, performs, then exports the engine's
 * cookies back with CURLINFO_COOKIELIST and merges them into the bucket.
 *
 * That makes the bucket the single source of truth, keeps handles short lived,
 * and lets several threads use one bucket (they only serialize while merging).
 * The Netscape line format libcurl exports is also exactly what
 * CURLOPT_COOKIELIST accepts, which makes save/load a plain text file.
 *
 * HEADER CAPTURE
 * CURLOPT_HEADERFUNCTION appends to a buffer that is truncated whenever a new
 * "HTTP/" status line arrives, so after a redirect chain the buffer holds the
 * *final* response's headers only. That is what callers need: Set-Cookie on a
 * 302 is still merged into the jar by libcurl's engine, but the status/code
 * they see belongs to the last hop.
 */
#include "ys_net.h"
#include "ys_curl.h"

#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CAINFO_PATH "/etc/ssl/certs/ca-certificates.crt"
#define MAX_JAR_LINE 1400

/* ------------------------------------------------------------------ buffer */

void ys_buf_init(ys_buf *b) {
    if (!b) return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void ys_buf_free(ys_buf *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int ys_buf_reserve(ys_buf *b, size_t need) {
    if (!b) return -1;
    if (b->len + need + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) {
        if (cap > (size_t)1 << 24) { /* 16 MiB ceiling: refuse runaway bodies */
            return -1;
        }
        cap *= 2;
    }
    char *nb = (char *)realloc(b->data, cap);
    if (!nb) return -1;
    b->data = nb;
    b->cap = cap;
    return 0;
}

int ys_buf_append(ys_buf *b, const void *p, size_t n) {
    if (!b || (!p && n)) return -1;
    if (ys_buf_reserve(b, n) != 0) return -1;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

int ys_buf_puts(ys_buf *b, const char *s) {
    if (!s) return 0;
    return ys_buf_append(b, s, strlen(s));
}

const char *ys_buf_cstr(ys_buf *b) {
    if (!b) return "";
    if (!b->data) {
        b->data = (char *)malloc(1);
        if (!b->data) return "";
        b->data[0] = 0;
        b->cap = 1;
    }
    b->data[b->len] = 0;
    return b->data;
}

/* ------------------------------------------------------------- small utils */

long long ys_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static char *ys_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static void ys_resp_reset(ys_resp *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

void ys_resp_free(ys_resp *r) {
    if (!r) return;
    free(r->headers);
    free(r->body);
    free(r->error);
    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------- curl global */

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static int g_curl_ready = 0;

static void ys_curl_global_init_once(void) {
    g_curl_ready = (curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
}

/* -------------------------------------------------------------- cookie jar */

typedef struct ys_jar {
    char *key;
    char **lines;
    int n;
    int cap;
    struct ys_jar *next;
} ys_jar;

static ys_jar *g_jars = NULL;
static pthread_mutex_t g_jar_mu = PTHREAD_MUTEX_INITIALIZER;

static ys_jar *jar_find_locked(const char *key) {
    for (ys_jar *j = g_jars; j; j = j->next) {
        if (strcmp(j->key, key) == 0) return j;
    }
    return NULL;
}

static ys_jar *jar_get_locked(const char *key, int create) {
    if (!key || !*key) key = "__anon__";
    ys_jar *j = jar_find_locked(key);
    if (j || !create) return j;
    j = (ys_jar *)calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->key = ys_strdup(key);
    if (!j->key) { free(j); return NULL; }
    j->next = g_jars;
    g_jars = j;
    return j;
}

/* Identity of a Netscape cookie line: domain + path + name. libcurl marks
 * HttpOnly cookies by prefixing the domain with "#HttpOnly_". */
static void jar_line_ident(const char *line, char *out, size_t outcap) {
    out[0] = 0;
    const char *p = line;
    if (strncmp(p, "#HttpOnly_", 11) == 0) p += 11;
    const char *f[7];
    int nf = 0;
    const char *cur = p;
    while (nf < 7) {
        const char *tab = strchr(cur, '\t');
        f[nf++] = cur;
        if (!tab) break;
        cur = tab + 1;
    }
    if (nf >= 6) {
        /* f[0]=domain f[2]=path f[5]=name -- recompute lengths from the line */
        const char *dom = f[0];
        const char *dom_end = strchr(dom, '\t');
        const char *path = f[2];
        const char *path_end = strchr(path, '\t');
        const char *name = f[5];
        const char *name_end = strchr(name, '\t');
        if (dom_end && path_end && name_end) {
            snprintf(out, outcap, "%.*s|%.*s|%.*s",
                     (int)(dom_end - dom), dom,
                     (int)(path_end - path), path,
                     (int)(name_end - name), name);
            return;
        }
    }
    snprintf(out, outcap, "%s", line);
}

static void jar_add_locked(ys_jar *j, const char *line) {
    if (!j || !line || !*line) return;
    char ident[MAX_JAR_LINE];
    jar_line_ident(line, ident, sizeof(ident));
    for (int i = 0; i < j->n; i++) {
        char have[MAX_JAR_LINE];
        jar_line_ident(j->lines[i], have, sizeof(have));
        if (strcmp(have, ident) == 0) {
            char *nl = ys_strdup(line);
            if (!nl) return;
            free(j->lines[i]);
            j->lines[i] = nl;
            return;
        }
    }
    if (j->n == j->cap) {
        int cap = j->cap ? j->cap * 2 : 8;
        char **nl = (char **)realloc(j->lines, (size_t)cap * sizeof(char *));
        if (!nl) return;
        j->lines = nl;
        j->cap = cap;
    }
    char *copy = ys_strdup(line);
    if (!copy) return;
    j->lines[j->n++] = copy;
}

/* Import the bucket into a live handle. Must be called with the jar locked. */
static void jar_import_to_handle_locked(ys_jar *j, CURL *h, ys_buf *errs) {
    if (!j) return;
    for (int i = 0; i < j->n; i++) {
        int rc = curl_easy_setopt(h, CURLOPT_COOKIELIST, j->lines[i]);
        if (rc != CURLE_OK && errs) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "cookie import rc=%d for %s\n", rc, j->lines[i]);
            ys_buf_puts(errs, tmp);
        }
    }
}

/* Pull the handle's cookies back into the bucket. */
static int jar_export_from_handle_locked(ys_jar *j, CURL *h) {
    if (!j) return 0;
    struct curl_slist *list = NULL;
    if (curl_easy_getinfo(h, CURLINFO_COOKIELIST, &list) != CURLE_OK) return -1;
    int added = 0;
    for (struct curl_slist *it = list; it; it = it->next) {
        if (it->data && it->data[0]) { jar_add_locked(j, it->data); added++; }
    }
    if (list) curl_slist_free_all(list);
    return added;
}

char *ys_jar_dump(const char *jar_key) {
    pthread_mutex_lock(&g_jar_mu);
    ys_jar *j = jar_get_locked(jar_key, 0);
    ys_buf b;
    ys_buf_init(&b);
    if (j && j->n) {
        for (int i = 0; i < j->n; i++) {
            ys_buf_puts(&b, j->lines[i]);
            ys_buf_puts(&b, "\n");
        }
    }
    pthread_mutex_unlock(&g_jar_mu);
    if (b.len == 0) { ys_buf_free(&b); return NULL; }
    return b.data;
}

int ys_jar_import(const char *jar_key, const char *text) {
    if (!text) return 0;
    pthread_mutex_lock(&g_jar_mu);
    ys_jar *j = jar_get_locked(jar_key, 1);
    int n = 0;
    if (j) {
        const char *p = text;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len && p[len - 1] == '\r') len--;
            if (len > 0 && p[0] != '#') {
                char line[MAX_JAR_LINE];
                if (len >= sizeof(line)) len = sizeof(line) - 1;
                memcpy(line, p, len);
                line[len] = 0;
                jar_add_locked(j, line);
                n++;
            } else if (len > 11 && strncmp(p, "#HttpOnly_", 11) == 0) {
                char line[MAX_JAR_LINE];
                if (len >= sizeof(line)) len = sizeof(line) - 1;
                memcpy(line, p, len);
                line[len] = 0;
                jar_add_locked(j, line);
                n++;
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
    pthread_mutex_unlock(&g_jar_mu);
    return n;
}

int ys_jar_count(const char *jar_key) {
    pthread_mutex_lock(&g_jar_mu);
    ys_jar *j = jar_get_locked(jar_key, 0);
    int n = j ? j->n : 0;
    pthread_mutex_unlock(&g_jar_mu);
    return n;
}

int ys_jar_clear(const char *jar_key) {
    if (!jar_key || !*jar_key) jar_key = "__anon__";
    pthread_mutex_lock(&g_jar_mu);
    ys_jar *j = jar_get_locked(jar_key, 0);
    if (j) {
        for (int i = 0; i < j->n; i++) free(j->lines[i]);
        j->n = 0;
    }
    pthread_mutex_unlock(&g_jar_mu);
    return 0;
}

/* mkdir -p 的极简版：只处理绝对路径，逐级建目录。
 * 为什么需要：cookie jar 的落盘路径 /userdata/penys/ys_cookies.txt 里，
 * penys 这一级在干净设备上并不存在，fopen("w") 会直接 ENOENT。
 * 静默忽略已存在（EEXIST）与权限不足（后者由随后的 fopen 报错体现）。 */
static void mkdir_parents(const char *path) {
    char tmp[512];
    size_t n;
    if (!path || !*path) return;
    n = strlen(path);
    if (n >= sizeof(tmp)) return;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        mkdir(tmp, 0755);
        tmp[i] = '/';
    }
}

int ys_jar_save(const char *jar_key, const char *path) {
    char *text = ys_jar_dump(jar_key);
    if (!path || !*path) { free(text); return -1; }
    mkdir_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) { free(text); return -1; }
    if (text) fputs(text, f);
    fclose(f);
    free(text);
    return 0;
}

int ys_jar_load(const char *jar_key, const char *path) {
    if (!path || !*path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    ys_buf b;
    ys_buf_init(&b);
    char tmp[1024];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (ys_buf_append(&b, tmp, n) != 0) break;
    }
    fclose(f);
    int got = ys_jar_import(jar_key, ys_buf_cstr(&b));
    ys_buf_free(&b);
    return got;
}

/* -------------------------------------------------------------- transfer */

struct ys_collect {
    ys_buf hdr;
    ys_buf body;
};

static size_t ys_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct ys_collect *c = (struct ys_collect *)ud;
    size_t len = size * nmemb;
    if (ys_buf_append(&c->body, ptr, len) != 0) return 0; /* aborts transfer */
    return len;
}

static size_t ys_header_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct ys_collect *c = (struct ys_collect *)ud;
    size_t len = size * nmemb;
    /* A new status line starts a new response block (redirect hops,
     * 100-continue, proxy CONNECT). Keep only the last one. */
    if (len >= 5 && memcmp(ptr, "HTTP/", 5) == 0) c->hdr.len = 0;
    if (ys_buf_append(&c->hdr, ptr, len) != 0) return 0;
    return len;
}

static long parse_status(const char *hdr) {
    /* "HTTP/1.1 200 OK" */
    if (!hdr) return 0;
    const char *sp = strchr(hdr, ' ');
    if (!sp) return 0;
    long code = 0;
    int digits = 0;
    for (const char *p = sp + 1; *p && digits < 3; p++, digits++) {
        if (*p < '0' || *p > '9') break;
        code = code * 10 + (*p - '0');
    }
    return digits == 3 ? code : 0;
}

static int is_cert_error(int code) {
    switch (code) {
        case 35: /* SSL connect error */
        case 51: /* peer cert / CN mismatch (old) */
        case 53: /* engine not found */
        case 58: /* local cert problem */
        case 59: /* cipher unavailable */
        case 60: /* peer cert cannot be authenticated */
        case 64: /* use of closed connection (unrelated but harmless) */
        case 66: /* SSL engine init failed */
        case 77: /* CA file problem */
        case 83: /* issuer check failed */
        case 90: /* pinned pubkey mismatch */
        case 91: /* invalid cert status */
            return 1;
        default:
            return 0;
    }
}

static int build_slist(const char *header_lines, struct curl_slist **out) {
    *out = NULL;
    if (!header_lines || !*header_lines) return 0;
    char *copy = ys_strdup(header_lines);
    if (!copy) return -1;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        /* trim trailing CR / spaces */
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (n == 0) continue;
        struct curl_slist *nl = curl_slist_append(*out, line);
        if (!nl) { curl_slist_free_all(*out); *out = NULL; free(copy); return -1; }
        *out = nl;
    }
    free(copy);
    return 0;
}

static int ys_http_once(const char *method, const char *url, const char *header_lines,
                        const char *body, size_t body_len, long timeout_ms,
                        int follow, const char *jar_key, int insecure,
                        ys_resp *out, struct ys_collect *col, ys_buf *jerr,
                        char *dbg_out, size_t dbg_cap) {
    CURL *h = curl_easy_init();
    if (!h) {
        out->error = ys_strdup("curl_easy_init failed");
        out->curl_code = CURLE_FAILED_INIT;
        return -1;
    }

    struct curl_slist *headers = NULL;
    if (build_slist(header_lines, &headers) != 0) {
        out->error = ys_strdup("out of memory building headers");
        curl_easy_cleanup(h);
        return -1;
    }

    int m = (method && *method) ? method[0] : 0;
    int is_get = (m == 0 || m == 'G' || m == 'g');
    int is_head = (m == 'H' || m == 'h');

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, ys_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, col);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, ys_header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, col);

    long total = timeout_ms > 0 ? timeout_ms : 20000L;
    long conn = total < 10000L ? total : 10000L;
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, total);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, conn);

    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
    if (follow) curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);

    /* Accept-Encoding "" lets libcurl negotiate AND decode gzip/deflate, which
     * also keeps the jsapi from ever seeing a compressed body. */
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");

    if (access(CAINFO_PATH, R_OK) == 0) {
        curl_easy_setopt(h, CURLOPT_CAINFO, CAINFO_PATH);
    }
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, insecure ? 0L : 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, insecure ? 0L : 2L);

    if (is_head) {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "HEAD");
    } else if (is_get && !body) {
        curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    } else {
        const char *verb = (method && *method) ? method : "POST";
        if (!(verb[0] == 'P' && verb[1] == 'O')) {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, verb);
        } else {
            curl_easy_setopt(h, CURLOPT_POST, 1L);
        }
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)(body ? body_len : 0));
    }

    if (headers) curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

    /* Cookie engine: "" means "enable but do not read a file". */
    curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");

    pthread_mutex_lock(&g_jar_mu);
    ys_jar *jar = jar_get_locked(jar_key, 1);
    jar_import_to_handle_locked(jar, h, jerr);
    pthread_mutex_unlock(&g_jar_mu);

    /* CURLINFO_HEADER_OUT needs this on, but it is cheap (one line per
     * request) and it is the only way to see the request headers libcurl
     * actually emitted -- which is how cookie replay is proven. */
    curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);

    int rc = curl_easy_perform(h);

    if (dbg_out && dbg_cap) {
        char *hout = NULL;
        if (curl_easy_getinfo(h, CURLINFO_HEADER_OUT, &hout) == CURLE_OK && hout) {
            size_t n = strlen(hout);
            if (n >= dbg_cap) n = dbg_cap - 1;
            memcpy(dbg_out, hout, n);
            dbg_out[n] = 0;
        } else {
            dbg_out[0] = 0;
        }
    }

    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    }

    pthread_mutex_lock(&g_jar_mu);
    jar_export_from_handle_locked(jar, h);
    pthread_mutex_unlock(&g_jar_mu);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(h);

    out->curl_code = rc;
    if (rc != CURLE_OK) {
        const char *es = curl_easy_strerror(rc);
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "curl error %d: %s", rc, es ? es : "?");
        free(out->error);
        out->error = ys_strdup(tmp);
        return -1;
    }
    if (status <= 0) status = parse_status(col->hdr.data);
    out->status = status;
    out->ok = status > 0 ? 1 : 0;
    return 0;
}

int ys_http_do(const char *method, const char *url, const char *header_lines,
               const char *body, size_t body_len, long timeout_ms,
               int follow, const char *jar_key, ys_resp *out) {
    if (!out) return -1;
    ys_resp_reset(out);
    if (!url || !*url) {
        out->error = ys_strdup("empty url");
        return -1;
    }
    pthread_once(&g_curl_once, ys_curl_global_init_once);
    if (!g_curl_ready) {
        out->error = ys_strdup("curl_global_init failed");
        return -1;
    }

    struct ys_collect col;
    ys_buf_init(&col.hdr);
    ys_buf_init(&col.body);
    ys_buf jerr;
    ys_buf_init(&jerr);

    int rc = ys_http_once(method, url, header_lines, body, body_len, timeout_ms,
                          follow, jar_key, 0, out, &col, &jerr, NULL, 0);

    /* One retry with verification off, only for TLS failures. This is the
     * documented degradation path: a device with a broken cert store must
     * still be able to reach the login server, and the caller is told the
     * connection was unverified via ys_resp.insecure. */
    if (rc != 0 && is_cert_error(out->curl_code)) {
        col.hdr.len = 0;
        col.body.len = 0;
        free(out->error);
        out->error = NULL;
        if (ys_http_once(method, url, header_lines, body, body_len, timeout_ms,
                         follow, jar_key, 1, out, &col, &jerr, NULL, 0) == 0 &&
            out->status > 0) {
            out->insecure = 1;
            rc = 0;
        }
    }

    out->headers = col.hdr.data ? col.hdr.data : ys_strdup("");
    out->body = col.body.data ? col.body.data : ys_strdup("");
    out->body_len = col.body.len;
    if (!col.hdr.data) col.hdr.cap = 0;
    if (!col.body.data) col.body.cap = 0;

    if (jerr.len) {
        /* cookie problems never fail the request; surface them on stderr so a
         * field log shows them without changing the JS contract. */
        fprintf(stderr, "[ys_http] jar warnings:\n%s", ys_buf_cstr(&jerr));
    }
    ys_buf_free(&jerr);
    return rc;
}

/* Debug variant: identical to ys_http_do but also copies the exact request
 * header block libcurl emitted into dbg_out. Verification uses this to prove
 * that the cookie jar actually replayed "Cookie:" on the second request --
 * a 200 alone would not distinguish "cookie sent" from "cookie ignored",
 * because passport-api answers 200 with retcode -100 in both cases. */
int ys_http_do_debug(const char *method, const char *url, const char *header_lines,
                     const char *body, size_t body_len, long timeout_ms, int follow,
                     const char *jar_key, ys_resp *out, char *dbg_out, size_t dbg_cap) {
    if (dbg_out && dbg_cap) dbg_out[0] = 0;
    if (!out) return -1;
    ys_resp_reset(out);
    if (!url || !*url) {
        out->error = ys_strdup("empty url");
        return -1;
    }
    pthread_once(&g_curl_once, ys_curl_global_init_once);
    if (!g_curl_ready) {
        out->error = ys_strdup("curl_global_init failed");
        return -1;
    }

    struct ys_collect col;
    ys_buf_init(&col.hdr);
    ys_buf_init(&col.body);
    ys_buf jerr;
    ys_buf_init(&jerr);

    int rc = ys_http_once(method, url, header_lines, body, body_len, timeout_ms,
                          follow, jar_key, 0, out, &col, &jerr, dbg_out, dbg_cap);

    out->headers = col.hdr.data ? col.hdr.data : ys_strdup("");
    out->body = col.body.data ? col.body.data : ys_strdup("");
    out->body_len = col.body.len;
    if (!col.hdr.data) col.hdr.cap = 0;
    if (!col.body.data) col.body.cap = 0;
    ys_buf_free(&jerr);
    return rc;
}
