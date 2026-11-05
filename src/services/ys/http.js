/* yshttp native 模块的 Promise 封装。
 *
 * 为什么要这一层：
 *   1. native 的 start()/take() 是"投递 + 轮询"的裸接口，调用方不该关心 job id；
 *   2. Falcon 里所有业务代码都是 async/await 风格，包成 Promise 才好串；
 *   3. 超时判定必须用 Date.now() 而不是"轮询了多少次"——词典笔息屏时
 *      定时器会被节流甚至停跳，数 tick 的写法会永远等下去。
 *
 * native 契约（libjsapi_ysplayer.so 模块 `yshttp`）：
 *   start({url, method, headers, body, timeoutMs, followRedirects, cookieJarKey}) -> id
 *   take(id) -> JSON 字符串 | null（null = 还没完）
 *   返回的 JSON：{ kind:"http", rc, ok, status, insecure, headers, body, error }
 *     rc   0=传输成功（即使 4xx/5xx），非 0=libcurl 错误码
 *     headers 是原始响应头文本（只含最后一跳）
 *     insecure  true 表示走了"跳过证书校验"的降级路径
 */

import { HTTP_TIMEOUT_MS, classifyRetcode, describeRetcode, HINT_AIGIS } from './const.js';

/* 安全日志：Falcon 有 console，但设备上的裸 JS 宿主（以及某些精简固件）
 * 可能没有。日志绝不该成为 import 链的致命依赖 —— 曾经因为
 * `console is not defined` 让整个 import 直接炸掉。 */
function safeWarn(msg) {
  try {
    if (typeof console !== 'undefined' && console && typeof console.warn === 'function') {
      console.warn(msg);
    }
  } catch (e) { /* ignore */ }
}


const MODULE = 'yshttp';

let mod = null;
let loadError = '';

/* 懒加载：**绝不能在模块顶层 require('yshttp')**。
 *
 * 原因（这是一个会让整个 app 白屏的坑）：`ys_client.js` 是
 * `require('./ys/index.js')` 包在 try/catch 里的，但 try/catch 只能接住
 * 抛出的异常；如果 index.js 顶部就 require 一个不存在的 C 模块，异常会在
 * 模块实例化阶段抛出 —— 那时 index.vue 的 import 链已经断了，UI 直接白屏，
 * 用户看到的是黑屏而不是"native 缺失"的提示。
 *
 * 所以所有 native 访问都走这里：函数内加载 + try/catch + 明确的错误对象。
 * 这里只有两个模块级变量（mod / loadError），import 时零 IO、零 require。
 */
function load() {
  if (mod) return mod;
  if (loadError) return null;
  try {
    // 测试注入点：设备上的独立 JS 宿主（.tmp/feas/net/jsrunner3）没有
    // Falcon 的 require，但它会把 C 模块命名空间挂到 globalThis.__yshttp。
    // 生产环境走下面的 require(MODULE)。
    const g = typeof globalThis !== 'undefined' ? globalThis : null;
    let m = g && g.__yshttp ? g.__yshttp : null;
    if (!m) {
      // Falcon 的 C 模块加载方式：require 模块名，默认导出即注册的 JS object
      m = require(MODULE);
    }
    mod = (m && m.default) ? m.default : m;
    if (!mod || typeof mod.start !== 'function') throw new Error('yshttp 缺少 start()');
    return mod;
  } catch (e) {
    loadError = (e && e.message) ? e.message : String(e);
    safeWarn('[ys/http] native 模块加载失败: ' + loadError);
    return null;
  }
}

/* 构造一个 UI 能识别的错误对象。
 *
 * 为什么要带 code：UI 按 code 分支（'NO_NATIVE' -> 显示"原生模块缺失"、
 * -3101 -> 显示"请在手机上完成验证"、-100 -> 回扫码页）。
 * 只抛 new Error('xxx') 的话 UI 只能正则匹配 message，非常脆。 */
export function ysError(code, message, extra) {
  const e = new Error(message || String(code));
  e.code = code;
  if (extra) {
    const ks = Object.keys(extra);
    for (let i = 0; i < ks.length; i++) e[ks[i]] = extra[ks[i]];
  }
  return e;
}

/* 所有需要 native 的同步原语的统一取模块入口。
 * 拿不到就抛 NO_NATIVE，而不是抛 undefined is not a function。 */
function needNative(what) {
  const m = load();
  if (!m) {
    throw ysError('NO_NATIVE', 'yshttp 模块未加载: ' + (loadError || '未知原因'), {
      needNative: true,
      nativeModule: MODULE,
      detail: loadError,
      what: what || '',
    });
  }
  return m;
}

export function isAvailable() {
  return !!load();
}

export function unavailableReason() {
  load();
  return loadError;
}

/* 通用 native 模块加载器（yshttp / ysws 共用）。
 *
 * 两条路径：
 *   1. 生产：Falcon 注入 require()，C 模块用 `require('模块名')` 加载，
 *      命名空间对象上有一个 `default` 聚合。
 *   2. 设备侧独立 JS 宿主（.tmp/feas/net/jsrunner2，做真机验证用）：
 *      没有 require，把 C 模块命名空间直接挂到 globalThis.__yshttp / __ysws。
 *      这是为了让同一份 JS 代码在两种宿主下跑出可比的结果 ——
 *      否则"真机验证"验的是另一份代码。
 */
var moduleCache = {};

export function nativeModule(name) {
  if (moduleCache[name] !== undefined) return moduleCache[name];
  var m = null;
  try {
    var g = typeof globalThis !== 'undefined' ? globalThis : null;
    if (g && g['__' + name]) {
      m = g['__' + name];
    } else if (typeof require === 'function') {
      m = require(name);
    }
  } catch (e) {
    m = null;
  }
  var mod2 = (m && m.default) ? m.default : m;
  moduleCache[name] = mod2 || null;
  return moduleCache[name];
}

/* ---- 小工具 ---- */

export function now() {
  const m = load();
  if (m && typeof m.nowMs === 'function') {
    const v = m.nowMs();
    if (typeof v === 'number' && v > 0) return v;
  }
  return Date.now();
}

export function randomHex(n) {
  const chars = '0123456789abcdef';
  let s = '';
  for (let i = 0; i < n; i++) s += chars.charAt(Math.floor(Math.random() * 16));
  return s;
}

export function randomUuid() {
  function seg(len) {
    let s = '';
    for (let i = 0; i < len; i++) s += randomHex(1);
    return s;
  }
  return seg(8) + '-' + seg(4) + '-4' + seg(3) + '-a' + seg(3) + '-' + seg(12);
}

/* 对象 -> "K: V\r\nK2: V2\r\n"。native 两种形式都收，但显式拼字符串
 * 能保证顺序（服务端对 header 顺序不敏感，但对**存在性**敏感）。 */
const CRLF = String.fromCharCode(13, 10);

export function headersToText(h) {
  if (!h) return '';
  const lines = [];
  const keys = Object.keys(h);
  for (let i = 0; i < keys.length; i++) {
    const k = keys[i];
    const v = h[k];
    if (v === undefined || v === null || v === '') continue;
    lines.push(k + ': ' + v);
  }
  return lines.length ? (lines.join(CRLF) + CRLF) : '';
}

/* 把原始响应头文本切成 { 'lower-case-name': 'value', ... }。
 * 重复的 header（尤其 Set-Cookie）保留在 setCookie 数组里，不丢。 */
export function parseHeaders(raw) {
  const out = { setCookie: [] };
  if (!raw || typeof raw !== 'string') return out;
  const lines = raw.split(/\r?\n/);
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (!line) continue;
    if (line.indexOf('HTTP/') === 0) continue;
    const idx = line.indexOf(':');
    if (idx <= 0) continue;
    const k = line.slice(0, idx).trim().toLowerCase();
    const v = line.slice(idx + 1).trim();
    if (k === 'set-cookie') {
      out.setCookie.push(v);
      out['set-cookie'] = v;   // 兼容只取最后一条的调用方
    } else {
      out[k] = v;
    }
  }
  return out;
}

/* ---- 业务 retcode 检查 ---- */

/* 米哈游的接口几乎全是 **HTTP 200 + body.retcode != 0**。
 * 这是全链最容易出事的地方：如果只判 HTTP 状态码，登录失败会显示成功。
 *
 * 所以把判定抽成一个函数，所有调用点都过它：
 *
 *   const json = await ...;
 *   const r = checkRetcode(json);          // {ok, code, kind, message, hint}
 *   if (!r.ok && r.kind === 'relogin') ...
 *
 * 返回的 kind 直接给 UI 分支用（见 const.js 的 classifyRetcode）：
 *   ok / relogin / qraz / human / ratelimit / fatal
 */
export function checkRetcode(body, opts) {
  opts = opts || {};
  const code = (body && typeof body.retcode === 'number') ? body.retcode : -99999;
  const message = (body && body.message) ? String(body.message) : '';
  const kind = classifyRetcode(code);
  const out = {
    ok: code === 0,
    code: code,
    kind: kind,
    message: message,
    raw: body,
  };
  if (kind === 'human') out.hint = HINT_AIGIS;
  else if (kind !== 'ok') out.hint = describeRetcode(code, message);
  if (!out.ok && opts.throwOnError) {
    const err = new Error('RETCODE ' + code + ': ' + out.hint);
    err.code = code;
    err.kind = kind;
    err.hint = out.hint;
    throw err;
  }
  return out;
}

/* 从响应对象直接判（requestJson 的返回已经摊平了 retcode -> code） */
export function checkJson(res, opts) {
  const body = { retcode: res && res.code, message: res && res.message, data: res && res.data };
  return checkRetcode(body, opts);
}


export function request(opts) {
  const m = load();
  if (!m) {
    return Promise.reject(ysError('NO_NATIVE', 'yshttp 模块未加载: ' + (loadError || '未知原因'), {
      needNative: true,
      nativeModule: MODULE,
      detail: loadError,
    }));
  }

  const timeoutMs = opts.timeoutMs || HTTP_TIMEOUT_MS;
  const method = opts.method || (opts.body !== undefined && opts.body !== null ? 'POST' : 'GET');

  const payload = {
    url: opts.url,
    method: method,
    headers: headersToText(opts.headers),
    body: opts.body === undefined || opts.body === null ? '' : String(opts.body),
    timeoutMs: timeoutMs,
    followRedirects: opts.followRedirects !== false,
    cookieJarKey: opts.cookieJarKey || '',
  };

  return new Promise(function (resolve, reject) {
    let id;
    try {
      id = m.start(payload);
    } catch (e) {
      return reject(new Error('yshttp.start 抛错: ' + ((e && e.message) || e)));
    }
    if (!id || id <= 0) return reject(new Error('yshttp.start 返回了非法 job id'));

    // 硬超时：native 自己有 CURLOPT_TIMEOUT_MS，这里再留 3s 余量兜底，
    // 防止 native 线程因故没回填而我们永远挂着。
    const deadline = now() + timeoutMs + 3000;

    function poll() {
      let raw;
      try {
        raw = m.take(id);
      } catch (e) {
        return reject(new Error('yshttp.take 抛错: ' + ((e && e.message) || e)));
      }
      if (raw === null || raw === undefined) {
        if (now() > deadline) {
          return reject(new Error('NET_TIMEOUT: ' + method + ' ' + opts.url));
        }
        setTimeout(poll, 40);
        return;
      }
      let r;
      try {
        r = JSON.parse(raw);
      } catch (e) {
        return reject(new Error('yshttp 返回了非法 JSON: ' + String(raw).slice(0, 200)));
      }
      const res = {
        ok: !!(r.ok && r.rc === 0),
        rc: r.rc,
        status: r.status || 0,
        insecure: !!r.insecure,
        headers: r.headers || '',
        hdr: parseHeaders(r.headers || ''),
        body: r.body || '',
        error: r.error || '',
      };
      if (!res.ok) {
        return reject(new Error('HTTP_FAIL: ' + (res.error || ('curl rc=' + res.rc)) +
          ' [' + method + ' ' + opts.url + ']'));
      }
      if (res.insecure) {
        safeWarn('[ys/http] 走了跳过证书校验的降级路径: ' + opts.url);
      }
      resolve(res);
    }

    poll();
  });
}

/* 便捷包装：JSON 请求/响应 + 业务 retcode 检查。
 *
 * 注意这里**不**因为 retcode!=0 就 throw —— 米哈游的 -100（未登录）、
 * -3501（二维码过期）都是正常的业务状态，调用方需要拿到 code 做分支，
 * 所以统一 resolve {code, message, data}，由调用方判断。 */
export function requestJson(opts) {
  const headers = {};
  const src = opts.headers || {};
  const keys = Object.keys(src);
  for (let i = 0; i < keys.length; i++) headers[keys[i]] = src[keys[i]];
  if (!headers['content-type'] && !headers['Content-Type']) {
    headers['content-type'] = 'application/json';
  }
  const o = {};
  const okeys = Object.keys(opts);
  for (let i = 0; i < okeys.length; i++) o[okeys[i]] = opts[okeys[i]];
  o.headers = headers;
  if (o.body !== undefined && o.body !== null && typeof o.body !== 'string') {
    o.body = JSON.stringify(o.body);
  }
  return request(o).then(function (res) {
    let json = null;
    try {
      json = JSON.parse(res.body);
    } catch (e) {
      /* 不是 JSON（404 页、网关错误页、空 body）也要把原文交出去 ——
       * 直接 throw 会把 res.body 丢掉，排查时看不到服务端到底说了什么。
       * 所以在错误对象上挂 body / status / url。 */
      throw ysError('NOT_JSON', '响应不是 JSON [' + opts.url + ']: ' + String(res.body).slice(0, 200), {
        url: opts.url,
        status: res.status,
        body: res.body,
        headers: res.headers,
        hdr: res.hdr,
      });
    }
    return {
      code: typeof json.retcode === 'number' ? json.retcode : -99999,
      message: json.message || '',
      data: json.data,
      status: res.status,
      hdr: res.hdr,
      headers: res.headers,
      body: res.body,
      raw: json,
    };
  });
}

/* ---- 同步原语（native 里微秒级，不需要异步） ---- */

export function hmacSha256(keyHex, dataStr) {
  return needNative('hmacSha256').hmacSha256(keyHex, dataStr);
}

export function sha256(dataStr) {
  return needNative('sha256').sha256(dataStr);
}

export function rsaEncrypt(plain, pubkeyB64) {
  const m = needNative('rsaEncrypt');
  return pubkeyB64 ? m.rsaEncrypt(plain, pubkeyB64) : m.rsaEncrypt(plain);
}

export function b64Encode(s) {
  return needNative('b64Encode').b64Encode(s);
}

export function b64Decode(s) {
  return needNative('b64Decode').b64Decode(s);
}

export default {
  ysError: ysError,
  isAvailable: isAvailable,
  unavailableReason: unavailableReason,
  nativeModule: nativeModule,
  request: request,
  requestJson: requestJson,
  checkRetcode: checkRetcode,
  checkJson: checkJson,
  parseHeaders: parseHeaders,
  headersToText: headersToText,
  now: now,
  randomHex: randomHex,
  randomUuid: randomUuid,
  hmacSha256: hmacSha256,
  sha256: sha256,
  rsaEncrypt: rsaEncrypt,
  b64Encode: b64Encode,
  b64Decode: b64Decode,
};
