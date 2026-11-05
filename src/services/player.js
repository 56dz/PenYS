/* native 播放器 adapter —— 云·原神「云游戏」会话的 JS 侧唯一入口。
 *
 * 和 PenDouyu 的 player.js 不是一回事，这里要说清楚：
 *   斗鱼那种是「给一个 URL，native 自己 ffmpeg 拉流、自己 demux、自己上屏」。
 *   云原神是「给一个已签名的 sdk_param，native 走 WSS 信令拿 offer，再做 DTLS-SRTP
 *   收 RTP，交给 mppvideodec 硬解后直出 KMS 平面」。所以这里的 start() 只传一个
 *   sdk_param 字符串，重活全在 native。
 *
 * 硬约束（沿用 PenDouyu 真机教训）：
 *   1) **JS 线程零阻塞**。所有方法都是 async，native 侧调用必须立刻返回；
 *      唯一允许等待的是 start()/stop() 这种一局一次的生命周期调用，而且上限 20s。
 *   2) **状态轮询不读盘**。status() 只读 native 内存里的计数器，不碰 storage。
 *   3) **绝不 SIGSTOP 暂停进程**。暂停就是告诉服务端「停发」，本地什么都不冻。
 *   4) 页面卸载必然调 stop()，卸载路径上不许抛异常（否则后面的 cleanup 全被跳过）。
 *
 * 契约（native libjsapi_ysplayer.so，模块名 `ysplayer`）：
 *   start({ sdkParam, rect }) -> { ok, code?, error? }
 *   stop()                    -> { ok }
 *   pause() / resume()        -> { ok }
 *   setRect(rect)             -> { ok }           竖屏旋转由 native 按面板坐标算
 *   touch({ nx, ny, action, fingerId }) -> { ok } 归一化坐标，action: 'down'|'up'|'move'
 *   status()                  -> { ok, state, fps, latencyMs, bitrateKbps,
 *                                  audioOk, screenOff, planeActive, lastError, ... }
 *   info()                    -> { ok, version, module }
 */

const MODULE = 'ysplayer';

// 一局从「点了开始」到「真的出画面」要经过：HTTP 调度 → WSS 信令 → DTLS 握手 →
// 收首个 RTP 包 → 硬解首帧 → KMS 平面 ACTIVE。真机实测这几步加起来 3~8s，
// 排队时可能几十秒，所以 start 的等待窗口给到 45s，超过才算超时。
const START_TIMEOUT_MS = 45000;
const STOP_TIMEOUT_MS = 8000;

let mod = null;
let loadError = '';
let warmPromise = null;
let warmFailed = false;

/* 模块形态：C 侧 JS_NewCModule 注册的 namespace，可能是 namespace 本身，
 * 也可能是 { default: namespace }（rollup/quickjs 的 interop 差异）。 */
function adopt(m) {
  const x = (m && m.default) ? m.default : m;
  if (!x || typeof x.status !== 'function') throw new Error('ysplayer 未导出 status()');
  return x;
}

/* 真机实测：**Falcon 运行时没有 `require`**（`'require' is not defined`，
 * /userdata/applog/DictPen_*.log），C 模块只能用动态 `import()` 加载 ——
 * PenBili/PenDouyu 全项目零 require() 调用，用的都是 `await import(name)`。
 * 但 status()/touch() 这些是同步 API，不能 await，所以这里用「异步预热 + 同步读缓存」：
 * 模块一被 import 就发起预热，UI 首帧时大概率已就绪，没就绪时同步路径先回 idle。 */
function doWarm() {
  return (async () => {
    try {
      const g = (typeof globalThis !== 'undefined') ? globalThis : null;
      let m = null;
      if (g && g.__ysplayer) m = g.__ysplayer;          // 测试宿主注入点
      else if (typeof require === 'function') m = require(MODULE);  // 老宿主兜底
      else {
        /* 真机教训（2026-11-05 两轮冷启动实测）：falcon 的 C 模块 loader 与
         * app 实例同生命周期 —— boot 期 import('ysplayer') <1ms resolve，
         * 而 onUnload 之后 loader 已被清掉，那时再 import 只会 reject 出
         * could not load module filename。所以预热只在 boot 期发起一次
         * （kickWarm 单发），另加 3s 超时兜底：宿主 import 万一悬空，
         * ensureNative 也要能 settle、UI 要能拿到结论。 */
        const p = import(MODULE);
        p.then(
          () => console.warn('[ysplayer] import resolve'),
          (e) => console.warn('[ysplayer] import reject: ' + ((e && e.message) || e))
        );
        m = await Promise.race([
          p,
          new Promise((_, rej) => setTimeout(() => rej(new Error('import 超时 3000ms')), 3000)),
        ]);
      }
      mod = adopt(m);
      loadError = '';
      warmFailed = false;
      return mod;
    } catch (e) {
      loadError = (e && e.message) ? e.message : String(e);
      warmFailed = true;
      console.warn('[ysplayer] 加载失败: ' + loadError);
      return null;
    }
  })();
}

function kickWarm() {
  if (!warmPromise) warmPromise = doWarm();
  return warmPromise;
}

/* 同步读缓存；没有就触发一次预热（不等待）。失败不重试，避免每次轮询刷屏。 */
function loadModule() {
  if (mod) return mod;
  kickWarm();
  return null;
}

/* 显式等待就绪。start()/stop() 与页面初始化走这里。 */
export async function ensureNative() {
  if (mod) return mod;
  return kickWarm();
}

export function isAvailable() {
  return !!mod;
}

export function unavailableReason() {
  return loadError;
}

/* 进度态：'idle' | 'connecting' | 'playing' | 'paused' | 'error'
 * 这个状态由 native 维护，JS 只读。UI 的骨架屏/错误页都从这里来。 */
export function status() {
  const m = loadModule();
  if (!m) {
    // 还在预热 → 回 idle（UI 不该把「模块正在 import」当错误闪红）；
    // 预热失败 → 才是真 error，带上具体原因。
    return {
      ok: false, state: warmFailed ? 'error' : 'idle', fps: 0, latencyMs: 0, bitrateKbps: 0,
      audioOk: false, screenOff: false, planeActive: false, packetsLost: 0,
      lastError: loadError || (warmFailed ? 'native module missing' : ''),
    };
  }
  let s = null;
  try { s = m.status(); } catch (e) { s = null; }
  if (!s || typeof s !== 'object') {
    return {
      ok: false, state: 'error', fps: 0, latencyMs: 0, bitrateKbps: 0,
      audioOk: false, screenOff: false, planeActive: false, packetsLost: 0,
      lastError: 'status() returned nothing',
    };
  }
  return {
    ok: !!s.ok,
    state: s.state || 'idle',
    fps: Number(s.fps) || 0,
    latencyMs: Number(s.latencyMs) || 0,
    bitrateKbps: Number(s.bitrateKbps) || 0,
    audioOk: !!s.audioOk,
    screenOff: !!s.screenOff,
    planeActive: !!s.planeActive,
    videoCodec: s.videoCodec || '',
    audioCodec: s.audioCodec || '',
    gameW: Number(s.gameW) || 0,
    gameH: Number(s.gameH) || 0,
    /* native status 有（jsapi_ysplayer.c:329），UI 播放浮层要显示丢包数 */
    packetsLost: Number(s.packetsLost) || 0,
    lastError: s.lastError || '',
    lastErrorMsg: s.lastErrorMsg || '',
  };
}

export function info() {
  const m = loadModule();
  if (!m) return { ok: false, version: '', module: MODULE };
  try { return m.info() || {}; } catch (e) { return { ok: false, module: MODULE }; }
}

/* 归一化 rect：native 要的是「画布上的逻辑矩形」，四个数都必须是有限数。
 * 传 NaN 进 native 会让 KMS 的 dst rect 变成 0x0，画面直接消失且没有报错——
 * 真机踩过，所以这里必须挡在 JS 侧。 */
function normalizeRect(rect) {
  const r = rect || {};
  const x = Number(r.x), y = Number(r.y), w = Number(r.w), h = Number(r.h);
  if (!isFinite(x) || !isFinite(y) || !isFinite(w) || !isFinite(h)) return null;
  if (w <= 0 || h <= 0) return null;
  return { x: Math.round(x), y: Math.round(y), w: Math.round(w), h: Math.round(h) };
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/* 起一局。
 *
 * sdkParam 是 paasDispatch 返回的 base64 protobuf，native 自己解；
 * JS 不解析它，也就不需要把字段一个个搬进来（少一层出错面）。
 *
 * aid 是登录拿到的 gamer 账号 id。它**不在** sdk_param 里，而服务端的
 * StartGameReq 需要它才能知道该给谁开哪台机器——缺了它服务端只会回一个
 * server hello 然后彻底沉默，表现就是「服务器 offer 连接超时」。所以这里
 * 必须显式传下去，别指望从 sdk_param 里能解出来。
 *
 * 返回 { ok:true } 表示**画面已经上屏**（native 等到 KMS 平面 ACTIVE 才返回），
 * 不是「请求已发出」。这一点很重要：UI 拿到 ok 就可以直接撤掉骨架屏。
 */
export async function start({ sdkParam, rect, aid } = {}) {
  const m = await ensureNative();
  if (!m) return { ok: false, code: 'NO_MODULE', error: loadError };

  const r = normalizeRect(rect);
  if (!r) return { ok: false, code: 'BAD_RECT', error: 'rect 非法: ' + JSON.stringify(rect) };
  if (typeof sdkParam !== 'string' || sdkParam.length < 32) {
    return { ok: false, code: 'BAD_SDK_PARAM', error: 'sdkParam 为空或过短' };
  }
  const aidStr = (aid === undefined || aid === null) ? '' : String(aid).trim();

  let res;
  try {
    res = m.start(sdkParam, r.x, r.y, r.w, r.h, aidStr);
  } catch (e) {
    return { ok: false, code: 'THROW', error: (e && e.message) || String(e) };
  }
  if (!res || !res.ok) {
    return { ok: false, code: (res && res.code) || 'START_FAIL', error: (res && res.error) || 'start 失败' };
  }

  // native 的 start 是异步的：它只把命令丢进 worker 队列就返回。
  // 这里轮询到 state==='playing' 才算真起播成功。
  const deadline = Date.now() + START_TIMEOUT_MS;
  while (Date.now() < deadline) {
    const s = status();
    if (s.state === 'playing') return { ok: true };
    if (s.state === 'error') {
      return { ok: false, code: s.lastError || 'START_ERROR', error: s.lastErrorMsg || '起播失败' };
    }
    await sleep(200);
  }
  // 超时也要收拾干净，否则平面/线程会留在设备上
  await stop();
  return { ok: false, code: 'START_TIMEOUT', error: '起播超时（' + Math.round(START_TIMEOUT_MS / 1000) + 's）' };
}

export function pause() {
  const m = loadModule();
  if (!m) return { ok: false, code: 'NO_MODULE' };
  try { return m.pause() || { ok: false }; } catch (e) { return { ok: false, error: String(e) }; }
}

export function resume() {
  const m = loadModule();
  if (!m) return { ok: false, code: 'NO_MODULE' };
  try { return m.resume() || { ok: false }; } catch (e) { return { ok: false, error: String(e) }; }
}

export function setRect(rect) {
  const m = loadModule();
  if (!m) return { ok: false, code: 'NO_MODULE' };
  const r = normalizeRect(rect);
  if (!r) return { ok: false, code: 'BAD_RECT' };
  try { return m.setRect(r.x, r.y, r.w, r.h) || { ok: false }; } catch (e) { return { ok: false, error: String(e) }; }
}

/* 触摸。坐标是**相对游戏画面 rect 的归一化 [0,1]**，不是画布像素。
 *
 * 注意：竖屏旋转在这里**不做**。词典笔面板是 480x960 rotation=1，而游戏画面是
 * 横的 2118x978；这个旋转关系由 native 根据 rect 和面板方向自己算，
 * JS 只负责把手势换算成「游戏画面内的归一化坐标」。两边都转就会转两次。
 */
const ACTION_MAP = { down: 0, up: 1, move: 2, cancel: 1 };

export function touch(a, b, c, d) {
  // 两种调用形态都收，避免 UI 那边因为位置参数/对象参数写错而静默丢手势：
  //   touch({ nx, ny, action, fingerId })
  //   touch(nx, ny, action, fingerId)          // 和最初给的契约一致
  let nx, ny, action, fingerId;
  if (a && typeof a === 'object') {
    nx = a.nx; ny = a.ny; action = a.action; fingerId = a.fingerId;
  } else {
    nx = a; ny = b; action = c; fingerId = d;
  }
  const m = loadModule();
  if (!m) return { ok: false, code: 'NO_MODULE' };
  const act = ACTION_MAP[action];
  if (act === undefined) return { ok: false, code: 'BAD_ACTION', error: 'action=' + action };
  const x = Number(nx), y = Number(ny);
  if (!isFinite(x) || !isFinite(y)) return { ok: false, code: 'BAD_COORD' };
  const fid = Number(fingerId) || 0;
  try {
    // native 侧自己钳到 [0,1]，这里不钳（钳了就丢了「超出画面」这个信息，
    // 游戏需要它来判断是否取消操作）
    return m.touch(x, y, act, fid) || { ok: false };
  } catch (e) {
    return { ok: false, error: String(e) };
  }
}

/* 停一局。**必须幂等**：页面 onUnload、错误处理、用户返回主页都会调它，
 * 任何一次都不许抛异常，也不许无限等。 */
export async function stop() {
  // 卸载路径专用守卫：boot 期预热从没发起过就不发起 —— falcon 在 onUnload 后
  // 会清掉 C 模块 loader，此时 import 只会白报一条 could not load module
  // filename（真机 2026-11-05）；而 boot 没 kick 意味着这个上下文从没 start()
  // 过，native 侧没有需要停的东西。预热发起过就等它，绝不新开 import。
  let m = mod;
  if (!m && warmPromise) m = (await warmPromise) || null;
  if (!m) return { ok: true, skipped: 'NO_MODULE' };
  let res;
  try { res = m.stop(); } catch (e) { return { ok: false, error: String(e) }; }
  if (!res || !res.ok) return { ok: false, code: (res && res.code) || 'STOP_FAIL' };

  // 等到 idle（native 拆管线、释放 KMS 平面、关 socket 是异步的）
  const deadline = Date.now() + STOP_TIMEOUT_MS;
  while (Date.now() < deadline) {
    const s = status();
    if (s.state === 'idle' || s.state === 'error') return { ok: true };
    await sleep(100);
  }
  console.warn('[ysplayer] stop 等待超时，强制认为已停');
  return { ok: false, code: 'STOP_TIMEOUT' };
}

/* 复位（换账号/换局前用）：stop + 清掉 native 里的残留状态。
 * 对 UI 来说和 stop 一样，但语义更狠，用在「退出登录」路径上。 */
export async function release() {
  await stop();
  const m = loadModule();
  if (m && typeof m.release === 'function') {
    try { m.release(); } catch (e) { /* 尽力而为 */ }
  }
  return { ok: true };
}
