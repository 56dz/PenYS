/* 统一资源回收的页面基类（从 PenDouyu 复制，已真机验证）。
 *
 * 为什么需要它：Falcon 的 $falcon.on / setTimeout / setInterval 都不会随页面卸载自动清，
 * 云游戏页面里有扫码轮询、播放状态轮询、控制条自动隐藏定时器、屏幕保活定时器——
 * 少清一个就会在退出后继续跑（PenDouyu 真机踩过：两套 #N tick 交错 = 泄漏）。
 * 这里把所有句柄记账，onUnload 的 finally 里统一释放。
 */
class ResourcePage extends $falcon.Page {
  constructor() {
    super();
    this.falconOnTokens = [];
    this.timeoutTokens = new Set();
    this.intervalTokens = new Set();
  }

  on(name, callback) {
    const token = $falcon.on(name, callback);
    this.falconOnTokens.push([token, name]);
    return token;
  }

  off(name, callbackOrToken) {
    $falcon.off(name, callbackOrToken);
    this.falconOnTokens = this.falconOnTokens.filter((item) => item[0] !== callbackOrToken);
  }

  setTimeout(callback, delay) {
    const token = setTimeout(() => {
      this.timeoutTokens.delete(token);
      callback();
    }, delay);
    this.timeoutTokens.add(token);
    return token;
  }

  clearTimeout(token) {
    clearTimeout(token);
    this.timeoutTokens.delete(token);
  }

  setInterval(callback, delay) {
    const token = setInterval(callback, delay);
    this.intervalTokens.add(token);
    return token;
  }

  clearInterval(token) {
    clearInterval(token);
    this.intervalTokens.delete(token);
  }

  sleep(delay) {
    return new Promise((resolve) => this.setTimeout(resolve, delay));
  }

  release() {
    this.falconOnTokens.forEach((item) => $falcon.off(item[1], item[0]));
    this.falconOnTokens = [];
    this.timeoutTokens.forEach((token) => clearTimeout(token));
    this.intervalTokens.forEach((token) => clearInterval(token));
    this.timeoutTokens.clear();
    this.intervalTokens.clear();
  }
}

export class BasePage extends ResourcePage {
  onLoad(options) {
    super.onLoad(options);
    this.options = options || {};
  }

  onNewOptions(options) {
    super.onNewOptions(options);
    this.options = options || {};
  }

  onShow() {
    super.onShow();
    if (this.$root && this.$root.onShow) this.$root.onShow();
  }

  onHide() {
    super.onHide();
    if (this.$root && this.$root.onHide) this.$root.onHide();
  }

  onUnload() {
    try {
      super.onUnload();
      if (this.$root && this.$root.onUnload) this.$root.onUnload();
    } finally {
      this.release();
    }
  }

  beforeVueInstantiate(Vue) {
    if (super.beforeVueInstantiate) super.beforeVueInstantiate(Vue);
    Vue.prototype.$workspace = globalThis.$workspace;
    Vue.prototype.$appid = globalThis.$appid;
  }
}
