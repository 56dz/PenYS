/* 云·原神 UI 骨架用的假数据源。
 *
 * 为什么需要它：协议层（services/ys/*.js，proto-http 在写）与 native 播放层
 * 都还没就位，而 UI 必须能先跑起来 —— 否则整个项目连构建-安装-截图的闭环都打不开。
 * 这里的返回值**严格按接口契约的形状**写，UI 侧不用为 mock 写任何分支。
 *
 * 每个函数都刻意加了 200~900ms 的假延迟：真机上是网络往返，UI 的 loading / 步骤条
 * 只有在有延迟时才看得出对不对。所有延迟都是 setTimeout，页面卸载时由 UI 侧
 * 统一 clear（见 index.vue 的 _timers 记账）。
 *
 * 注意：mock 的 dispatch 返回的 sdkParam 是假的，native 一定起不来。
 * UI 侧靠 player.isAvailable() 判断，native 缺失时直接进 playing 占位态，
 * 不会把「假 sdkParam 打不通」误报成用户可见的错误。
 */

const GEARS = [
  { id: 1, name: '超高清', top: 1, bitRate: 12000 },
  { id: 2, name: '高清', top: 2, bitRate: 8000 },
  { id: 3, name: '标清', top: 3, bitRate: 4500 },
  { id: 4, name: '低清', top: 4, bitRate: 2500 },
  { id: 5, name: '极低清', top: 5, bitRate: 1200 }
];

let loggedIn = false;
let currentTicket = '';
let qrSeq = 0;
let polls = 0;

function delay(ms) {
  return new Promise(function (resolve) {
    setTimeout(resolve, ms);
  });
}

function makeId(prefix) {
  qrSeq++;
  return prefix + Date.now().toString(36) + '-' + qrSeq;
}

export function source() {
  return 'mock';
}

export function loginBeginQr() {
  // 真实实现会打米哈游 createQRLogin 拿 url + ticket。
  // 这里造一个形似的 url，让 QR 编码器有真实长度的内容可编（太短会得到 v1 码，扫不出来也不像）。
  loggedIn = false;
  polls = 0;
  currentTicket = makeId('t');
  const url = 'https://user.mihoyo.com/qr_code_in_game.html?app_id=ddxf6vlr3dn6u2tg&ticket=' +
    currentTicket + '&expire=300&platform=3';
  return delay(240).then(function () {
    return { url: url, ticket: currentTicket };
  });
}

export function loginPollStatus(ticket) {
  // 状态机：Created(等扫) -> Scanned(已扫待确认) -> Confirmed(确认)
  // mock 用轮询次数推进，5 次后到 Scanned、9 次后 Confirmed —— 这个节奏
  // 正好让人在真机上看到「等待扫码 → 已扫码待确认 → 登录成功」三段文案。
  polls++;
  return delay(120).then(function () {
    if (ticket !== currentTicket) return { status: 'Expired' };
    if (polls < 5) return { status: 'Created' };
    if (polls < 9) return { status: 'Scanned', nickname: '旅行者', avatar: '' };
    return { status: 'Confirmed', nickname: '旅行者', avatar: '' };
  });
}

export function loginComplete(ticket) {
  return delay(180).then(function () {
    loggedIn = ticket === currentTicket;
    return {
      ok: loggedIn,
      nickname: '旅行者',
      avatar: '',
      uid: '100234567'
    };
  });
}

export function logout() {
  loggedIn = false;
  currentTicket = '';
  return delay(80).then(function () {
    return { ok: true };
  });
}

export function isLoggedIn() {
  return loggedIn;
}

export function fetchWallet() {
  return delay(160).then(function () {
    // totalTime 单位是分钟（契约如此）。5820 分 = 97 小时。
    return { totalTime: 5820, freeTime: 600, playCard: 3 };
  });
}

export function fetchGears() {
  return delay(140).then(function () {
    // 深拷贝，避免 UI 侧改到常量（真机上出过一次 mock 被就地改写）
    return GEARS.map(function (g) {
      return { id: g.id, name: g.name, top: g.top, bitRate: g.bitRate };
    });
  });
}

export function dispatch(opt) {
  const gearIndex = (opt && opt.gearIndex) || 0;
  return delay(900).then(function () {
    return {
      sdkParam: 'MOCK-SDK-PARAM-' + makeId('s') + '-0000000000000000000000000000',
      sid: makeId('sid'),
      gearIndex: gearIndex,
      mocked: true
    };
  });
}

export function leaveQueue() {
  return delay(80).then(function () {
    return { ok: true };
  });
}

/* 排队进度（契约里没有，但 dispatching 页面必须有东西动）。
 * 单独导出，真实协议层若有对应字段可以覆盖；没有就一直是 0。 */
export function queueSnapshot() {
  return {
    position: 0,
    total: 0,
    node: '云端节点 · 华东一区'
  };
}
