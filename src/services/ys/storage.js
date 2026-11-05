/* 会话持久化：cookie jar 落盘 + 登录态缓存。
 *
 * 两层存储，职责不同：
 *   1. cookie jar  —— native 管。jarSave/jarLoad 直接读写文本文件（Netscape 格式），
 *      因为 native 是唯一知道 cookie 语义的地方，JS 不该解析它。
 *   2. 会话元数据 —— JS 管（uid / nickname / open_id / combo_token / 设备 id）。
 *      走 Falcon 的 getStorage/setStorage，让 app 重装前能记住"上次登录的是谁"。
 *
 * 为什么 jar 不也走 Falcon storage：jar 是 native 的桶，落盘路径必须和
 * native 的 fopen 一致；而 Falcon storage 写的是 app 私有目录，JS 拿不到路径。
 * 所以 jar 走 native 的固定路径（应用数据目录下 ys_cookies.txt）。
 */

/* 复用 http.js 的通用 native 模块加载器（import('yshttp') 字面量预热 +
 * typeof require 守卫 + globalThis.__yshttp 测试注入）。
 * 不能在这里自己写 require('yshttp')：falcon 运行时没有全局 require
 * （2026-11-05 实测 'require' is not defined），jar 会永远读不到。 */
import { nativeModule } from './http.js';

const STORE_KEY = 'ys_session_v1';

/* Falcon storage 的坑（PenBili/src/services/storage.js 实测）：
 *   - 必须对象形态：getStorage({key}) / setStorage({key, data})
 *   - 传 value/content 会落盘为空串
 *   - 字符串形态 setStorage(k,v) 的 promise 永不 settle
 * 所以这里统一对象形态，并且给 set 加超时兜底，绝不让 Promise 悬着。 */
function falconStorage() {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  const s = f && f.jsapi && f.jsapi.storage;
  if (!s) return null;
  if (typeof s.getStorage === 'function' && typeof s.setStorage === 'function') return s;
  return null;
}

const WRITE_TIMEOUT_MS = 2500;

export function loadSession() {
  const s = falconStorage();
  if (!s) return Promise.resolve(null);
  return new Promise(function (resolve) {
    let done = false;
    function finish(v) {
      if (done) return;
      done = true;
      resolve(v);
    }
    setTimeout(function () { finish(null); }, 2000);
    try {
      Promise.resolve(s.getStorage({ key: STORE_KEY })).then(function (res) {
        let txt = '';
        if (res && typeof res === 'object' && typeof res.data === 'string') txt = res.data;
        else if (typeof res === 'string') txt = res;
        if (!txt) return finish(null);
        let obj = null;
        try { obj = JSON.parse(txt); } catch (e) { obj = null; }
        finish(obj);
      }).catch(function () { finish(null); });
    } catch (e) {
      finish(null);
    }
  });
}

export function saveSession(obj) {
  const s = falconStorage();
  if (!s) return Promise.resolve(false);
  let txt;
  try { txt = JSON.stringify(obj || {}); } catch (e) { return Promise.resolve(false); }
  return new Promise(function (resolve) {
    let done = false;
    function finish(v) {
      if (done) return;
      done = true;
      resolve(v);
    }
    // setStorage 的 promise 在真机上可能永不 settle（PenBili 注释），必须自己兜底
    setTimeout(function () { finish(false); }, WRITE_TIMEOUT_MS);
    try {
      Promise.resolve(s.setStorage({ key: STORE_KEY, data: txt }))
        .then(function () { finish(true); })
        .catch(function () { finish(false); });
    } catch (e) {
      finish(false);
    }
  });
}

export function clearSession() {
  return saveSession(null);
}

/* ---- cookie jar 落盘（走 native） ---- */

const JAR_KEY = 'ys';

/* native 的 jarSave 需要一个可写路径。词典笔上：
 *   /tmp 是 tmpfs，进程活得比 /tmp 长就没了；
 *   /userdata 可写且持久，但需要子目录。
 * 实际选 <应用数据目录> 更稳，但我们拿不到确切路径，所以退而求其次：
 * 先试 /userdata/penys，失败就落到 /tmp（进程生命周期足够一次登录会话）。
 * 两条路径都失败时只是"下次要重新扫码"，不影响本次运行。 */
const JAR_PATHS = ['/userdata/penys/ys_cookies.txt', '/tmp/ys_cookies.txt'];

export function saveJar() {
  const m = loadHttp();
  if (!m || typeof m.jarSave !== 'function') return false;
  for (let i = 0; i < JAR_PATHS.length; i++) {
    try {
      const rc = m.jarSave(JAR_KEY, JAR_PATHS[i]);
      if (rc === 0) return true;
    } catch (e) { /* 试下一条 */ }
  }
  return false;
}

export function loadJar() {
  const m = loadHttp();
  if (!m || typeof m.jarLoad !== 'function') return false;
  for (let i = 0; i < JAR_PATHS.length; i++) {
    try {
      const rc = m.jarLoad(JAR_KEY, JAR_PATHS[i]);
      if (rc === 0) return true;
    } catch (e) { /* 试下一条 */ }
  }
  return false;
}

export function dumpJar() {
  const m = loadHttp();
  if (!m || typeof m.jarDump !== 'function') return '';
  try { return m.jarDump(JAR_KEY) || ''; } catch (e) { return ''; }
}

export function importJar(text) {
  const m = loadHttp();
  if (!m || typeof m.jarImport !== 'function') return -1;
  try { return m.jarImport(JAR_KEY, text); } catch (e) { return -1; }
}

export function clearJar() {
  const m = loadHttp();
  if (!m || typeof m.jarClear !== 'function') return false;
  try { m.jarClear(JAR_KEY); return true; } catch (e) { return false; }
}

/* native 模块加载：统一走 http.js 的三路径加载器。
 *   1. 测试宿主 globalThis.__yshttp
 *   2. require —— 仅当 typeof require === 'function'（Node 测试用）
 *   3. import('yshttp') 字面量 —— falcon 生产路径（唯一能用的）
 * 同步拿不到就返回 null 并在后台点火；调用方若在 async 上下文里，
 * 先 await http.ensureNative() 再调这些 jar 函数。
 */
function loadHttp() {
  try {
    return nativeModule('yshttp') || null;
  } catch (e) {
    return null;
  }
}

export const JAR_KEY_NAME = JAR_KEY;
export const SESSION_KEY = STORE_KEY;

export default {
  SESSION_KEY: STORE_KEY,
  JAR_KEY_NAME: JAR_KEY,
  JAR_PATHS: JAR_PATHS,
  loadSession: loadSession,
  saveSession: saveSession,
  clearSession: clearSession,
  saveJar: saveJar,
  loadJar: loadJar,
  dumpJar: dumpJar,
  importJar: importJar,
  clearJar: clearJar,
};
