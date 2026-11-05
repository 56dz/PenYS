/* 云游戏调度 6 步链（.tmp/yys/docs/protocol.md 顺序表）。
 *
 *   0. POST /gamer/api/login                          {}                 （建 session）
 *   1. GET  /gamer/api/getSpeedLimitConfig?speed_client_type=8           （画质档）
 *   2. POST /dispatcher/api/statusCheck               {biz_key}
 *   3. POST /dispatcher/api/listPingServer            {biz_key, ext_data}
 *   4. WSS  RTT 测速（native wsRttTest，20 帧）
 *   5. POST /dispatcher/api/getNodesInfo              {biz_key, node, speed_client_type}
 *   6. POST /dispatcher/api/paasDispatch              （★ 会开始计费，默认拒跑）
 *
 * 第 0~5 步不扣时长，可以随便调；第 6 步一旦 retcode 0 就开始扣。
 * 所以 dispatch() 只走到 getNodesInfo 就返回，paasDispatch 单独导出成
 * dispatchPaas()，必须显式传 confirmBilling:true 才执行。
 */

import * as C from './const.js';
import http from './http.js';
import login from './login.js';
import pb from './pb.js';

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


var BIZ_KEY = C.GAME_BIZ;

/* 最近一次 getNodesInfo 的产物，paasDispatch 要用它拼 body */
var last = {
  gears: null,
  defaultGear: 0,
  nodes: null,
  pingResults: null,
  nodeId: '',
  regionIds: null,
  bitRate: 0,
  queueTicket: ''
};

function hdr(extra) {
  return login.cloudHeaders(extra);
}

function b64(str) {
  /* 设备信息的 di 字段是 base64(JSON)。用 native 的 b64Encode：
   * 这里编码的是 UTF-8 文本（英文+数字），不涉及二进制 mangle 问题。 */
  return http.b64Encode(str);
}

/* 第 1 步：画质档位。
 * data.list[i] = {gear, top, ...}，top 是 Mbps，bitRate = 1024 * top (kbps)。
 * data.default_gear 是下标（不是 gear id），要用 list[default_gear]。 */
export async function fetchGears() {
  var res = await http.requestJson({
    url: C.CLOUD_BASE + '/gamer/api/getSpeedLimitConfig?speed_client_type=' + C.PROFILE.speedClientType,
    method: 'GET',
    headers: hdr(),
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: login.jarKey()
  });
  if (res.code !== 0 || !res.data) {
    throw login.bizError(res.code, 'getSpeedLimitConfig failed: ' + res.code + ' ' + res.message);
  }
  var list = res.data.list || [];
  var gears = [];
  for (var i = 0; i < list.length; i++) {
    var g = list[i] || {};
    gears.push({
      index: i,
      gear: g.gear,
      name: g.name || g.title || ('画质 ' + (i + 1)),
      top: g.top,
      bitRate: 1024 * (g.top || 0)
    });
  }
  last.gears = gears;
  last.defaultGear = res.data.default_gear || 0;
  return { gears: gears, defaultGear: last.defaultGear };
}

/* 第 2 步：状态检查。返回 data（可能含 wait_seconds / maintenance） */
async function statusCheck() {
  var res = await http.requestJson({
    url: C.CLOUD_BASE + '/dispatcher/api/statusCheck',
    method: 'POST',
    headers: hdr({ 'content-type': 'application/json' }),
    body: { biz_key: BIZ_KEY },
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: login.jarKey()
  });
  if (res.code !== 0) {
    throw login.bizError(res.code, 'statusCheck failed: ' + res.code + ' ' + res.message);
  }
  return res.data || {};
}

/* 第 3 步：拿 ping 服务器列表。
 * ext_data 是 **JSON 字符串**（不是对象），双重转义是 pitfalls #20。 */
async function listPingServer() {
  var res = await http.requestJson({
    url: C.CLOUD_BASE + '/dispatcher/api/listPingServer',
    method: 'POST',
    headers: hdr({ 'content-type': 'application/json' }),
    body: {
      biz_key: BIZ_KEY,
      ext_data: JSON.stringify({ platform: C.PROFILE.cloudClientType })
    },
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: login.jarKey()
  });
  if (res.code !== 0 || !res.data) {
    throw login.bizError(res.code, 'listPingServer failed: ' + res.code + ' ' + res.message);
  }
  return res.data.ping_svr || [];
}

/* 第 4 步：对每个 ping 服务器做 WSS RTT 测速。
 *
 * 这里**不是 HTTP**（pitfalls #7）：要开 WebSocket 连 wss_url，
 * 换 20 个 "X" 帧算平均 RTT。Falcon 的 JS 拿不到 WebSocket 客户端，
 * 所以整个探针在 native（ysws 模块的 rttStart/rttTake），
 * 且 native 里强制 AF_INET —— AF_UNSPEC 在这台固件上会卡 15 秒。
 *
 * 测速失败（rtt=-1）不是错误：该节点按 rtt=0 上报，让服务端自己挑。
 */
async function measureRtt(pingServers, perServerTimeoutMs) {
  // ysws 也要预热：falcon 没有全局 require，只能 import('ysws') 异步拿。
  // 这里 await 一次，下面 rttOnce() 的同步 nativeModule() 就能直接命中缓存。
  try { await http.ensureModule('ysws'); } catch (e) { /* 下面逐个 reject 兜底 */ }
  var results = [];
  for (var i = 0; i < pingServers.length; i++) {
    var s = pingServers[i] || {};
    var url = s.wss_url || s.url || '';
    var rtt = -1;
    if (url) {
      try {
        rtt = await rttOnce(url, perServerTimeoutMs || 2000);
      } catch (e) {
        safeWarn('[ys] rtt failed for ' + url + ': ' + ((e && e.message) || e));
        rtt = -1;
      }
    }
    results.push({
      ping_server_id: s.ping_server_id,
      node_id: s.node_id,
      display_name: s.display_name || '',
      region_id: s.region_id,
      province_id: String(s.province_id === undefined ? '' : s.province_id),
      isp_type: s.isp_type === undefined ? 0 : s.isp_type,
      rtt: rtt < 0 ? 0 : rtt,
      loss_rate: 0,
      jitter: 0,
      out_of_order: 0
    });
  }
  return results;
}

function rttOnce(url, timeoutMs) {
  return new Promise(function (resolve, reject) {
    var w = null;
    try {
      // falcon 运行时没有全局 require（实测 'require' is not defined），
      // 改走 http.js 的通用加载器：import('ysws') 预热 + 同步读缓存。
      w = http.nativeModule('ysws');
    } catch (e) {
      return reject(new Error('ysws 不可用: ' + ((e && e.message) || e)));
    }
    if (!w) {
      // 还没热完（measureRtt 通常已经 await 过 ensureModule，这里只是兜底）
      return void http.ensureModule('ysws').then(function (m) {
        if (!m) return reject(new Error('ysws 不可用: ' + (http.moduleError('ysws') || 'import 失败')));
        w = m;
        runRtt();
      }, function (e) {
        reject(new Error('ysws 不可用: ' + ((e && e.message) || e)));
      });
    }
    return runRtt();

    function runRtt() {
    var m = (w && w.default) ? w.default : w;
    if (!m || typeof m.rttStart !== 'function') return reject(new Error('ysws 缺少 rttStart'));

    var id;
    try {
      id = m.rttStart(url, 20, timeoutMs);
    } catch (e) {
      return reject(new Error('rttStart 抛错: ' + ((e && e.message) || e)));
    }
    if (!id || id <= 0) return reject(new Error('rttStart 返回非法 id'));

    /* 硬超时：native 内部有 poll 超时，这里再加 3s 兜底，绝不悬着 */
    var deadline = http.now() + timeoutMs * 3 + 3000;
    function poll() {
      var raw;
      try {
        raw = m.rttTake(id);
      } catch (e) {
        return reject(new Error('rttTake 抛错: ' + ((e && e.message) || e)));
      }
      if (raw === null || raw === undefined) {
        if (http.now() > deadline) return reject(new Error('RTT_TIMEOUT: ' + url));
        setTimeout(poll, 40);
        return;
      }
      var r;
      try { r = JSON.parse(raw); } catch (e) { return reject(new Error('rtt 返回非法 JSON')); }
      if (!r.ok || !r.recvd) return reject(new Error(r.error || 'rtt failed'));
      resolve(Math.round(r.avgRtt));
    }
      poll();
    }
  });
}

/* 第 5 步：拿节点。
 * node 也是 **JSON 字符串**；ping_results 是数组，两层嵌套都要 stringify。 */
async function getNodesInfo(pingResults) {
  var res = await http.requestJson({
    url: C.CLOUD_BASE + '/dispatcher/api/getNodesInfo',
    method: 'POST',
    headers: hdr({ 'content-type': 'application/json' }),
    body: {
      biz_key: BIZ_KEY,
      node: JSON.stringify({ bandwidth: 0, ping_results: pingResults }),
      speed_client_type: C.PROFILE.speedClientType
    },
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: login.jarKey()
  });
  if (res.code !== 0 || !res.data) {
    throw login.bizError(res.code, 'getNodesInfo failed: ' + res.code + ' ' + res.message);
  }
  var nodes = res.data.nodes || [];
  if (!nodes.length) throw new Error('getNodesInfo 返回空节点列表');
  return { node: nodes[0], wait: res.data.wait_seconds || 0, all: nodes };
}

/* 用户设备信息（进 sdk_param 的 user_data 字段）。
 * 格式来自 examples/dispatch.js：{dpi, w, h, lang, di}，
 * di = base64(JSON({deviceName, systemVersion, deviceInfo}))，
 * deviceInfo 是 'k:v&k:v' 串 —— 服务端只是原样回填到 cmd_line，不解析。 */
function buildUserData() {
  var deviceInfo = {
    deviceName: C.PROFILE.deviceName,
    systemVersion: C.PROFILE.sysVersion,
    deviceInfo: [
      'operatingSystem:' + C.PROFILE.sysVersion,
      'deviceModel:' + C.PROFILE.deviceModel,
      'processorCount:4',
      'processorType:Unknown',
      'systemMemorySize:2',
      'graphicsDeviceName:Unknown'
    ].join('&')
  };
  var userData = {
    dpi: C.PROFILE.dpi,
    w: C.PROFILE.width,
    h: C.PROFILE.height,
    lang: 'zh-CN',
    di: b64(JSON.stringify(deviceInfo))
  };
  return JSON.stringify(userData);
}

/* paasDispatch 的签名：**先签 11 字段中间对象，再往 body 里塞 6 个字段**。
 *
 * 这是全链最容易错的地方（pitfalls #1）：
 *   - 被签对象 regions 是 JSON.stringify 后的**字符串**；
 *   - 最终 body 的 regions 是**数组**（覆盖掉）；
 *   - sign/hint/net_state/queue_type/using_new_cmd_line/queue_switch 六个
 *     字段**不参与签名**。
 * 直接签最终 body 得 e71d4289...（错），正确是 198f2d8d...。
 */
export function buildPaasBody(o) {
  var base = {
    bit_rate: o.bitRate,
    biz_key: BIZ_KEY,
    cmd_line: o.cmdLine === undefined ? '' : o.cmdLine,
    user_data: o.userData,
    codec_type: o.codecType === undefined ? 1 : o.codecType,
    env: o.env === undefined ? '2' : String(o.env),
    ext_data: o.extData === undefined ? '' : o.extData,
    fps: o.fps === undefined ? 60 : o.fps,
    node_id: String(o.nodeId),
    resolution: o.resolution,
    regions: JSON.stringify(o.regions)
  };
  var sign = http.hmacSha256(C.APP_KEY, login.signPayload(base));
  return {
    bit_rate: base.bit_rate,
    biz_key: base.biz_key,
    cmd_line: base.cmd_line,
    user_data: base.user_data,
    codec_type: base.codec_type,
    env: base.env,
    ext_data: base.ext_data,
    fps: base.fps,
    node_id: base.node_id,
    resolution: base.resolution,
    regions: o.regions,                  /* 覆盖成数组 */
    hint: o.hint === undefined ? null : o.hint,
    net_state: o.netState === undefined ? 4 : o.netState,
    queue_type: o.queueType === undefined ? '' : o.queueType,
    sign: sign,
    using_new_cmd_line: true,
    queue_switch: false
  };
}

/* 主入口：跑到 getNodesInfo 为止（不扣时长）。
 *
 * 返回 {sdkParam: '', sid: '', nodeId, regionIds, bitRate, gears, rtt}。
 * sdkParam 这里是空串 —— 只有 dispatchPaas() 才会拿到真值。
 */
export async function dispatch(opts) {
  opts = opts || {};

  /* 没有登录态就别浪费时间：直接抛，让 UI 引导扫码 */
  if (!login.isLoggedIn()) {
    var restored = await login.restore();
    if (!restored) throw new Error('NOT_LOGGED_IN: 需要先扫码登录');
  }

  /* 0. gamer/api/login —— 建云游戏 session。已建过也不报错，重复调无害。 */
  try {
    await login.gamerLogin();
  } catch (e) {
    /* -100 说明 cookie 真的没了，这个必须冒泡 */
    if (String(e.message).indexOf('-100') >= 0) throw e;
    safeWarn('[ys] gamerLogin warmup: ' + e.message);
  }

  /* 1. 画质档 */
  var gearsInfo = await fetchGears();
  var gearIndex = typeof opts.gearIndex === 'number' ? opts.gearIndex : gearsInfo.defaultGear;
  if (gearIndex < 0 || gearIndex >= gearsInfo.gears.length) gearIndex = gearsInfo.defaultGear;
  var gear = gearsInfo.gears[gearIndex] || gearsInfo.gears[0];
  var bitRate = gear ? gear.bitRate : 9216000;
  var resolution = C.PROFILE.resolution;

  /* 2. 状态检查 */
  var status = await statusCheck();
  if (status.wait_seconds && status.wait_seconds > 0) {
    safeWarn('[ys] statusCheck says wait ' + status.wait_seconds + 's');
  }

  /* 3. ping 服务器 */
  var servers = await listPingServer();
  if (!servers.length) throw new Error('listPingServer 返回空列表');

  /* 4. WSS RTT 测速（native） */
  var pingResults = await measureRtt(servers);

  /* 5. 节点 */
  var nodesInfo = await getNodesInfo(pingResults);
  var node = nodesInfo.node;
  var regionIds = node.region_ids || [];
  if (!regionIds.length) throw new Error('节点缺少 region_ids（paasDispatch 无法签名）');

  last.nodes = nodesInfo.all;
  last.pingResults = pingResults;
  last.nodeId = String(node.node_id);
  last.regionIds = regionIds;
  last.bitRate = bitRate;

  return {
    sdkParam: '',
    sid: '',
    nodeId: last.nodeId,
    regionIds: regionIds,
    bitRate: bitRate,
    resolution: resolution,
    gearIndex: gearIndex,
    gear: gear,
    gears: gearsInfo.gears,
    waitSeconds: nodesInfo.wait,
    rtt: pingResults,
    pingCount: servers.length,
    billingStarted: false
  };
}

/* 第 6 步：paasDispatch。★ 一旦成功就开始扣云游戏时长。
 *
 * 两道闸：
 *   1. 必须显式传 confirmBilling: true（开发期/自动化测试不该误触）；
 *   2. 必须已经跑过 dispatch() 拿到 nodeId/regionIds。
 * 返回里带 sdkParam + 解好的 endpoints（直接喂给 session.c）。
 */
export async function dispatchPaas(opts) {
  opts = opts || {};
  if (opts.confirmBilling !== true) {
    throw new Error('BILLING_GUARD: paasDispatch 会开始计费，必须显式传 confirmBilling:true');
  }
  if (!last.nodeId || !last.regionIds) {
    throw new Error('NO_NODE: 需要先调用 dispatch() 拿到 nodeId/regionIds');
  }

  var body = buildPaasBody({
    bitRate: opts.bitRate || last.bitRate,
    userData: opts.userData || buildUserData(),
    nodeId: last.nodeId,
    resolution: opts.resolution || C.PROFILE.resolution,
    regions: last.regionIds,
    fps: opts.fps,
    env: opts.env,
    codecType: opts.codecType,
    cmdLine: opts.cmdLine,
    extData: opts.extData,
    netState: opts.netState,
    queueType: opts.queueType,
    hint: opts.hint
  });

  var res = await http.requestJson({
    url: C.CLOUD_BASE + '/dispatcher/api/paasDispatch',
    method: 'POST',
    headers: hdr({ 'content-type': 'application/json' }),
    body: body,
    timeoutMs: 20000,
    cookieJarKey: login.jarKey()
  });

  if (res.code !== 0 || !res.data) {
    throw login.bizError(res.code, 'paasDispatch failed: ' + res.code + ' ' + res.message);
  }

  var d = res.data;
  var resultCode = d.result_code || '';

  /* QUEUED：排队中，拿 ticket 让上层轮询 / 或者直接 leaveQueue 放弃 */
  if (resultCode === 'QUEUED') {
    var qi = d.queue_info || {};
    last.queueTicket = qi.ticket || '';
    return {
      ok: false,
      resultCode: 'QUEUED',
      queueTicket: last.queueTicket,
      queryInterval: qi.query_interval || 0,
      billingStarted: false,
      message: '排队中（未开始计费）'
    };
  }

  if (resultCode !== 'FINISHED') {
    throw new Error('paasDispatch 未完成: result_code=' + resultCode);
  }

  var fr = d.finish_result || {};
  var sdkParam = fr.sdk_param || '';
  if (!sdkParam) throw new Error('finish_result 缺少 sdk_param');

  /* 解码 protobuf，把 session.c 需要的字段提出来 */
  var decoded = pb.decodeSdkParam(sdkParam);
  var ep = pb.extractEndpoints(decoded);

  return {
    ok: true,
    resultCode: 'FINISHED',
    sdkParam: sdkParam,
    sid: String(fr.sid || ep.sid || ''),
    cloudProvider: fr.cloud_provider || '',
    regionId: fr.region_id || '',
    nodeId: fr.node_id || last.nodeId,
    queueType: fr.queue_type || 'normal',
    costMethod: fr.cost_method,
    gameId: fr.game_id || '',
    billingStarted: true,
    decoded: decoded,
    endpoints: ep
  };
}

/* 离开调度队列。拿到 sdk_param 后立刻调，避免占着队列槽位。
 * 注意这不能"取消计费"，计费已经在 paasDispatch 成功那一刻开始了。 */
export async function leaveQueue(ticket) {
  var t = ticket || last.queueTicket;
  if (!t) return { ok: true, skipped: true, message: '没有队列 ticket，无需离开' };
  var res = await http.requestJson({
    url: C.CLOUD_BASE + '/dispatcher/api/leaveDispatchQueue',
    method: 'POST',
    headers: hdr({ 'content-type': 'application/json' }),
    body: { biz_key: BIZ_KEY, ticket: t },
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: login.jarKey()
  });
  if (res.code !== 0) {
    return { ok: false, code: res.code, message: res.message };
  }
  last.queueTicket = '';
  return { ok: true };
}

/* 同步快照，给 UI 画"当前节点/延迟"用 */
export function queueSnapshot() {
  if (!last.nodeId) return {};
  return {
    nodeId: last.nodeId,
    regionCount: last.regionIds ? last.regionIds.length : 0,
    bitRate: last.bitRate,
    pingCount: last.pingResults ? last.pingResults.length : 0,
    rtt: last.pingResults ? last.pingResults.map(function (r) { return r.rtt; }) : []
  };
}

export default {
  fetchGears: fetchGears,
  dispatch: dispatch,
  dispatchPaas: dispatchPaas,
  buildPaasBody: buildPaasBody,
  leaveQueue: leaveQueue,
  queueSnapshot: queueSnapshot
};
