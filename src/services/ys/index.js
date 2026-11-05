/* 协议层总入口。
 *
 * ============ 计费边界（README 会引用这段，改之前先想想） ============
 *
 *   ★ 不产生任何费用（可以随便调、随便重试）：
 *       loginBeginQr / loginPollStatus / loginComplete / logout / isLoggedIn
 *       fetchWallet        —— 只读钱包
 *       fetchGears         —— 只读画质档位
 *       dispatch()         —— 走到 getNodesInfo 为止
 *       这些接口的服务端语义是"查询/登记"，不进入排队，不计时长。
 *       dispatch() 内部虽然会调 gamer/api/login（建 session）和
 *       statusCheck / listPingServer / getNodesInfo，都是无副作用的探询。
 *
 *   ★ 会产生费用：只有 dispatchPaas()。
 *       paasDispatch 一旦返回 result_code=FINISHED，米哈游就开始扣云游戏
 *       时长（或原点），用户哪怕一帧视频都没看到也在扣。所以：
 *         - 默认抛错，必须显式传 confirmBilling: true；
 *         - 排队（QUEUED）状态不计费，此时可以不 leaveQueue 等它；
 *         - 拿到 sdk_param 后计费已开始，leaveQueue() 只能释放队列槽位，
 *           不能退费。
 *      开发/自测阶段绝对不要传 confirmBilling。
 *
 * ===================================================================
 *
 * 这个文件存在的另一个理由是 `ys_client.js` 的探测：
 *
 *     const m = require('./ys/index.js');
 *     const real = m && m.default ? m.default : m;
 *     if (real && typeof real.loginBeginQr === 'function') → 用真实现
 *     否则 → 永远跑 mock
 *
 * 所以**必须同时提供 default 聚合对象和具名导出**，缺一个都可能判成 mock。
 * 具名导出是为了别的模块 `import { dispatch } from './ys/index.js'`。
 *
 * 对外契约（ui-shell 已按这个写 UI，改签名要先跟 lead 说）：
 *   loginBeginQr()            -> Promise<{ticket, qrText, url, expiresAt}>
 *   loginPollStatus(ticket)   -> Promise<{status:'Created'|'Scanned'|'Confirmed'|'Expired', ...}>
 *   loginComplete(ticket)     -> Promise<{ok, nickname, avatar, uid}>
 *   logout()                  -> Promise<{ok}>
 *   isLoggedIn()              -> boolean（同步！）
 *   fetchWallet()             -> Promise<{totalTime, freeTime, playCard}>
 *   fetchGears()              -> Promise<{gears:[{index,name,top,bitRate}], defaultGear}>
 *   dispatch({gearIndex})     -> Promise<{sdkParam, sid, nodeId, ...}>（sdkParam 为空串）
 *   dispatchPaas(opts)        -> Promise<{sdkParam, sid, endpoints, ...}>（★计费）
 *   leaveQueue()              -> Promise<{ok}>
 *   queueSnapshot()           -> {}（同步）
 */

import * as C from './const.js';
import http from './http.js';
import storage from './storage.js';
import pb from './pb.js';
import login from './login.js';
import dispatchMod from './dispatch.js';
import walletMod from './wallet.js';

/* ---------- 登录 ---------- */

export function loginBeginQr() {
  return login.loginBeginQr();
}

export function loginPollStatus(ticket) {
  return login.loginPollStatus(ticket);
}

export function loginComplete(ticket, knownStatus) {
  return login.loginComplete(ticket, knownStatus);
}

export function logout() {
  return login.logout();
}

/* 同步判定：UI 在渲染首屏时就要决定"显示登录按钮还是时长"，
 * 不能等 Promise。内存里有会话就算登录（真正的失效由首个请求的 -100 暴露）。 */
export function isLoggedIn() {
  return login.isLoggedIn();
}

/* 便捷：一次跑完"取码 -> 等确认 -> 完成"，onUpdate 拿状态刷 UI。
 * UI 如果自己管理轮询就不需要它。 */
export function loginWaitQr(opts) {
  return login.loginWaitQr(opts);
}

/* ---------- 钱包 ---------- */

export function fetchWallet() {
  return walletMod.fetchWallet();
}

/* ---------- 调度 ---------- */

export function fetchGears() {
  return dispatchMod.fetchGears();
}

export function dispatch(opts) {
  return dispatchMod.dispatch(opts);
}

export function leaveQueue(ticket) {
  return dispatchMod.leaveQueue(ticket);
}

export function queueSnapshot() {
  return dispatchMod.queueSnapshot();
}

/* paasDispatch 单独暴露，且默认拒跑 —— 它会真实开始计费。
 * 只有显式 confirmBilling:true 才执行。 */
export function dispatchPaas(opts) {
  return dispatchMod.dispatchPaas(opts);
}

/* ---------- 杂项（UI 调试用得到） ---------- */

export function getSession() {
  return login.getSession();
}

export function restore() {
  return login.restore();
}

/* 冷启动时调一次：恢复会话 + 把落盘的 cookie jar 灌回 native。
 * 不调也能工作（只是每次都要重新扫码）。 */
export function bootstrap() {
  return login.restore().then(function (s) {
    return { loggedIn: !!s, session: s };
  });
}

export function nativeStatus() {
  return {
    http: http.isAvailable(),
    reason: http.unavailableReason()
  };
}

/* ★ 异步预热 native 传输层（import('yshttp') 一次）。
 * 页面初始化时 `await ensureNative()`，之后同步的 nativeStatus()/isAvailable()
 * 只读缓存 —— falcon 运行时没有全局 require，只能走 import()。永不 reject。 */
export function ensureNative() {
  try {
    return Promise.resolve(http.ensureNative()).then(function (m) {
      return { http: !!m, reason: m ? '' : (http.unavailableReason() || '') };
    });
  } catch (e) {
    return Promise.resolve({ http: false, reason: (e && e.message) || String(e) });
  }
}

export { C as consts, http, storage, pb, login, dispatchMod, walletMod };

/* default 聚合：ys_client.js 优先取这个。
 * 注意这里必须把 10 个契约函数全部挂上，少一个就判成 mock 不可用。 */
export default {
  loginBeginQr: loginBeginQr,
  loginPollStatus: loginPollStatus,
  loginComplete: loginComplete,
  loginWaitQr: loginWaitQr,
  logout: logout,
  isLoggedIn: isLoggedIn,
  fetchWallet: fetchWallet,
  fetchGears: fetchGears,
  dispatch: dispatch,
  dispatchPaas: dispatchPaas,
  leaveQueue: leaveQueue,
  queueSnapshot: queueSnapshot,
  getSession: getSession,
  restore: restore,
  bootstrap: bootstrap,
  nativeStatus: nativeStatus,
  ensureNative: ensureNative,
};
