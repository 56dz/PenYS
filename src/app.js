import { BasePage } from './base-page.js';

/* 逻辑画布宽度（真机实测）：
 * 有道 X6 Pro 的 Falcon UI 合成面是横向条带 960x266，物理面板 480x960（rotation=1 竖屏）。
 * setViewPort(W) 把 W 个设计像素铺满合成面宽度，所以 W 必须等于合成面宽度 960，
 * 否则整页会被等比缩放且纵向裁底（PenDouyu 真机踩过）。
 *
 * 坐标系说明（写布局时天天要用）：
 *   设计态 960x266（宽 x 高），转换成面板坐标时
 *   面板 = (y, 960-(x+w), h, w) —— 见 services/screen.js 的 logicalRectToPhysicalRect。
 */
const DESIGN_WIDTH = 960;

class App extends $falcon.App {
  constructor() {
    super();
  }

  onLaunch(options) {
    super.onLaunch(options);
    this.setViewPort(DESIGN_WIDTH);
    $falcon.useDefaultBasePageClass(BasePage);
    // console.log 在 Falcon 里不落盘，只有 warn/error 会进 /userdata/applog，
    // 所以启动打点统一用 console.warn，方便真机用 grep 取证。
    console.warn('[ys] app launch, viewport=' + DESIGN_WIDTH);
  }

  onShow() {
    super.onShow();
    console.warn('[ys] app onShow');
  }

  onHide() {
    super.onHide();
    console.warn('[ys] app onHide');
  }

  onDestroy() {
    super.onDestroy();
    console.warn('[ys] app onDestroy');
  }
}

export default App;
