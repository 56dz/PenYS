/* 云·原神 协议常量表。
 *
 * 为什么单独一个文件：两套 client_type 是这条链上最容易搞错的东西
 * （docs/pitfalls.md 第 3 条），散落在 login.js / dispatch.js 里迟早会串。
 * 另外两个 HMAC key 也放在这里，让"用错 key"这种 bug 只有一个出错点。
 */

export const GAME_BIZ = 'hk4e_cn';
export const APP_ID = 4;
export const CHANNEL_ID = 1;
export const PASSPORT_APP_ID = 'c76ync6mutq8';

export const PASSPORT_BASE = 'https://passport-api.mihoyo.com';
export const CLOUD_BASE = 'https://api-cloudgame.mihoyo.com/hk4e_cg_cn';
export const SDK_BASE = 'https://hk4e-sdk.mihoyo.com';
export const STATIC_BASE = 'https://api-cloudgame-static.mihoyo.com/hk4e_cn/cg';

/* 两个 key 绝对不能混（pitfalls #2）：
 *   - combo_token 里的 si      -> COMBO_APP_KEY
 *   - paasDispatch 的 sign     -> APP_KEY
 * 混用会得到 e8ce9d35... 这种"看起来像哈希但服务端拒绝"的值。 */
export const APP_KEY = '7511caf05a67bb7ca33b1970ce6222a8';
export const COMBO_APP_KEY = 'd0d3a7342df2026a70f650b907800111';

/* 两套 client_type（pitfalls #3），用途完全不同：
 *   comboClientType —— passport / combo 接口用（CloudWeb*）
 *   cloudClientType —— 云游戏 cloudgame 接口用（Web*）
 *   speedClientType —— getSpeedLimitConfig 的 query 参数
 * X6 Pro 走的"网页版云端"路径 = touch 档位（服务端认 2532x1170 那套 UA 组合）。 */
export const PROFILE = {
  platform: 'android',
  comboClientType: 24,
  cloudClientType: 18,
  speedClientType: 8,
  cps: 'touch_mihoyo',
  sysVersion: 'Android 13',
  deviceName: 'Unknown',
  deviceModel: 'Unknown',
  bundleId: 'hk4e_cn_webtouch',
  dpi: 480,
  width: 2532,
  height: 1170,
  resolution: '2532x1170',
  referrer: 'https://ys.mihoyo.com/cloud/m/#/',
  origin: 'https://ys.mihoyo.com',
};

export const APP_VERSION = '7.2.0';
export const SDK_VERSION = '2.57.0';
export const COMBO_SDK_VERSION = '2.50.1';
export const MDK_VERSION = '2.57.0';
export const VENDOR_ID = '2';
export const OP_BIZ = 'clgm_cn';

/* 轮询节奏。60000 是"扫完码等人点确认"的窗口，不是网络超时。 */
export const QR_POLL_INTERVAL_MS = 2000;
export const QR_POLL_TIMEOUT_MS = 60000;
export const HTTP_TIMEOUT_MS = 15000;

/* 业务错误码 -> 人话，UI 直接拿去显示 */
export const RETCODE = {
  OK: 0,
  NEED_LOGIN: -100,
  RATE_LIMITED: -3006,
  BAD_PARAMS: -3005,
  NEED_AIGIS: -3101,
  BAD_PASSWORD: -3208,
  QR_EXPIRED: -3501,
  QR_DEVICE_INCONSISTENT: -3503,
  QR_USER_INCONSISTENT: -3504,
  QR_USER_CANCELED: -3505,
  QR_RISKY: -3506,
  INSUFF_GAME_TIME: -110003,
  INSUFF_COIN: -110008,
};

export function describeRetcode(code, message) {
  switch (code) {
    case RETCODE.NEED_LOGIN: return '登录状态失效，请重新扫码';
    case RETCODE.RATE_LIMITED: return '请求过于频繁，请稍后再试';
    case RETCODE.NEED_AIGIS: return '需要在手机上完成人机校验';
    case RETCODE.QR_EXPIRED: return '二维码已过期，请刷新';
    case RETCODE.QR_USER_CANCELED: return '已在手机上取消登录';
    case RETCODE.QR_RISKY: return '登录被风控拦截，请稍后再试';
    case RETCODE.INSUFF_GAME_TIME: return '游戏时长不足';
    case RETCODE.INSUFF_COIN: return '原点不足';
    default: return message || ('服务端返回 ' + code);
  }
}

/* 业务错误码 -> 给 UI 的分支标签。
 *
 * 为什么需要这个：米哈游几乎所有接口都是 **HTTP 200 + body.retcode≠0**。
 * 只看 HTTP 状态码会把"账号密码错"当成功，把"要过极验"当成功 —— 然后
 * UI 就永远转圈。所以每个调用点都必须过一遍 retcode。
 *
 * kind 语义（UI 按它选交互）：
 *   'ok'        成功
 *   'relogin'   登录态失效 -> 回扫码页
 *   'qraz'      二维码过期 -> 换一张码，不用退出登录
 *   'human'     需要人机校验（极验）。**词典笔上做不了**（要跑 geetest v4
 *               的 JS），只能提示用户去手机/网页端完成后回来重试
 *   'ratelimit' 风控限流 -> 退避重试
 *   'fatal'     其它不可恢复错误 -> 显示 message
 */
export function classifyRetcode(code) {
  switch (code) {
    case RETCODE.OK: return 'ok';
    case RETCODE.NEED_LOGIN: return 'relogin';
    case RETCODE.QR_EXPIRED: return 'qraz';
    case RETCODE.NEED_AIGIS: return 'human';
    case RETCODE.RATE_LIMITED: return 'ratelimit';
    default: return 'fatal';
  }
}

/* -3101 的专用提示。极验需要跑 geetest v4 的 Web JS + 拖拽/点选交互，
 * 词典笔没有浏览器内核也没有触摸拖拽，唯一正路是让用户在手机上过。
 * 把这句话写死在这里，避免 UI 各写一版。 */
export const HINT_AIGIS = '该账号触发了米哈游的人机校验，词典笔上无法完成。请先在手机上打开米游社/云原神网页版登录一次，再回到本应用重试。';

/* 命名空间式 default：允许 `import C from './const.js'` 这种写法。
 * QuickJS 的模块加载器不会把命名导出自动合并成 default，
 * 所以显式给一个，避免 "Could not find export 'default'"。 */
export default {
  GAME_BIZ: GAME_BIZ,
  APP_ID: APP_ID,
  CHANNEL_ID: CHANNEL_ID,
  PASSPORT_APP_ID: PASSPORT_APP_ID,
  PASSPORT_BASE: PASSPORT_BASE,
  CLOUD_BASE: CLOUD_BASE,
  SDK_BASE: SDK_BASE,
  STATIC_BASE: STATIC_BASE,
  APP_KEY: APP_KEY,
  COMBO_APP_KEY: COMBO_APP_KEY,
  PROFILE: PROFILE,
  APP_VERSION: APP_VERSION,
  SDK_VERSION: SDK_VERSION,
  COMBO_SDK_VERSION: COMBO_SDK_VERSION,
  MDK_VERSION: MDK_VERSION,
  VENDOR_ID: VENDOR_ID,
  OP_BIZ: OP_BIZ,
  QR_POLL_INTERVAL_MS: QR_POLL_INTERVAL_MS,
  QR_POLL_TIMEOUT_MS: QR_POLL_TIMEOUT_MS,
  HTTP_TIMEOUT_MS: HTTP_TIMEOUT_MS,
  RETCODE: RETCODE,
  HINT_AIGIS: HINT_AIGIS,
  describeRetcode: describeRetcode,
  classifyRetcode: classifyRetcode,
};
