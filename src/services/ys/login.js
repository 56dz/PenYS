/* 米哈游通行证扫码登录：四段链的第 ①+② 段。
 *
 * 流程（.tmp/yys/docs/account.md + examples/login.js）：
 *   1. POST /account/ma-cn-passport/web/createQRLogin   {}        -> {url, ticket}
 *   2. POST /account/ma-cn-passport/web/queryQRLoginStatus {ticket} 轮询 2s
 *        status: Created -> Scanned -> Confirmed
 *   3. POST /account/ma-cn-session/web/webVerifyForGame  (cookie 已在 jar 里)
 *   4. POST /hk4e_cn/combo/granter/login/webLogin        {app_id, channel_id}
 *   5. POST /gamer/api/login                             {}   (让云游戏服务端建 session)
 *
 * 关键设计（每一条都是踩过的坑）：
 *   - **cookie 全程走 native cookie jar**，JS 层不碰 Cookie 头。
 *     Falcon 的 http jsapi 拿不到响应头 -> 拿不到 Set-Cookie -> 拿不到
 *     stoken/ltoken；而这三步全靠 cookie 传递登录态。
 *   - **轮询用 Date.now() 判超时，不数 tick**。词典笔 300s 熄屏，
 *     熄屏时 JS 定时器会被节流甚至停摆，数 tick 会把"慢"误判成"没轮询"。
 *   - **Expired（-3501）不是终点**：重新 createQRLogin 换一张码继续，
 *     由上层 UI 决定要不要展示新码。
 *   - device_id / device_fp / lifecycle_id 必须**会话内稳定**，
 *     否则风控（-3006 请求过于频繁）会连环触发。
 */

import * as C from './const.js';
import http from './http.js';
import storage, { JAR_KEY_NAME } from './storage.js';

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


/* 登录态存在内存 + 落盘各一份。内存是真相，落盘是为了冷启动免扫码。
 * jarKey 必须与 storage.js 的 JAR_KEY 一致（都是 'ys'），否则落盘的 cookie
 * 和请求用的 jar 是两个桶，冷启动会假死。 */
var state = {
  session: null,      /* {uid, nickname, avatar, openId, comboToken, accountType, cookie, aid, ...} */
  device: null,       /* {deviceId, deviceFp, lifecycleId, miReferrer} */
  jarKey: storage.JAR_KEY_NAME,
  jarReady: false,
  pending: null       /* {ticket, url, createdAt, expiresAt} */
};

/* 冷启动时把磁盘上的 jar 灌回 native（只做一次）。
 * 失败不抛错：最多是"要重新扫码"，不该阻塞登录入口。 */
async function ensureJar() {
  if (state.jarReady) return;
  state.jarReady = true;
  try {
    storage.loadJar();
  } catch (e) {
    safeWarn('[ys] loadJar failed: ' + ((e && e.message) || e));
  }
}

function device() {
  if (state.device) return state.device;
  state.device = {
    deviceId: http.randomUuid(),
    deviceFp: http.randomHex(32),
    lifecycleId: http.randomHex(10),
    miReferrer: C.PROFILE.referrer
  };
  return state.device;
}

/* 通行证公共头。account.md 的消融结论：
 * 缺 x-rpc-app_id -> -3005 参数不合法；
 * 缺 x-rpc-client_type / device_id / mi_referrer / lifecycle_id -> -3006 请求过于频繁。
 * 所以这五个是硬性的，不能"优化"掉。 */
export function passportHeaders() {
  var d = device();
  return {
    'x-rpc-app_id': C.PASSPORT_APP_ID,
    'x-rpc-sdk_version': C.SDK_VERSION,
    'x-rpc-game_biz': C.GAME_BIZ,
    'x-rpc-client_type': String(C.PROFILE.comboClientType),
    'x-rpc-device_id': d.deviceId,
    'x-rpc-device_fp': d.deviceFp,
    'x-rpc-device_model': encodeURIComponent(C.PROFILE.deviceModel),
    'x-rpc-device_name': encodeURIComponent(C.PROFILE.deviceName),
    'x-rpc-device_os': encodeURIComponent(C.PROFILE.sysVersion),
    'x-rpc-lifecycle_id': d.lifecycleId,
    'x-rpc-mi_referrer': d.miReferrer,
    'x-rpc-channel': 'mihoyo',
    'x-rpc-language': 'zh-cn',
    'origin': C.PROFILE.origin,
    'referer': C.PROFILE.referrer
  };
}

/* 云游戏侧公共头（调度 6 步和 gamer/api/login 共用）。
 * client_type 这里要用 **cloudClientType**（WebTouch=18），不是 combo 的 24 —— 
 * 两套枚举混用会被拒。 */
export function cloudHeaders(extra) {
  var d = device();
  var h = {
    'x-rpc-app_id': String(C.APP_ID),
    'x-rpc-app_version': C.APP_VERSION,
    'x-rpc-client_type': String(C.PROFILE.cloudClientType),
    'x-rpc-cg_game_biz': C.GAME_BIZ,
    'x-rpc-op_biz': C.OP_BIZ,
    'x-rpc-channel': 'mihoyo',
    'x-rpc-cps': C.PROFILE.cps,
    'x-rpc-language': 'zh-cn',
    'x-rpc-sys_version': encodeURIComponent(C.PROFILE.sysVersion),
    'x-rpc-device_id': d.deviceId,
    'x-rpc-device_name': encodeURIComponent(C.PROFILE.deviceName),
    'x-rpc-device_model': encodeURIComponent(C.PROFILE.deviceModel),
    'x-rpc-vendor_id': C.VENDOR_ID,
    'origin': C.PROFILE.origin,
    'referer': C.PROFILE.referrer
  };
  if (state.session && state.session.comboToken) {
    /* 注意：这里要的是 buildComboToken 拼出来的整串，不是裸 combo_token。
     * pitfalls #16 专门写了这一点。 */
    h['x-rpc-combo_token'] = state.session.comboHeader || buildComboHeader(state.session);
  }
  if (extra) {
    for (var k in extra) {
      if (Object.prototype.hasOwnProperty.call(extra, k)) h[k] = extra[k];
    }
  }
  return h;
}

/* combo token 的五段拼装：ai/ci/oi/ct/si/bi。
 * si 是对 {app_id, channel_id, open_id, combo_token} 做 HMAC-SHA256，
 * key 是 COMBO_APP_KEY（不是 paasDispatch 那把！用错得 e8ce9d35...）。 */
export function buildComboHeader(session) {
  var c = {
    app_id: C.APP_ID,
    channel_id: C.CHANNEL_ID,
    open_id: session.openId,
    combo_token: session.comboToken
  };
  var si = http.hmacSha256(C.COMBO_APP_KEY, signPayload(c));
  return 'ai=' + C.APP_ID + ';ci=' + C.CHANNEL_ID + ';oi=' + session.openId +
         ';ct=' + session.comboToken + ';si=' + si + ';bi=' + C.GAME_BIZ;
}

/* 签名串 = 按 key 字典序拼 k=v&，去掉末尾 &。
 * 这是全部 HMAC 步骤的共同格式，dispatch.js 也复用它。 */
export function signPayload(obj) {
  var keys = [];
  for (var k in obj) {
    if (Object.prototype.hasOwnProperty.call(obj, k)) keys.push(k);
  }
  keys.sort();
  var parts = [];
  for (var i = 0; i < keys.length; i++) parts.push(keys[i] + '=' + obj[keys[i]]);
  return parts.join('&');
}

/* 第 1 步：取二维码。返回 {ticket, qrText, url, expiresAt}。
 * qrText 就是米哈游给的 url 本身 —— UI 用 qrcode.js 的 encodeQr(qrText)
 * 直接画，不需要再加工。 */
export async function loginBeginQr() {
  await ensureJar();
  var res = await http.requestJson({
    url: C.PASSPORT_BASE + '/account/ma-cn-passport/web/createQRLogin',
    method: 'POST',
    headers: passportHeaders(),
    body: {},
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: state.jarKey
  });
  if (res.code !== 0 || !res.data) {
    throw bizError(res.code, 'createQRLogin failed: ' + res.code + ' ' + res.message);
  }
  var ticket = res.data.ticket;
  var url = res.data.url;
  if (!ticket || !url) throw new Error('createQRLogin missing ticket/url');

  /* expire= 是 unix 秒，直接读出来给 UI 做倒计时；读不到就按 180s 估 */
  var expiresAt = 0;
  var m = /[?&]expire=(\d+)/.exec(url);
  if (m) expiresAt = parseInt(m[1], 10) * 1000;
  if (!expiresAt) expiresAt = Date.now() + 180000;

  state.pending = { ticket: ticket, url: url, createdAt: Date.now(), expiresAt: expiresAt };
  return { ticket: ticket, qrText: url, url: url, expiresAt: expiresAt };
}

/* 第 2 步：单次轮询。返回 status 五个字符串之一。
 * 把 -3501 映射成 'Expired' 而不是抛错 —— 上层据此重新取码。 */
export async function loginPollStatus(ticket) {
  await ensureJar();
  var res = await http.requestJson({
    url: C.PASSPORT_BASE + '/account/ma-cn-passport/web/queryQRLoginStatus',
    method: 'POST',
    headers: passportHeaders(),
    body: { ticket: ticket },
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: state.jarKey
  });

  /* 每个 ticket 只打一次形状日志（真机验收要看到未扫码时的
   * data.status / tokens，以及 -3503 QRLoginDeviceInconsistent 是否复发）。
   * 每轮都打会把 DictPen_*.log 刷爆，所以按 ticket 去重。 */
  if (state.pollLoggedTicket !== ticket) {
    state.pollLoggedTicket = ticket;
    var shape = res.data || {};
    var tok = shape.tokens;
    safeWarn('[ys] queryQRLoginStatus 形状: code=' + res.code +
      ' status=' + String(shape.status === undefined ? '(无)' : shape.status) +
      ' tokens=' + (Array.isArray(tok) ? ('Array(' + tok.length + ')')
        : (tok === undefined ? 'undefined' : typeof tok)) +
      ' dataKeys=' + (res.data ? Object.keys(res.data).join(',') : '(无 data)'));
  }

  if (res.code === C.RETCODE.QR_EXPIRED) {
    return { status: 'Expired', code: res.code, message: res.message };
  }
  if (res.code !== 0) {
    var kind = C.classifyRetcode(res.code);
    /* -3006 之类的限流不能当致命错：上层会重试，抛错会让 UI 直接跳失败页。
     * 但 'human'（-3101 极验）和 'fatal' 必须冒泡 —— 继续轮询只会白等。 */
    if (kind === 'human' || kind === 'fatal') {
      throw bizError(res.code, 'queryQRLoginStatus failed: ' + res.code + ' ' + res.message);
    }
    return { status: 'Created', code: res.code, message: res.message, retryable: true, kind: kind };
  }
  var d = res.data || {};
  var raw = String(d.status || 'Created');
  if (raw !== 'Created' && raw !== 'Scanned' && raw !== 'Confirmed' && raw !== 'Expired') {
    raw = 'Created';
  }
  var out = { status: raw, code: 0, scannedAt: d.scanned_at, appId: d.app_id };
  var ui = d.user_info;
  if (ui) {
    out.nickname = ui.nickname || ui.mid || '';
    out.avatar = ui.head_icon || '';
    out.uid = String(ui.uid || '');
  }
  if (raw === 'Confirmed') {
    out.tokens = d.tokens || [];
  }
  return out;
}

/* 构造带业务标记的错误。UI 靠这些字段分支，不要只看 message。
 *
 *   e.code     业务 retcode（-3101 / -100 / -3501 ...）或自定义字符串
 *   e.kind     'relogin'|'qraz'|'human'|'ratelimit'|'fatal'|'NO_NATIVE'
 *   e.needAigis true 表示必须去手机上过极验（词典笔做不了）
 *   e.hint     给用户看的中文提示
 *
 * 为什么必须这么细：`-3101 NeedAigis` 会让客户端必须跑 geetest v4 的 JS，
 * 词典笔既没有浏览器内核也没有拖拽交互，唯一正路是引导用户去手机。
 * 如果只抛 `new Error('-3101')`，UI 只能显示一个数字或者转圈。 */
function bizError(code, message, extra) {
  var kind = C.classifyRetcode(typeof code === 'number' ? code : -99999);
  var e = new Error(message || (C.describeRetcode(code, '') || ('retcode ' + code)));
  e.code = code;
  e.kind = kind;
  e.hint = kind === 'human' ? C.HINT_AIGIS : C.describeRetcode(code, message);
  if (kind === 'human') e.needAigis = true;
  if (extra) {
    for (var k in extra) {
      if (Object.prototype.hasOwnProperty.call(extra, k)) e[k] = extra[k];
    }
  }
  return e;
}

/* 第 2b 步：带超时和"过期自动换码"的完整轮询。
 * onUpdate(state) 会被反复调用，UI 据此刷新文案。
 * 注意：这里所有超时判断都基于 Date.now()，不是 tick 计数 ——
 * 词典笔 300s 熄屏时 JS 定时器会被节流，数 tick 会把"慢"误判成"没轮询"。 */
export async function loginWaitQr(opts) {
  opts = opts || {};
  var onUpdate = opts.onUpdate || function () {};
  var interval = opts.intervalMs || C.QR_POLL_INTERVAL_MS;
  var hardTimeout = opts.timeoutMs || C.QR_POLL_TIMEOUT_MS;
  var begin = Date.now();

  var qr = await loginBeginQr();
  onUpdate({ status: 'Created', qrText: qr.qrText, ticket: qr.ticket, expiresAt: qr.expiresAt });

  while (true) {
    if (Date.now() - begin > hardTimeout) {
      throw new Error('QR_LOGIN_TIMEOUT: 等了 ' + Math.round((Date.now() - begin) / 1000) + 's 仍未确认');
    }
    await sleep(interval);
    var st = await loginPollStatus(qr.ticket);

    if (st.status === 'Expired') {
      /* 码过期不是失败：换一张继续等，UI 会重画 */
      qr = await loginBeginQr();
      onUpdate({ status: 'Created', qrText: qr.qrText, ticket: qr.ticket, expiresAt: qr.expiresAt, renewed: true });
      continue;
    }
    onUpdate(st);
    if (st.status === 'Confirmed') return { ticket: qr.ticket, status: st };
    if (st.status === 'Scanned') continue;
  }
}

function sleep(ms) {
  return new Promise(function (resolve) { setTimeout(resolve, ms); });
}

/* 第 3 步：webVerifyForGame。
 * 扫码确认后 passport 已经把 stoken 写进 jar（Set-Cookie），这一步是
 * 用 cookie 换"游戏可见的登录态"。失败一般是 -100：说明 cookie 没带上。 */
export async function webVerifyForGame() {
  var h = passportHeaders();
  h['x-rpc-source'] = 'v2.webLogin';
  var res = await http.requestJson({
    url: C.PASSPORT_BASE + '/account/ma-cn-session/web/webVerifyForGame',
    method: 'POST',
    headers: h,
    body: {},
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: state.jarKey
  });
  if (res.code !== 0) {
    throw bizError(res.code, 'webVerifyForGame failed: ' + res.code + ' ' + describe(res));
  }
  return res.data || {};
}

/* 第 4 步：combo 取票。
 * 返回 data.{open_id, combo_token, account_type, fatigue_remind}。
 * 这一步之后 jar 里还多了 combo 相关 cookie，gamerLogin 要用。 */
export async function webLoginCombo() {
  var h = passportHeaders();
  h['x-rpc-channel_id'] = String(C.CHANNEL_ID);
  h['x-rpc-mdk_version'] = C.MDK_VERSION;
  var res = await http.requestJson({
    url: C.SDK_BASE + '/' + C.GAME_BIZ + '/combo/granter/login/webLogin',
    method: 'POST',
    headers: h,
    body: { app_id: C.APP_ID, channel_id: C.CHANNEL_ID },
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: state.jarKey
  });
  if (res.code !== 0 || !res.data) {
    throw bizError(res.code, 'webLogin failed: ' + res.code + ' ' + describe(res));
  }
  var d = res.data;
  if (!d.combo_token || !d.open_id) throw new Error('webLogin missing combo_token/open_id');
  return d;
}

/* 第 5 步：gamer/api/login。
 * 作用只是让云游戏服务端建 session 并下发 cookie —— body 是空对象。
 * 必须在调度前调用，否则 listPingServer 会 -100。 */
export async function gamerLogin() {
  var res = await http.requestJson({
    url: C.CLOUD_BASE + '/gamer/api/login',
    method: 'POST',
    headers: cloudHeaders({ 'content-type': 'application/json' }),
    body: {},
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: state.jarKey
  });
  if (res.code !== 0) {
    throw bizError(res.code, 'gamer login failed: ' + res.code + ' ' + describe(res));
  }
  return res.data || {};
}

/* 第 6 步（对外）：Confirmed 之后把 3/4/5 串起来，落盘会话。
 *
 * ticket 是一次性的：轮询里拿到 Confirmed 之后它就已经被消费，
 * 再查同一个 ticket 服务端直接回 -502 登录凭证无效（真机 22:12 实测：
 * poll #13 Confirmed 拿到 Set-Cookie，poll #14 回查即 -502）。
 * 所以调用方必须把轮询里已拿到的 Confirmed 结果当 knownStatus 传进来，
 * 不许回查；knownStatus 缺失或不是 Confirmed 时才回查一次兜底。 */
export async function loginComplete(ticket, knownStatus) {
  var st = knownStatus;
  if (!st || st.status !== 'Confirmed') {
    st = await loginPollStatus(ticket);
    if (st.status !== 'Confirmed') {
      return { ok: false, status: st.status, message: '二维码未确认（当前 ' + st.status + '）' };
    }
  }

  await webVerifyForGame();
  var combo = await webLoginCombo();

  var session = {
    uid: '',
    nickname: '',
    avatar: '',
    openId: combo.open_id,
    comboToken: combo.combo_token,
    accountType: combo.account_type,
    fatigueRemind: combo.fatigue_remind,
    aid: '',
    createdAt: Date.now()
  };
  session.comboHeader = buildComboHeader(session);

  /* gamerLogin 用的是带 combo 头的 cloudHeaders，所以要先塞进 state */
  state.session = session;
  var gamer = await gamerLogin();
  session.aid = gamer.aid || '';

  var ui = (st && st.nickname) ? st : null;
  if (ui) {
    session.nickname = ui.nickname || '';
    session.avatar = ui.avatar || '';
    session.uid = ui.uid || '';
  }
  /* queryQRLoginStatus 里没给 user_info 时，捞一次账号信息兜底 */
  if (!session.nickname) {
    try {
      var me = await http.requestJson({
        url: C.PASSPORT_BASE + '/account/ma-cn-session/web/verifyCookieToken',
        method: 'POST',
        headers: passportHeaders(),
        body: {},
        timeoutMs: C.HTTP_TIMEOUT_MS,
        cookieJarKey: state.jarKey
      });
      if (me.code === 0 && me.data) {
        session.uid = String(me.data.user_info ? (me.data.user_info.uid || '') : (me.data.uid || ''));
        if (me.data.user_info) {
          session.nickname = me.data.user_info.nickname || '';
          session.avatar = me.data.user_info.head_icon || '';
        }
      }
    } catch (e) {
      /* 拿不到昵称不影响协议链，只是 UI 少个名字 */
      safeWarn('[ys] verifyCookieToken for profile failed: ' + e.message);
    }
  }

  await storage.saveSession(session);
  storage.saveJar();
  state.pending = null;

  return {
    ok: true,
    status: 'Confirmed',
    nickname: session.nickname || '旅行者',
    avatar: session.avatar || '',
    uid: session.uid || ''
  };
}

/* 冷启动恢复。从磁盘读会话 + cookie jar。 */
/* 安全日志：Falcon 有 console，但设备上的裸 JS 宿主（以及某些精简固件）
 * 可能没有。日志不该成为 import 链的致命依赖。 */
function warn(msg) {
  try {
    if (typeof console !== 'undefined' && console && typeof console.warn === 'function') {
      safeWarn(msg);
    }
  } catch (e) { /* 日志失败绝不影响主流程 */ }
}

export async function restore() {
  if (state.session) return state.session;
  var s = await storage.loadSession();
  if (!s || !s.comboToken) return null;
  state.session = s;
  if (!s.comboHeader) s.comboHeader = buildComboHeader(s);
  storage.loadJar();
  state.jarReady = true;
  return s;
}

/* **同步**判定（契约要求 bool，不是 Promise）。
 *
 * 千万不能写成 async：UI 是 `if (isLoggedIn())` 直接判断，Promise 恒为真值，
 * 会让未登录用户也看到"已登录"界面。 */
export function isLoggedIn() {
  return !!(state.session && state.session.comboToken);
}

export function getSession() {
  return state.session;
}

/* 当前 cookie jar 的名字。dispatch.js 需要它来保证所有请求共用同一个桶
 * （passport 登下的 cookie 必须在云游戏请求里也带上）。 */
export function jarKey() {
  return state.jarKey;
}

export async function logout() {
  try {
    await http.requestJson({
      url: C.PASSPORT_BASE + '/account/ma-cn-session/web/logout',
      method: 'POST',
      headers: passportHeaders(),
      body: {},
      timeoutMs: 5000,
      cookieJarKey: state.jarKey
    });
  } catch (e) {
    /* 登出失败不影响本地清理 */
  }
  state.session = null;
  state.pending = null;
  await storage.clearSession();
  storage.clearJar();
  return { ok: true };
}

function describe(res) {
  return (res && (res.message || res.raw)) ? String(res.message || res.raw).slice(0, 200) : '';
}

export { bizError };

export default {
  bizError: bizError,
  passportHeaders: passportHeaders,
  loginBeginQr: loginBeginQr,
  loginPollStatus: loginPollStatus,
  loginWaitQr: loginWaitQr,
  loginComplete: loginComplete,
  webVerifyForGame: webVerifyForGame,
  webLoginCombo: webLoginCombo,
  gamerLogin: gamerLogin,
  cloudHeaders: cloudHeaders,
  buildComboHeader: buildComboHeader,
  signPayload: signPayload,
  restore: restore,
  isLoggedIn: isLoggedIn,
  getSession: getSession,
  jarKey: jarKey,
  logout: logout
};
