/* 云游戏钱包 / 时长余额。
 *
 * 端点：GET ${CLOUD_BASE}/wallet/wallet/get?cost_method=0
 * 需要登录态（cookie jar）+ 云游戏公共头。
 *
 * 返回字段（实测 payload 见下），注意**服务端把所有数字都当字符串下发**：
 *   {
 *     "total_time": "31",                       // 总可用分钟
 *     "free_time": {"free_time": "31", "free_time_limit": "600", "send_freetime": "0", "over_freetime": "0"},
 *     "coin": {"coin_num": "0", "free_coin_num": "0", "coin_limit": "150000", "exchange": "10"},
 *     "stat": {"vip_point": "500"},
 *     "play_card": {"expire": "0", "msg": "未开通...", "short_msg": "未开通",
 *                   "play_card_limit": "180", "remaining_sec": "0", "play_card_tag": "PLAYCARD_UNKNOWN"}
 *   }
 *
 * 对外契约（ui-shell 的 UI 已按这个写）：
 *   { totalTime, freeTime, playCard }
 *   - totalTime 单位**分钟**
 *   - freeTime  单位**分钟**（免费时长剩余）
 *   - playCard  单位**分钟**（畅玩卡剩余；服务端给的是秒，这里除 60 取整）
 *
 * 为什么要换算：服务端 play_card.remaining_sec 是秒，而 UI 的 mock 里
 * playCard: 3 明显是按"天/时"混用的。为了不让 UI 出现 "3 秒" 这种荒诞值，
 * 统一成分钟，并在 detail 里保留原始秒数给需要精确显示的地方。
 */

import * as C from './const.js';
import http from './http.js';
import login from './login.js';

function num(v, dflt) {
  if (v === undefined || v === null || v === '') return dflt;
  var n = typeof v === 'number' ? v : parseFloat(String(v));
  if (isNaN(n) || !isFinite(n)) return dflt;
  return n;
}

/* 主接口。失败时抛错（UI 会显示"时长获取失败"），但**不抛 -100**：
 * -100 代表登录态失效，转成 {needLogin:true} 让 UI 走扫码流程更友好。 */
export async function fetchWallet() {
  if (!login.isLoggedIn()) {
    var s = await login.restore();
    if (!s) return { totalTime: 0, freeTime: 0, playCard: 0, needLogin: true };
  }

  var res = await http.requestJson({
    url: C.CLOUD_BASE + '/wallet/wallet/get?cost_method=0',
    method: 'GET',
    headers: login.cloudHeaders(),
    timeoutMs: C.HTTP_TIMEOUT_MS,
    cookieJarKey: login.jarKey()
  });

  if (res.code === C.RETCODE.NEED_LOGIN) {
    return { totalTime: 0, freeTime: 0, playCard: 0, needLogin: true };
  }
  if (res.code !== 0 || !res.data) {
    throw new Error('getWallet failed: ' + res.code + ' ' + res.message);
  }

  var d = res.data;
  var totalMinutes = num(d.total_time, 0);
  var ft = d.free_time || {};
  var freeMinutes = num(ft.free_time, 0);
  var pc = d.play_card || {};
  var playCardSeconds = num(pc.remaining_sec, 0);
  var playCardMinutes = Math.floor(playCardSeconds / 60);

  /* total_time 有时是 0 但 free_time 有值（新账号），取两者较大值更符合直觉 */
  if (totalMinutes <= 0 && freeMinutes > 0) totalMinutes = freeMinutes;

  return {
    totalTime: Math.round(totalMinutes),
    freeTime: Math.round(freeMinutes),
    playCard: playCardMinutes,
    /* 附加信息：UI 想显示得更细可以拿来用，不显示也不影响契约 */
    detail: {
      totalMinutes: Math.round(totalMinutes),
      freeMinutes: Math.round(freeMinutes),
      freeLimitMinutes: num(ft.free_time_limit, 0),
      overFreeMinutes: num(ft.over_freetime, 0),
      coin: num((d.coin || {}).coin_num, 0),
      freeCoin: num((d.coin || {}).free_coin_num, 0),
      vipPoint: num((d.stat || {}).vip_point, 0),
      playCardSeconds: playCardSeconds,
      playCardTag: pc.play_card_tag || '',
      playCardMsg: pc.msg || '',
      playCardShortMsg: pc.short_msg || '',
      playCardExpire: num(pc.expire, 0)
    }
  };
}

/* 「还有多少时长可以玩」的一句话总结，给 UI 直接贴 */
export function describeRemaining(w) {
  if (!w) return '';
  if (w.needLogin) return '需要登录';
  var m = w.totalTime || 0;
  if (m <= 0) return '时长已用尽';
  if (m < 60) return '剩余 ' + m + ' 分钟';
  var h = Math.floor(m / 60);
  var r = m % 60;
  return r ? ('剩余 ' + h + ' 小时 ' + r + ' 分钟') : ('剩余 ' + h + ' 小时');
}

export default {
  fetchWallet: fetchWallet,
  describeRemaining: describeRemaining
};
