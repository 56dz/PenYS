/* 协议层的唯一调用入口。
 *
 * 历史：写这个文件时 services/ys/ 还不存在，为了「页面能跑」用了运行期
 * require('./ys/index.js') + try/catch 探测。代价是 **rollup 无法静态解析动态 require，
 * 那 8 个协议层文件会被整个 tree-shake 掉**（实测 .falcon_/app.js.bin 里
 * grep '7511caf0'（dispatch 的 paasDispatch 签名 key）为 0，即 dispatch.js 没进包）。
 *
 * 现在 proto-http 已全部交付，且明确保证 ys/http.js 的 native 加载是懒加载
 * （模块实例化阶段零 IO），所以可以安全地静态 import。
 *
 * ★ 2026-11-05 真机根因修复：falcon 运行时**没有全局 require**
 * （YD_PEN_APP.log 实测 `'require' is not defined`），ys/http.js 旧写法
 * require('yshttp') 永远失败 -> 顶栏恒「native 未就绪」。现在改成
 * `import('yshttp')` 异步预热 + 同步读缓存，入口就是下面的 ensureNative()。
 *
 * 保留的降级语义：call() 里「真实现缺函数 / 抛错 → 退 mock」的分支仍在，
 * 但它只服务于**单个调用失败**，不再决定「用谁」——用谁在编译期就定了。
 */

import * as real from './ys/index.js';
import * as mock from './ys_mock.js';

const active = real;

/* ★ 异步预热 native 传输层（import('yshttp') 一次，之后全走同步缓存）。
 * 页面初始化时 `await ensureNative()`；永不 reject，失败解析成 {http:false,reason}。 */
export function ensureNative() {
  try {
    if (active && typeof active.ensureNative === 'function') {
      return Promise.resolve(active.ensureNative()).catch(function (e) {
        console.warn('[ys] ensureNative 失败: ' + ((e && e.message) || e));
        return { http: false, reason: (e && e.message) || String(e) };
      });
    }
  } catch (e) {
    console.warn('[ys] ensureNative 同步抛错: ' + ((e && e.message) || e));
  }
  return Promise.resolve({ http: false, reason: '协议层未导出 ensureNative' });
}

/* 模块求值阶段就点一次火（fire-and-forget，绝不影响 import 链）。
 * 目的：让页面 created() 里那次同步 nativeUsable() 尽可能读到缓存 ——
 * import('yshttp') 只要已 resolve，nativeStatus() 就返回 true。
 * 真正的确定性保证仍是页面 init 时 await ensureNative()。 */
try {
  const warming = ensureNative();
  if (warming && typeof warming.then === 'function') warming.then(null, function () {});
} catch (e) {
  /* ignore */
}

/* 真实协议层是否齐全。缺函数据此判定，UI 可据此显示"协议层不完整"。 */
export function isReady() {
  return !!active && typeof active.loginBeginQr === 'function';
}

/* 保留导出名给 UI 用：现在恒为 false，因为编译期已绑定真实现。
 * UI 顶栏不再用它判断数据源，改用 nativeUsable()。 */
export function isMock() {
  return false;
}

/* native 侧（yshttp）是否可用 —— 这才是 UI 顶栏该显示的"数据源"信号。
 * ys.http.isAvailable() 内部是懒加载探测，不会在模块实例化阶段抛异常。 */
export function nativeUsable() {
  try {
    const st = active && active.nativeStatus ? active.nativeStatus() : null;
    return !!(st && st.http);
  } catch (e) {
    console.warn('[ys] nativeStatus 失败: ' + ((e && e.message) || e));
    return false;
  }
}

export function nativeReason() {
  try {
    const st = active && active.nativeStatus ? active.nativeStatus() : null;
    return (st && st.reason) || '';
  } catch (e) {
    return '';
  }
}

/* 统一调用：真实现缺函数 / 抛错时才退回 mock，并把降级打到日志。
 * 为什么还留 mock：native .so 缺失时（YSHTTP 没装）真实现必然全部失败，
 * 这时让 UI 退到 mock 至少页面结构还能看、能截图；错误本身会走日志，
 * 不会静默。 */
function call(name) {
  const args = Array.prototype.slice.call(arguments, 1);
  const fn = active && active[name];
  if (typeof fn !== 'function') {
    const mf = mock[name];
    console.warn('[ys] 协议层缺函数 ' + name + ' → 本次调用退 mock');
    if (typeof mf === 'function') return Promise.resolve(mf.apply(mock, args));
    return Promise.reject(new Error('ys.' + name + ' 未实现'));
  }
  try {
    return Promise.resolve(fn.apply(active, args)).catch(function (e) {
      console.warn('[ys] ' + name + ' 失败: ' + ((e && e.message) || e));
      const mf = mock[name];
      if (typeof mf === 'function') {
        console.warn('[ys] ' + name + ' 降级到 mock');
        return mf.apply(mock, args);
      }
      throw e;
    });
  } catch (e) {
    console.warn('[ys] ' + name + ' 同步抛错: ' + ((e && e.message) || e));
    throw e;
  }
}

export function loginBeginQr() { return call('loginBeginQr'); }
export function loginPollStatus(t) { return call('loginPollStatus', t); }
export function loginComplete(t) { return call('loginComplete', t); }
export function logout() { return call('logout'); }
export function fetchWallet() { return call('fetchWallet'); }
export function fetchGears() { return call('fetchGears'); }
export function dispatch(o) { return call('dispatch', o); }
export function leaveQueue() { return call('leaveQueue'); }

export function isLoggedIn() {
  if (active && typeof active.isLoggedIn === 'function') {
    try { return !!active.isLoggedIn(); } catch (e) { return false; }
  }
  return mock.isLoggedIn();
}

/* 排队快照不是契约的一部分，真实层有就用，没有就返回空对象由 UI 兜底 */
export function queueSnapshot() {
  if (active && typeof active.queueSnapshot === 'function') {
    try { return active.queueSnapshot() || {}; } catch (e) { return {}; }
  }
  return {};
}

/* 冷启动恢复会话（把落盘 cookie jar 灌回 native）。
 * 失败不算错：只是需要重新扫码。 */
export function bootstrap() {
  if (active && typeof active.bootstrap === 'function') {
    try {
      return Promise.resolve(active.bootstrap()).catch(function (e) {
        console.warn('[ys] bootstrap 失败: ' + ((e && e.message) || e));
        return { loggedIn: false };
      });
    } catch (e) {
      console.warn('[ys] bootstrap 同步抛错: ' + ((e && e.message) || e));
    }
  }
  return Promise.resolve({ loggedIn: false });
}
