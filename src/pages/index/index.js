import IndexComponent from './index.vue';
import { BasePage } from '../../base-page.js';

/* 单页多状态：整个 app 只有 index 一个页面，boot/login/home/dispatching/playing/error
 * 六个状态都是同一个 Vue 根组件内的 mode 分支（照抄 PenDouyu 的做法）。
 *
 * 为什么不做多页：Falcon 的页面栈切换会重跑 onLoad，而云游戏的「扫码中 / 排队中 /
 * 播放中」三态里有长连接与 native 播放实例，跨页切换必然要重建；单页状态机把
 * 这些资源的生命周期收敛到一个 Vue 组件的 onShow/onHide/onUnload 里，好收拾。
 */
class PageIndex extends BasePage {
  constructor() {
    super();
  }

  onLoad(options) {
    super.onLoad(options);
    console.warn('[ys] page index onLoad');
    this.setRootComponent(IndexComponent);
  }
}

export default PageIndex;
