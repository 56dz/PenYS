/* ys_jsapi.c -- QuickJS glue for the ys_net cores.
 *
 * Exposes two C modules to the Falcon mini-app:
 *
 *   import { start, take, cancelAll, hmacSha256, ... } from 'yshttp'
 *   import { wsRttStart, wsRttTake, nowMs } from 'ysws'
 *
 * THREADING CONTRACT (the reason this file is written the way it is)
 * The Falcon JS thread must never block: a libcurl transfer can take 20 s and
 * a WSS probe 3 s, and the runtime renders on that same thread. So every slow
 * call is split into "start -> id" (returns immediately, work happens on a
 * detached pthread) and "take(id) -> JSON string | null" (non-blocking poll).
 * This mirrors PenBili's proven startPost/takeResult pattern.
 *
 * Only genuinely fast calls are synchronous: HMAC/SHA/RSA/base64 (microseconds)
 * and the cookie-jar text accessors. \n
 * Nothing here touches JSValue from a worker thread -- workers only ever fill
 * a malloc'd C string, which the JS thread later converts.
 */
#include <jquick_config.h>
#include <jsmodules/JSCModuleExtension.h>
#include <quickjs/quickjs.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ys_net.h"

/* ---------------- JSON helpers ---------------- */

static void jbuf_put(ys_buf *b, const char *s) {
    if (s) ys_buf_puts(b, s);
}

/* Escape `s` into a JSON string literal, appending the surrounding quotes.
 * Body text is arbitrary UTF-8 and may contain quotes/newlines/control bytes,
 * and the JS side does JSON.parse on the result, so this must be exact. */
static void jbuf_put_json_string(ys_buf *b, const char *s) {
    ys_buf_puts(b, "\"");
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            unsigned char c = *p;
            switch (c) {
            case '"':  ys_buf_puts(b, "\\\""); break;
            case '\\': ys_buf_puts(b, "\\\\"); break;
            case '\n': ys_buf_puts(b, "\\n"); break;
            case '\r': ys_buf_puts(b, "\\r"); break;
            case '\t': ys_buf_puts(b, "\\t"); break;
            case '\b': ys_buf_puts(b, "\\b"); break;
            case '\f': ys_buf_puts(b, "\\f"); break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    ys_buf_puts(b, tmp);
                } else {
                    ys_buf_append(b, &c, 1);
                }
            }
        }
    }
    ys_buf_puts(b, "\"");
}

static void jbuf_put_long(ys_buf *b, long v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", v);
    ys_buf_puts(b, tmp);
}

/* ---------------- job queue ---------------- */

enum { JOB_HTTP = 0, JOB_WSRTT = 1 };

struct job {
    int id;
    int kind;
    int state;      /* 0 = running, 1 = done */
    int cancelled;
    char *result;   /* JSON, set by the worker */

    /* http inputs (owned copies; the JS thread frees its strings immediately) */
    char *method;
    char *url;
    char *headers;
    char *body;
    size_t body_len;
    long timeout_ms;
    int follow;
    char *jar_key;

    /* wsrtt inputs */
    int count;

    struct job *next;
};

static struct job *g_jobs;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 1;

static void job_free(struct job *j) {
    if (!j) return;
    free(j->method);
    free(j->url);
    free(j->headers);
    free(j->body);
    free(j->jar_key);
    free(j->result);
    free(j);
}

/* Runs on a detached worker thread. Fills j->result then flips j->state.
 * Touches no JSValue. */
static void *job_thread(void *arg) {
    struct job *j = (struct job *)arg;
    ys_buf out;
    ys_buf_init(&out);

    if (j->kind == JOB_HTTP) {
        ys_resp r;
        int rc = ys_http_do(j->method, j->url, j->headers, j->body, j->body_len,
                            j->timeout_ms, j->follow, j->jar_key, &r);
        jbuf_put(&out, "{\"kind\":\"http\",\"rc\":");
        jbuf_put_long(&out, rc);
        jbuf_put(&out, ",\"ok\":");
        jbuf_put_long(&out, r.ok ? 1 : 0);
        jbuf_put(&out, ",\"status\":");
        jbuf_put_long(&out, r.status);
        jbuf_put(&out, ",\"insecure\":");
        jbuf_put_long(&out, r.insecure ? 1 : 0);
        jbuf_put(&out, ",\"headers\":");
        jbuf_put_json_string(&out, r.headers);
        jbuf_put(&out, ",\"body\":");
        if (r.body && r.body_len) {
            /* body may contain NUL-free bytes only after UTF-8 decode; still
             * cap at body_len so a truncated tail never leaks into the JSON. */
            char *tmp = (char *)malloc(r.body_len + 1);
            if (tmp) {
                memcpy(tmp, r.body, r.body_len);
                tmp[r.body_len] = 0;
                jbuf_put_json_string(&out, tmp);
                free(tmp);
            } else {
                jbuf_put_json_string(&out, "");
            }
        } else {
            jbuf_put_json_string(&out, "");
        }
        jbuf_put(&out, ",\"error\":");
        jbuf_put_json_string(&out, r.error);
        jbuf_put(&out, "}");
        ys_resp_free(&r);
    } else {
        ys_rtt_result res;
        memset(&res, 0, sizeof(res));
        int rc = ys_ws_rtt_test(j->url, j->count, (int)j->timeout_ms, &res);
        jbuf_put(&out, "{\"kind\":\"wsrtt\",\"rc\":");
        jbuf_put_long(&out, rc);
        jbuf_put(&out, ",\"ok\":");
        jbuf_put_long(&out, res.ok ? 1 : 0);
        jbuf_put(&out, ",\"sent\":");
        jbuf_put_long(&out, res.sent);
        jbuf_put(&out, ",\"recvd\":");
        jbuf_put_long(&out, res.recvd);
        jbuf_put(&out, ",\"avgRtt\":");
        jbuf_put_long(&out, res.avg_rtt);
        jbuf_put(&out, ",\"minRtt\":");
        jbuf_put_long(&out, res.min_rtt);
        jbuf_put(&out, ",\"maxRtt\":");
        jbuf_put_long(&out, res.max_rtt);
        jbuf_put(&out, ",\"connectMs\":");
        jbuf_put_long(&out, res.connect_ms);
        jbuf_put(&out, ",\"tlsMs\":");
        jbuf_put_long(&out, res.tls_ms);
        jbuf_put(&out, ",\"error\":");
        jbuf_put_json_string(&out, res.error);
        jbuf_put(&out, "}");
    }

    pthread_mutex_lock(&g_mu);
    if (j->cancelled) {
        pthread_mutex_unlock(&g_mu);
        ys_buf_free(&out);
        job_free(j);
        return NULL;
    }
    j->result = out.data ? out.data : strdup("{}");
    j->state = 1;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* Detach a job into the table and start it. Returns the id. The inputs are
 * copied here so the caller can free its C strings right away. */
static int job_start(struct job *j) {
    pthread_t th;
    pthread_mutex_lock(&g_mu);
    j->id = g_next_id++;
    j->next = g_jobs;
    g_jobs = j;
    pthread_mutex_unlock(&g_mu);

    if (pthread_create(&th, NULL, job_thread, j) != 0) {
        struct job **pp;
        pthread_mutex_lock(&g_mu);
        pp = &g_jobs;
        while (*pp && *pp != j) pp = &(*pp)->next;
        if (*pp == j) *pp = j->next;
        pthread_mutex_unlock(&g_mu);
        job_free(j);
        return 0;
    }
    pthread_detach(th);
    return j->id;
}

static char *dup_js_string(JSContext *ctx, JSValueConst v) {
    const char *s = JS_ToCString(ctx, v);
    if (!s) return NULL;
    char *d = strdup(s);
    JS_FreeCString(ctx, s);
    return d;
}

/* Read dst from obj.key if it is a string; leaves dst NULL otherwise. */
static void read_string_prop(JSContext *ctx, JSValueConst obj, const char *key, char **dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) *dst = dup_js_string(ctx, v);
    JS_FreeValue(ctx, v);
}

/* ------------------------------------------------------------------ http */

/* start(opts) -> id
 *   opts = { url, method?, headers?, body?, timeoutMs?, followRedirects?,
 *            cookieJarKey? }
 *
 * headers may be either a "K: V\r\nK2: V2" string or a plain object (its
 * enumerable own props are serialised in insertion order, which is what a
 * MiniProgram caller naturally writes). */
static JSValue js_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "yshttp.start(opts) requires an object");
    JSValueConst o = argv[0];
    if (!JS_IsObject(o)) return JS_ThrowTypeError(ctx, "yshttp.start: opts must be an object");

    struct job *j = (struct job *)calloc(1, sizeof(struct job));
    if (!j) return JS_ThrowInternalError(ctx, "yshttp: oom");
    j->kind = JOB_HTTP;
    j->timeout_ms = 20000;
    j->follow = 0;

    read_string_prop(ctx, o, "url", &j->url);
    read_string_prop(ctx, o, "method", &j->method);
    read_string_prop(ctx, o, "body", &j->body);
    read_string_prop(ctx, o, "cookieJarKey", &j->jar_key);

    /* timeoutMs */
    JSValue tv = JS_GetPropertyStr(ctx, o, "timeoutMs");
    if (!JS_IsUndefined(tv) && !JS_IsNull(tv)) {
        int32_t t = 0;
        if (JS_ToInt32(ctx, &t, tv) == 0 && t > 0) j->timeout_ms = t;
    }
    JS_FreeValue(ctx, tv);

    /* followRedirects */
    JSValue fv = JS_GetPropertyStr(ctx, o, "followRedirects");
    if (!JS_IsUndefined(fv) && !JS_IsNull(fv)) {
        j->follow = JS_ToBool(ctx, fv) ? 1 : 0;
    }
    JS_FreeValue(ctx, fv);

    /* headers: string or object */
    JSValue hv = JS_GetPropertyStr(ctx, o, "headers");
    if (!JS_IsUndefined(hv) && !JS_IsNull(hv)) {
        if (JS_IsString(hv)) {
            j->headers = dup_js_string(ctx, hv);
        } else if (JS_IsObject(hv)) {
            ys_buf hb;
            ys_buf_init(&hb);
            JSPropertyEnum *tab = NULL;
            uint32_t n = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &n, hv, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < n; i++) {
                    const char *k = JS_AtomToCString(ctx, tab[i].atom);
                    if (!k) continue;
                    JSValue val = JS_GetPropertyStr(ctx, hv, k);
                    const char *vs = JS_ToCString(ctx, val);
                    if (vs && *vs) {
                        if (hb.len) ys_buf_puts(&hb, "\r\n");
                        ys_buf_puts(&hb, k);
                        ys_buf_puts(&hb, ": ");
                        ys_buf_puts(&hb, vs);
                    }
                    if (vs) JS_FreeCString(ctx, vs);
                    JS_FreeValue(ctx, val);
                    JS_FreeAtom(ctx, tab[i].atom);
                    JS_FreeCString(ctx, k);
                }
                js_free(ctx, tab);
            }
            j->headers = hb.data ? hb.data : NULL;
        }
        JS_FreeValue(ctx, hv);
    }

    JSValue rv = JS_GetPropertyStr(ctx, o, "body");
    (void)rv;
    JS_FreeValue(ctx, rv);

    if (!j->url || !*j->url) {
        job_free(j);
        return JS_ThrowTypeError(ctx, "yshttp.start: opts.url is required");
    }
    if (j->body) j->body_len = strlen(j->body);
    if (!j->method) j->method = strdup(j->body ? "POST" : "GET");

    int id = job_start(j);
    if (!id) return JS_ThrowInternalError(ctx, "yshttp: pthread_create failed");
    return JS_NewInt32(ctx, id);
}

/* take(id) -> JSON string, or null while still running. */
static JSValue js_take(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    int32_t id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) != 0 || id <= 0) return JS_NULL;

    struct job *j = NULL;
    struct job **pp = NULL;
    pthread_mutex_lock(&g_mu);
    pp = &g_jobs;
    while (*pp && (*pp)->id != id) pp = &(*pp)->next;
    j = *pp;
    if (!j || j->state != 1) {
        pthread_mutex_unlock(&g_mu);
        return JS_NULL;
    }
    *pp = j->next;
    pthread_mutex_unlock(&g_mu);

    JSValue res = JS_NewString(ctx, j->result ? j->result : "{}");
    job_free(j);
    return res;
}

/* cancelAll(): page teardown. Completed jobs are freed, running ones are
 * flagged and free themselves when their transfer returns. */
static JSValue js_cancel_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    struct job **pp;
    pthread_mutex_lock(&g_mu);
    pp = &g_jobs;
    while (*pp) {
        struct job *j = *pp;
        if (j->state == 1) {
            *pp = j->next;
            job_free(j);
        } else {
            j->cancelled = 1;
            pp = &j->next;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return JS_UNDEFINED;
}

/* --------------------------------------------------------------- fast ops */

static JSValue js_hmac_sha256(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "hmacSha256(keyHex, data)");
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *d = JS_ToCString(ctx, argv[1]);
    JSValue r = JS_NULL;
    if (k && d) {
        char *h = ys_hmac_sha256_hex(k, strlen(k), d, strlen(d));
        if (h) { r = JS_NewString(ctx, h); free(h); }
    }
    if (k) JS_FreeCString(ctx, k);
    if (d) JS_FreeCString(ctx, d);
    return r;
}

static JSValue js_sha256(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *d = JS_ToCString(ctx, argv[0]);
    JSValue r = JS_NULL;
    if (d) {
        char *h = ys_sha256_hex(d, strlen(d));
        if (h) { r = JS_NewString(ctx, h); free(h); }
        JS_FreeCString(ctx, d);
    }
    return r;
}

/* rsaEncrypt(plain[, pubkeyB64]) -> base64 | null */
static JSValue js_rsa_encrypt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *p = JS_ToCString(ctx, argv[0]);
    char *pub = argc >= 2 ? dup_js_string(ctx, argv[1]) : NULL;
    JSValue r = JS_NULL;
    if (p) {
        char *err = NULL;
        char *e = ys_rsa_encrypt_b64(pub, p, strlen(p), &err);
        if (e) { r = JS_NewString(ctx, e); free(e); }
        if (err) free(err);
        JS_FreeCString(ctx, p);
    }
    free(pub);
    return r;
}

static JSValue js_b64_enc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *d = JS_ToCString(ctx, argv[0]);
    JSValue r = JS_NULL;
    if (d) {
        char *e = ys_b64_encode(d, strlen(d));
        if (e) { r = JS_NewString(ctx, e); free(e); }
        JS_FreeCString(ctx, d);
    }
    return r;
}

static JSValue js_b64_dec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *d = JS_ToCString(ctx, argv[0]);
    JSValue r = JS_NULL;
    if (d) {
        size_t n = 0;
        unsigned char *b = ys_b64_decode(d, &n);
        if (b) { r = JS_NewStringLen(ctx, (const char *)b, n); free(b); }
        JS_FreeCString(ctx, d);
    }
    return r;
}

/* ---- cookie jar (text accessors; the JS side never sees libcurl) ---- */

static JSValue js_jar_dump(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    char *key = dup_js_string(ctx, argv[0]);
    JSValue r = JS_NULL;
    if (key) {
        char *d = ys_jar_dump(key);
        if (d) { r = JS_NewString(ctx, d); free(d); }
        free(key);
    }
    return r;
}

static JSValue js_jar_import(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_NewInt32(ctx, -1);
    char *key = dup_js_string(ctx, argv[0]);
    char *txt = dup_js_string(ctx, argv[1]);
    int rc = -1;
    if (key && txt) rc = ys_jar_import(key, txt);
    free(key); free(txt);
    return JS_NewInt32(ctx, rc);
}

static JSValue js_jar_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_NewInt32(ctx, -1);
    char *key = dup_js_string(ctx, argv[0]);
    char *path = dup_js_string(ctx, argv[1]);
    int rc = -1;
    if (key && path) rc = ys_jar_save(key, path);
    free(key); free(path);
    return JS_NewInt32(ctx, rc);
}

static JSValue js_jar_load(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_NewInt32(ctx, -1);
    char *key = dup_js_string(ctx, argv[0]);
    char *path = dup_js_string(ctx, argv[1]);
    int rc = -1;
    if (key && path) rc = ys_jar_load(key, path);
    free(key); free(path);
    return JS_NewInt32(ctx, rc);
}

static JSValue js_jar_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    char *key = dup_js_string(ctx, argv[0]);
    int rc = -1;
    if (key) rc = ys_jar_clear(key);
    free(key);
    return JS_NewInt32(ctx, rc);
}

static JSValue js_jar_count(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    char *key = dup_js_string(ctx, argv[0]);
    int c = -1;
    if (key) c = ys_jar_count(key);
    free(key);
    return JS_NewInt32(ctx, c);
}

/* ---------------------------------------------------------------- wsrtt */

/* wsRttStart(url, count, timeoutMs) -> id */
static JSValue js_ws_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "wsRttStart(url, count, timeoutMs)");
    struct job *j = (struct job *)calloc(1, sizeof(struct job));
    if (!j) return JS_ThrowInternalError(ctx, "ysws: oom");
    j->kind = JOB_WSRTT;
    j->url = dup_js_string(ctx, argv[0]);
    j->count = 20;
    j->timeout_ms = 3000;
    if (argc >= 2) {
        int32_t c = 0;
        if (JS_ToInt32(ctx, &c, argv[1]) == 0 && c > 0) j->count = c;
    }
    if (argc >= 3) {
        int32_t t = 0;
        if (JS_ToInt32(ctx, &t, argv[2]) == 0 && t > 0) j->timeout_ms = t;
    }
    if (!j->url || !*j->url) {
        job_free(j);
        return JS_ThrowTypeError(ctx, "wsRttStart: url required");
    }
    int id = job_start(j);
    if (!id) return JS_ThrowInternalError(ctx, "ysws: pthread_create failed");
    return JS_NewInt32(ctx, id);
}

/* wsRttTake(id) -> JSON | null  (same table as yshttp.take) */
static JSValue js_ws_take(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return js_take(ctx, this_val, argc, argv);
}

static JSValue js_now_ms(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    /* Date.now() exists in QuickJS, but routing it through the native clock
     * keeps the JS timeout arithmetic and the curl timeouts on one clock. */
    return JS_NewInt64(ctx, ys_now_ms());
}

/* ---------------- module registration ---------------- */

static int yshttp_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "start", JS_NewCFunction(ctx, js_start, "start", 1));
    JS_SetPropertyStr(ctx, def, "take", JS_NewCFunction(ctx, js_take, "take", 1));
    JS_SetPropertyStr(ctx, def, "cancelAll", JS_NewCFunction(ctx, js_cancel_all, "cancelAll", 0));
    JS_SetPropertyStr(ctx, def, "hmacSha256", JS_NewCFunction(ctx, js_hmac_sha256, "hmacSha256", 2));
    JS_SetPropertyStr(ctx, def, "sha256", JS_NewCFunction(ctx, js_sha256, "sha256", 1));
    JS_SetPropertyStr(ctx, def, "rsaEncrypt", JS_NewCFunction(ctx, js_rsa_encrypt, "rsaEncrypt", 2));
    JS_SetPropertyStr(ctx, def, "b64Encode", JS_NewCFunction(ctx, js_b64_enc, "b64Encode", 1));
    JS_SetPropertyStr(ctx, def, "b64Decode", JS_NewCFunction(ctx, js_b64_dec, "b64Decode", 1));
    JS_SetPropertyStr(ctx, def, "jarDump", JS_NewCFunction(ctx, js_jar_dump, "jarDump", 1));
    JS_SetPropertyStr(ctx, def, "jarImport", JS_NewCFunction(ctx, js_jar_import, "jarImport", 2));
    JS_SetPropertyStr(ctx, def, "jarSave", JS_NewCFunction(ctx, js_jar_save, "jarSave", 2));
    JS_SetPropertyStr(ctx, def, "jarLoad", JS_NewCFunction(ctx, js_jar_load, "jarLoad", 2));
    JS_SetPropertyStr(ctx, def, "jarClear", JS_NewCFunction(ctx, js_jar_clear, "jarClear", 1));
    JS_SetPropertyStr(ctx, def, "jarCount", JS_NewCFunction(ctx, js_jar_count, "jarCount", 1));
    JS_SetPropertyStr(ctx, def, "nowMs", JS_NewCFunction(ctx, js_now_ms, "nowMs", 0));

    JS_SetModuleExport(ctx, m, "default", def);
    JS_SetModuleExport(ctx, m, "start", JS_NewCFunction(ctx, js_start, "start", 1));
    JS_SetModuleExport(ctx, m, "take", JS_NewCFunction(ctx, js_take, "take", 1));
    JS_SetModuleExport(ctx, m, "cancelAll", JS_NewCFunction(ctx, js_cancel_all, "cancelAll", 0));
    JS_SetModuleExport(ctx, m, "hmacSha256", JS_NewCFunction(ctx, js_hmac_sha256, "hmacSha256", 2));
    JS_SetModuleExport(ctx, m, "sha256", JS_NewCFunction(ctx, js_sha256, "sha256", 1));
    JS_SetModuleExport(ctx, m, "rsaEncrypt", JS_NewCFunction(ctx, js_rsa_encrypt, "rsaEncrypt", 2));
    JS_SetModuleExport(ctx, m, "b64Encode", JS_NewCFunction(ctx, js_b64_enc, "b64Encode", 1));
    JS_SetModuleExport(ctx, m, "b64Decode", JS_NewCFunction(ctx, js_b64_dec, "b64Decode", 1));
    JS_SetModuleExport(ctx, m, "jarDump", JS_NewCFunction(ctx, js_jar_dump, "jarDump", 1));
    JS_SetModuleExport(ctx, m, "jarImport", JS_NewCFunction(ctx, js_jar_import, "jarImport", 2));
    JS_SetModuleExport(ctx, m, "jarSave", JS_NewCFunction(ctx, js_jar_save, "jarSave", 2));
    JS_SetModuleExport(ctx, m, "jarLoad", JS_NewCFunction(ctx, js_jar_load, "jarLoad", 2));
    JS_SetModuleExport(ctx, m, "jarClear", JS_NewCFunction(ctx, js_jar_clear, "jarClear", 1));
    JS_SetModuleExport(ctx, m, "jarCount", JS_NewCFunction(ctx, js_jar_count, "jarCount", 1));
    JS_SetModuleExport(ctx, m, "nowMs", JS_NewCFunction(ctx, js_now_ms, "nowMs", 0));
    return 0;
}

static JSModuleDef *yshttp_module_load(JSContext *ctx, const char *moduleName) {
    if (strcmp(moduleName, "yshttp") != 0) return NULL;
    JSModuleDef *m = JS_NewCModule(ctx, moduleName, yshttp_module_init);
    if (!m) return NULL;
    static const char *names[] = {
        "default", "start", "take", "cancelAll", "hmacSha256", "sha256",
        "rsaEncrypt", "b64Encode", "b64Decode", "jarDump", "jarImport",
        "jarSave", "jarLoad", "jarClear", "jarCount", "nowMs"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        JS_AddModuleExport(ctx, m, names[i]);
    return m;
}

static int ysws_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "rttStart", JS_NewCFunction(ctx, js_ws_start, "rttStart", 3));
    JS_SetPropertyStr(ctx, def, "rttTake", JS_NewCFunction(ctx, js_ws_take, "rttTake", 1));
    JS_SetPropertyStr(ctx, def, "take", JS_NewCFunction(ctx, js_ws_take, "take", 1));
    JS_SetPropertyStr(ctx, def, "cancelAll", JS_NewCFunction(ctx, js_cancel_all, "cancelAll", 0));
    JS_SetPropertyStr(ctx, def, "nowMs", JS_NewCFunction(ctx, js_now_ms, "nowMs", 0));
    JS_SetModuleExport(ctx, m, "default", def);
    JS_SetModuleExport(ctx, m, "rttStart", JS_NewCFunction(ctx, js_ws_start, "rttStart", 3));
    JS_SetModuleExport(ctx, m, "rttTake", JS_NewCFunction(ctx, js_ws_take, "rttTake", 1));
    JS_SetModuleExport(ctx, m, "take", JS_NewCFunction(ctx, js_ws_take, "take", 1));
    JS_SetModuleExport(ctx, m, "cancelAll", JS_NewCFunction(ctx, js_cancel_all, "cancelAll", 0));
    JS_SetModuleExport(ctx, m, "nowMs", JS_NewCFunction(ctx, js_now_ms, "nowMs", 0));
    return 0;
}

static JSModuleDef *ysws_module_load(JSContext *ctx, const char *moduleName) {
    if (strcmp(moduleName, "ysws") != 0) return NULL;
    JSModuleDef *m = JS_NewCModule(ctx, moduleName, ysws_module_init);
    if (!m) return NULL;
    static const char *names[] = { "default", "rttStart", "rttTake", "take", "cancelAll", "nowMs" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        JS_AddModuleExport(ctx, m, names[i]);
    return m;
}

extern void registerCModuleLoader(const char *moduleName, LoadCModuleFunction loader);

/* NOTE: custom_init_jsapis() lives in jsapi_ysplayer.c (the project's single
 * JSAPI entry point). Defining it here too would be a duplicate symbol at link
 * time, so this file only offers ys_net_register_jsapis(), which that entry
 * point calls. */
void ys_net_register_jsapis(void) {
    registerCModuleLoader("yshttp", &yshttp_module_load);
    registerCModuleLoader("ysws", &ysws_module_load);
}
