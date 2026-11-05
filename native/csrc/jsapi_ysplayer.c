/* jsapi_ysplayer.c -- the ONLY JS-entry point of libjsapi_ysplayer.so.
 *
 * ============================== WHAT THIS FILE IS ============================
 *
 * Three jobs, nothing else:
 *   1) turn JS arguments into validated C values (and reject bad ones early),
 *   2) push a fixed-size command into the worker queue, returning immediately,
 *   3) read the lock-free status snapshot back into a JS object.
 *
 * There is no blocking call in this file. Every socket, every GStreamer call,
 * every sleep lives in session/session.c on its own thread. That is rule R1 of
 * the project and the reason a drag gesture cannot stutter the UI.
 *
 * ============================== WHY THIS EXISTS SEPARATELY ==================
 *
 * `custom_init_jsapis()` is the single global symbol the Falcon runtime calls
 * when it dlopen()s a native module. Two modules cannot both define it (the
 * linker would keep one and silently drop the other's registrations), so the
 * net/proto module deliberately exposes `ys_net_register_jsapis()` and this
 * file -- which owns the entry point -- calls it. If you add a third native
 * module, add its register function to the list below, never a second
 * custom_init_jsapis.
 *
 * ============================== STATE STRINGS ===============================
 *
 * The strings returned by status().state are contract with
 * src/services/player.js, which compares them literally. They come from
 * ys_state_name() in session.c, so the two cannot drift.
 */
#include <jquick_config.h>
#include <jsmodules/JSCModuleExtension.h>
#include <quickjs/quickjs.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "session/cmd.h"
#include "session/session.h"

#define YS_MODULE_NAME  "ysplayer"
#define YS_VERSION      "1.0.0"

/* Largest sdk_param we accept, in base64 characters. Shared with cmd.h so the
 * queue slot and the validation agree by construction. */
#define YS_MAX_SDK_B64  YS_MAX_SDK_PARAM_B64

/* ============================== small helpers ============================= */

static JSValue mk_bool(JSContext *ctx, const char *k, int v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, JS_NewBool(ctx, v ? 1 : 0));
    return obj;
}

static JSValue mk_int(JSContext *ctx, const char *k, long long v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, JS_NewInt64(ctx, (int64_t)v));
    return obj;
}

static JSValue mk_num(JSContext *ctx, const char *k, double v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, JS_NewFloat64(ctx, v));
    return obj;
}

static JSValue mk_str(JSContext *ctx, const char *k, const char *v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, v ? JS_NewString(ctx, v) : JS_NULL);
    return obj;
}

static JSValue mk_ok(JSContext *ctx) {
    JSValue r = JS_NewObject(ctx);
    mk_bool(ctx, "ok", 1, r);
    return r;
}

static JSValue mk_fail(JSContext *ctx, const char *code, const char *msg) {
    JSValue r = JS_NewObject(ctx);
    mk_bool(ctx, "ok", 0, r);
    mk_str(ctx, "code", code, r);
    mk_str(ctx, "error", msg, r);
    return r;
}

static const char *arg_str(JSContext *ctx, int argc, JSValueConst *argv, int i) {
    if (i >= argc) return NULL;
    if (JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return NULL;
    return JS_ToCString(ctx, argv[i]);
}

static int arg_int(JSContext *ctx, int argc, JSValueConst *argv, int i, int dflt) {
    int64_t v = dflt;
    if (i >= argc) return dflt;
    if (JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return dflt;
    if (JS_ToInt64(ctx, &v, argv[i]) != 0) return dflt;
    return (int)v;
}

static double arg_num(JSContext *ctx, int argc, JSValueConst *argv, int i, double dflt) {
    double v = dflt;
    if (i >= argc) return dflt;
    if (JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return dflt;
    if (JS_ToFloat64(ctx, &v, argv[i]) != 0) return dflt;
    return v;
}

/* Lazy one-time init. The worker thread is created here rather than in
 * custom_init_jsapis() so that merely installing the .amr does not cost a
 * thread; the first start() pays for it (~2 ms).
 * Returns 0 if the session layer is usable. */
static int ensure_init(void) {
    static volatile int tried = 0;
    static volatile int rc = -1;
    if (tried) return rc;
    tried = 1;
    ys_log_init();
    rc = ys_session_init();
    if (rc != 0) ys_logf("ysapi: ys_session_init failed");
    return rc;
}

/* ============================== video rect =================================
 * The JS layer passes logical canvas coordinates (960x266, see app.js
 * setViewPort). session.c and video/video_pipe.c own the rotation into the
 * 480x960 physical panel, including the ~107 px dead band. So this file only
 * sanity-checks; it must NOT rotate.
 */
static int valid_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (x < 0 || y < 0) return 0;
    /* The logical canvas is 960 wide and 266 tall, but allow generous slack:
     * a future page layout may letterbox further. The hard cap only exists to
     * stop NaN/garbage from reaching the KMS destination rect, where a 0x0
     * size makes the plane disappear with no error at all. */
    if (x + w > 4096 || y + h > 4096) return 0;
    return 1;
}

/* ============================== exports =================================== */

/* start(sdkParam, x, y, w, h) -> { ok, code?, error? }
 * Returns as soon as the command is queued. player.js polls status() until
 * state becomes 'playing' -- that is deliberate, because "queued" is not
 * "picture on screen" and only the worker knows the difference. */
static JSValue js_start(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    const char *sdk;
    int x, y, w, h, rc;
    (void)this_val;

    if (argc < 5) return mk_fail(ctx, "BAD_ARG", "start(sdkParam,x,y,w,h) 需要 5 个参数");
    sdk = arg_str(ctx, argc, argv, 0);
    if (!sdk || !sdk[0]) {
        if (sdk) JS_FreeCString(ctx, sdk);
        return mk_fail(ctx, "BAD_SDK_PARAM", "sdkParam 为空");
    }
    /* Length check before the worker sees it: a too-long param would be
     * rejected anyway, but failing here gives the caller a precise message
     * instead of a generic BAD_PARAM from an async path. */
    {
        size_t n = strlen(sdk);
        if (n > YS_MAX_SDK_B64) {
            JS_FreeCString(ctx, sdk);
            return mk_fail(ctx, "SDK_TOO_LONG", "sdkParam 超过上限（base64 1980 字符）");
        }
        /* A base64 blob is ASCII. Anything else means the caller passed a JSON
         * blob or a URL by mistake, which we can say out loud. */
        {
            size_t i;
            for (i = 0; i < n; i++) {
                unsigned char c = (unsigned char)sdk[i];
                if (c <= 0x20 || c > 0x7e) {
                    JS_FreeCString(ctx, sdk);
                    return mk_fail(ctx, "BAD_SDK_PARAM", "sdkParam 含非 base64 字符");
                }
            }
        }
    }

    x = arg_int(ctx, argc, argv, 1, 0);
    y = arg_int(ctx, argc, argv, 2, 0);
    w = arg_int(ctx, argc, argv, 3, 0);
    h = arg_int(ctx, argc, argv, 4, 0);
    if (!valid_rect(x, y, w, h)) {
        JS_FreeCString(ctx, sdk);
        return mk_fail(ctx, "BAD_RECT", "rect 非法（w/h 必须 > 0 且不越界）");
    }
    if (ensure_init() != 0) {
        JS_FreeCString(ctx, sdk);
        return mk_fail(ctx, "NO_WORKER", "worker 线程创建失败");
    }

    rc = ys_session_start(sdk, x, y, w, h);
    JS_FreeCString(ctx, sdk);
    if (rc == 0) return mk_ok(ctx);
    if (rc == -2) return mk_fail(ctx, "QUEUE_FULL", "命令队列满，请稍后重试");
    if (rc == -3) return mk_fail(ctx, "NO_WORKER", "session 未初始化");
    return mk_fail(ctx, "BAD_ARG", "sdkParam 或 rect 被拒");
}

static JSValue js_stop(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (ensure_init() != 0) return mk_ok(ctx);   /* nothing to stop */
    if (ys_session_stop() != 0) return mk_fail(ctx, "QUEUE_FULL", "队列满");
    return mk_ok(ctx);
}

static JSValue js_pause(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (ensure_init() != 0) return mk_fail(ctx, "NO_WORKER", "session 未初始化");
    if (ys_session_pause() != 0) return mk_fail(ctx, "QUEUE_FULL", "队列满");
    return mk_ok(ctx);
}

static JSValue js_resume(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (ensure_init() != 0) return mk_fail(ctx, "NO_WORKER", "session 未初始化");
    if (ys_session_resume() != 0) return mk_fail(ctx, "QUEUE_FULL", "队列满");
    return mk_ok(ctx);
}

static JSValue js_set_rect(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    int x, y, w, h;
    (void)this_val;
    if (argc < 4) return mk_fail(ctx, "BAD_ARG", "setRect(x,y,w,h) 需要 4 个参数");
    x = arg_int(ctx, argc, argv, 0, 0);
    y = arg_int(ctx, argc, argv, 1, 0);
    w = arg_int(ctx, argc, argv, 2, 0);
    h = arg_int(ctx, argc, argv, 3, 0);
    if (!valid_rect(x, y, w, h)) return mk_fail(ctx, "BAD_RECT", "rect 非法");
    if (ensure_init() != 0) return mk_fail(ctx, "NO_WORKER", "session 未初始化");
    if (ys_session_set_rect(x, y, w, h) != 0) return mk_fail(ctx, "QUEUE_FULL", "队列满");
    return mk_ok(ctx);
}

/* touch(nx, ny, action, fingerId) -> { ok, dropped? }
 * nx/ny are normalized to the video rect, NOT canvas pixels: the game's own
 * coordinate space is "inside my 2118x978 picture". Out-of-range values are
 * passed through untouched -- the game uses them to detect a drag leaving the
 * viewport as a cancel, and clamping here would break that. */
static JSValue js_touch(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    double nx, ny;
    int action, fid, rc;
    (void)this_val;
    if (argc < 3) return mk_fail(ctx, "BAD_ARG", "touch(nx,ny,action[,fingerId]) 需要至少 3 个参数");
    nx = arg_num(ctx, argc, argv, 0, -1.0);
    ny = arg_num(ctx, argc, argv, 1, -1.0);
    action = arg_int(ctx, argc, argv, 2, -1);
    fid = arg_int(ctx, argc, argv, 3, 0);
    if (action < 0 || action > 2) return mk_fail(ctx, "BAD_ACTION", "action 必须是 0/1/2");
    if (nx != nx || ny != ny) return mk_fail(ctx, "BAD_COORD", "坐标是 NaN");
    if (ensure_init() != 0) return mk_fail(ctx, "NO_WORKER", "session 未初始化");

    rc = ys_session_touch((float)nx, (float)ny, action, fid);
    if (rc == 0) return mk_ok(ctx);
    if (rc == 1) {
        /* Queue full and this was a MOVE: dropped on purpose, and the UI must
         * not treat it as an error (a drag emits 120 of these per second). */
        JSValue r = mk_ok(ctx);
        mk_bool(ctx, "dropped", 1, r);
        return r;
    }
    if (rc == -1) return mk_fail(ctx, "BAD_ARG", "touch 参数被拒");
    return mk_fail(ctx, "QUEUE_FULL", "队列满");
}

static JSValue js_release(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (ensure_init() != 0) return mk_ok(ctx);
    ys_session_release();
    /* Deliberately does not wait: player.js's release() already awaited stop()
     * and this is the "clear error state" half of the operation. */
    return mk_ok(ctx);
}

/* status() -> the whole observable surface. Never blocks: a seqlock read. */
static JSValue js_status(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    ys_status st;
    JSValue res;
    (void)this_val; (void)argc; (void)argv;

    memset(&st, 0, sizeof(st));
    if (ensure_init() != 0) {
        res = JS_NewObject(ctx);
        mk_bool(ctx, "ok", 0, res);
        mk_str(ctx, "state", "error", res);
        mk_str(ctx, "lastError", "NO_WORKER", res);
        mk_str(ctx, "lastErrorMsg", "session 未初始化", res);
        mk_num(ctx, "fps", 0, res);
        mk_int(ctx, "latencyMs", 0, res);
        mk_int(ctx, "bitrateKbps", 0, res);
        return res;
    }
    ys_session_snapshot(&st);

    res = JS_NewObject(ctx);
    mk_bool(ctx, "ok", st.ok, res);
    mk_str(ctx, "state", ys_state_name(st.state), res);
    mk_num(ctx, "fps", st.fps, res);
    mk_int(ctx, "latencyMs", st.latency_ms, res);
    mk_int(ctx, "bitrateKbps", st.bitrate_kbps, res);
    mk_bool(ctx, "audioOk", st.audio_ok, res);
    /* screenOff matters to the UI: while the panel sleeps the KMS plane reads
     * inactive, which is not a playback failure. player.js forwards this so the
     * page can keep the session alive instead of tearing it down. */
    mk_bool(ctx, "screenOff", st.screen_off, res);
    mk_bool(ctx, "planeActive", st.plane_active, res);
    mk_str(ctx, "videoCodec", st.video_codec, res);
    mk_str(ctx, "audioCodec", st.audio_codec, res);
    mk_int(ctx, "gameW", st.game_w, res);
    mk_int(ctx, "gameH", st.game_h, res);
    mk_int(ctx, "videoW", st.video_w, res);
    mk_int(ctx, "videoH", st.video_h, res);
    mk_str(ctx, "lastError", st.error, res);
    mk_str(ctx, "lastErrorMsg", st.error_msg, res);

    /* Diagnostics. Kept in status() rather than a separate call because the
     * on-screen debug overlay already polls status() at 5 Hz and a second
     * native call per tick buys nothing. */
    mk_int(ctx, "packetsRx", (long long)st.packets_rx, res);
    mk_int(ctx, "packetsLost", (long long)st.packets_lost, res);
    mk_int(ctx, "framesOut", (long long)st.frames_out, res);
    mk_int(ctx, "framesDropped", (long long)st.frames_dropped, res);
    mk_int(ctx, "audioFrames", (long long)st.audio_frames, res);
    mk_int(ctx, "rtpLate", (long long)st.rtp_late, res);
    mk_int(ctx, "srtpAuthFail", (long long)st.srtp_auth_fail, res);
    mk_int(ctx, "wssMs", st.wss_ms, res);
    mk_int(ctx, "dtlsMs", st.dtls_ms, res);
    mk_int(ctx, "firstFrameMs", st.first_frame_ms, res);
    mk_int(ctx, "retransmits", st.retransmits, res);
    mk_str(ctx, "cipher", st.cipher, res);
    mk_str(ctx, "srtpProfile", st.srtp_profile, res);
    return res;
}

static JSValue js_info(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    JSValue res = JS_NewObject(ctx);
    (void)this_val; (void)argc; (void)argv;
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "version", YS_VERSION, res);
    mk_str(ctx, "module", YS_MODULE_NAME, res);
    mk_str(ctx, "logPath", ys_log_path(), res);
    mk_bool(ctx, "logOn", ys_log_on(), res);
    mk_int(ctx, "maxSdkParam", YS_MAX_SDK_B64, res);
    mk_int(ctx, "workerInited", ensure_init() == 0, res);
    return res;
}

/* ============================== module wiring =============================
 *
 * Both a default export object (how this project's JS calls it: `require(name)`
 * returns the module namespace, and player.js takes `.default`) and named
 * exports are registered, matching PenDouyu's proven pattern.
 */
#define YS_EXPORTS(X)              \
    X("start",    js_start,    5)  \
    X("stop",     js_stop,     0)  \
    X("pause",    js_pause,    0)  \
    X("resume",   js_resume,   0)  \
    X("setRect",  js_set_rect, 4)  \
    X("touch",    js_touch,    4)  \
    X("release",  js_release,  0)  \
    X("status",   js_status,   0)  \
    X("info",     js_info,     0)

static int ysplayer_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);

#define YS_SET_PROP(name, fn, nargs) \
    JS_SetPropertyStr(ctx, def, name, JS_NewCFunction(ctx, fn, name, nargs));
    YS_EXPORTS(YS_SET_PROP)
#undef YS_SET_PROP

    JS_SetModuleExport(ctx, m, "default", def);

#define YS_SET_EXPORT(name, fn, nargs) \
    JS_SetModuleExport(ctx, m, name, JS_NewCFunction(ctx, fn, name, nargs));
    YS_EXPORTS(YS_SET_EXPORT)
#undef YS_SET_EXPORT
    return 0;
}

static JSModuleDef *ysplayer_module_load(JSContext *ctx, const char *moduleName) {
    JSModuleDef *m;
    if (strcmp(moduleName, YS_MODULE_NAME) != 0) return NULL;
    m = JS_NewCModule(ctx, moduleName, ysplayer_module_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
#define YS_ADD_EXPORT(name, fn, nargs) JS_AddModuleExport(ctx, m, name);
    YS_EXPORTS(YS_ADD_EXPORT)
#undef YS_ADD_EXPORT
    return m;
}

/* Implemented by net/ys_jsapi.c. Registering `yshttp` and `ysws` from here is
 * what keeps a single custom_init_jsapis in the whole shared object. */
extern void ys_net_register_jsapis(void);

JQUICK_EXPORT void custom_init_jsapis(void) {
    /* A cloud-game session writes to sockets whose peer may vanish. Without
     * SIG_IGN the default action kills the process mid-frame. */
    signal(SIGPIPE, SIG_IGN);
    ys_log_init();
    /* Startup banner: unconditional, so "did my .so even load?" is answerable
     * from /tmp/penys.log without turning full logging on first. */
    ys_log_always("ysplayer %s module init", YS_VERSION);

    registerCModuleLoader(YS_MODULE_NAME, &ysplayer_module_load);
    ys_net_register_jsapis();
}
