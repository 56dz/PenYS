<template>
  <div :class="rootClass">

    <!-- ==================== boot：启动页 ====================
         266 = 顶部金线 1 + 主体 243 + 页脚 22。
         为什么主体不占满：底部页脚要固定显示版本与数据源（mock/真实协议层），
         排查问题时一眼能看到当前跑的是哪套后端。 -->
    <template v-if="mode === 'boot'">
      <div class="deco-topline"></div>
      <div class="boot-wrap">
        <div class="boot-gem-row">
          <div class="boot-gem"></div>
          <div class="boot-gem boot-gem-2"></div>
          <div class="boot-gem boot-gem-3"></div>
          <div class="boot-gem boot-gem-4"></div>
          <div class="boot-gem boot-gem-5"></div>
        </div>
        <text class="boot-title">云·原神</text>
        <text class="boot-sub">有道词典笔 X6 Pro 定制版</text>
        <div class="vgap-10"></div>
        <text class="boot-hint">{{ bootHint }}</text>
      </div>
      <div class="boot-foot">
        <text class="boot-ver">v{{ ver }}</text>
        <div class="spacer"></div>
        <text class="boot-ver">{{ sourceText }}</text>
      </div>
    </template>

    <!-- ==================== login：扫码登录 ====================
         左侧 236x236 二维码（纯 div 拼格子），右侧文案与状态。
         没有键盘输入法可用，所以这是唯一的登录方式。 -->
    <template v-if="mode === 'login'">
      <div class="strip">
        <div :class="sourceDotCls"></div>
        <div class="gap-6"></div>
        <text class="strip-text">{{ sourceText }}</text>
        <div class="strip-gap"></div>
        <text class="strip-text">扫码登录 · 全程无需键盘</text>
      </div>
      <div class="login-body">
        <div class="login-left">
          <div v-if="qrRows.length" class="qr-box">
            <div v-for="(row, ri) in qrRows" :key="'r' + ri" :class="qrRowCls">
              <div v-for="(seg, si) in row" :key="'s' + si" :class="seg.dark ? qrSegCls + ' qr-dark ' + seg.cls : qrSegCls + ' ' + seg.cls"></div>
            </div>
          </div>
          <div v-else class="qr-box">
            <text class="qr-tip">{{ qrErr || '正在生成二维码…' }}</text>
          </div>
        </div>
        <div class="login-right">
          <text class="login-title">云·原神</text>
          <text class="login-sub">请使用米游社 App 扫描左侧二维码</text>
          <div class="login-state-row">
            <div :class="stateDotCls"></div>
            <div class="login-mark-gap"></div>
            <text class="login-state">{{ loginStateText }}</text>
          </div>
          <div class="login-flow">
            <text :class="flowCls(0)">① 扫一扫</text>
            <div class="login-flow-arrow"></div>
            <text :class="flowCls(1)">② 手机确认</text>
            <div class="login-flow-arrow"></div>
            <text :class="flowCls(2)">③ 自动进入</text>
          </div>
          <div class="login-actions">
            <div class="btn btn-primary" @click="startLogin">
              <text class="btn-text btn-on-text">{{ loginState === 'Expired' ? '刷新二维码' : '重新获取' }}</text>
            </div>
            <div class="login-btn-gap"></div>
            <div class="btn" @click="doLogout">
              <text class="btn-text">清除登录</text>
            </div>
          </div>
          <text v-if="qrErr" class="login-err">{{ qrErr }}</text>
        </div>
      </div>
    </template>

    <!-- ==================== home：主页 ====================
         三段：左 264 账号/时长，中弹性区域放大 CTA，右 264 画质档位。
         为什么 CTA 放中间：960x266 是超宽条，视线中心在水平中线，
         主按钮必须落在中线上；左右两栏只放"看一眼就够"的信息。 -->
    <template v-if="mode === 'home'">
      <div class="strip">
        <div :class="sourceDotCls"></div>
        <div class="gap-6"></div>
        <text class="strip-text">{{ sourceText }}</text>
        <div class="strip-gap"></div>
        <text class="strip-text">{{ statText }}</text>
        <div class="gap-10"></div>
        <div :class="netDotCls"></div>
        <div class="gap-6"></div>
        <text class="strip-text">{{ latencyText }}</text>
      </div>

      <div v-if="!overlay" class="hm-body">
        <div class="hm-left">
          <div class="hm-acc-row">
            <div class="hm-avatar">
              <text class="hm-avatar-txt">{{ avatarChar }}</text>
            </div>
            <div class="hm-acc-txt">
              <text class="hm-nick">{{ nickname || '未登录' }}</text>
              <text class="hm-uid">{{ uidText }}</text>
            </div>
          </div>
          <div class="hm-vgap"></div>
          <div class="hm-time-card">
            <text class="hm-time-label">剩余可玩时长</text>
            <div class="hm-time-row">
              <text :class="walletBad ? 'hm-time-val hm-time-bad' : 'hm-time-val'">{{ walletText }}</text>
              <div class="gap-4"></div>
              <text class="hm-time-unit">{{ walletUnit }}</text>
            </div>
          </div>
          <div class="hm-vgap"></div>
          <div class="hm-link" @click="overlay = 'settings'">
            <text class="hm-link-txt">设置</text>
          </div>
        </div>

        <div class="hm-mid">
          <text class="hm-gear-name">画质 · {{ gearName }}</text>
          <div :class="ctaOn ? 'hm-cta hm-cta-on' : 'hm-cta'" @touchstart="ctaOn = true" @touchend="ctaOn = false" @click="askDispatch">
            <text class="hm-cta-title">开始游戏</text>
            <text class="hm-cta-sub">申请云端实例 · 按分钟计费</text>
          </div>
          <text class="hm-hint">{{ hintText }}</text>
        </div>

        <div class="hm-right">
          <text class="hm-sec-title">画质档位</text>
          <div v-for="(g, gi) in gears" :key="'g' + gi" :class="gi === gearIndex ? 'hm-gear hm-gear-on' : 'hm-gear'" @click="pickGear(gi)">
            <text :class="gi === gearIndex ? 'hm-gear-name-txt hm-gear-name-on' : 'hm-gear-name-txt'">{{ g.name }}</text>
            <div class="hm-gear-spring"></div>
            <text class="hm-gear-bit">{{ g.bit }}</text>
          </div>
          <div class="hm-gear-spring"></div>
          <div class="hm-link" @click="loadHome">
            <text class="hm-link-txt">刷新时长</text>
          </div>
        </div>
      </div>

      <!-- 弹层：不用 position:absolute（Falcon 上不可靠），
           直接在同一位置替换掉 242 高的 body —— 视觉上等同于覆盖层。 -->
      <div v-else class="cf-mask">
        <div class="cf-card">
          <template v-if="overlay === 'confirm'">
            <text class="cf-title">开始云游戏</text>
            <text class="cf-line">即将向云端申请游戏实例。申请成功后会立刻开始计费，并按实际游玩分钟数扣除时长。</text>
            <text class="cf-warn">当前剩余：{{ walletFullText }}</text>
            <div class="cf-btns">
              <div class="cf-btn" @click="overlay = ''">
                <text class="cf-btn-txt">再想想</text>
              </div>
              <div class="cf-btn cf-btn-go" @click="doDispatch">
                <text class="cf-btn-txt cf-btn-txt-go">确认开始</text>
              </div>
            </div>
          </template>
          <template v-else>
            <text class="cf-title">设置</text>
            <text class="cf-line">画质档位：{{ gearName }}　·　免费时长：{{ freeText }}　·　畅玩卡：{{ cardText }}</text>
            <text class="cf-warn">当前数据源：{{ sourceText }}</text>
            <div class="cf-btns">
              <div class="cf-btn cf-btn-s" @click="overlay = ''">
                <text class="cf-btn-txt">关闭</text>
              </div>
              <div class="cf-btn cf-btn-s" @click="loadHome">
                <text class="cf-btn-txt">刷新</text>
              </div>
              <div class="cf-btn cf-btn-s" @click="doLogout">
                <text class="cf-btn-txt">退出登录</text>
              </div>
            </div>
          </template>
        </div>
      </div>
    </template>

    <!-- ==================== dispatching：调度中 ====================
         四步步骤条：登录 → 测速 → 分配节点 → 启动实例。
         步骤推进用本地节奏（真实相位由 dispatch 的 Promise 决定），
         所以队列人数是"加分信息"，不承担正确性。 -->
    <template v-if="mode === 'dispatching'">
      <div class="strip">
        <div class="dot dot-warn"></div>
        <div class="gap-6"></div>
        <text class="strip-text">正在申请云端实例 · 请勿熄屏</text>
        <div class="strip-gap"></div>
        <text class="strip-text">{{ queueText }}</text>
      </div>
      <div class="dp-wrap">
        <text class="dp-title">正在进入云端</text>
        <text class="dp-node">{{ nodeText }}</text>
        <text class="dp-queue">{{ queueText }}</text>
        <div class="dp-steps">
          <div v-for="(it, ii) in dispSteps" :key="'p' + ii" :class="it.kind === 's' ? '' : ''">
            <div v-if="it.kind === 's'" class="dp-step">
              <div :class="it.si < dispStep ? 'dp-step-dot dp-step-dot-done' : (it.si === dispStep ? 'dp-step-dot dp-step-dot-on' : 'dp-step-dot')"></div>
              <text :class="it.si <= dispStep ? 'dp-step-txt dp-step-txt-on' : 'dp-step-txt'">{{ it.label }}</text>
            </div>
            <div v-else :class="it.si < dispStep ? 'dp-line dp-line-on' : 'dp-line'"></div>
          </div>
        </div>
        <div class="dp-actions">
          <div class="dp-cancel" @click="cancelDispatch">
            <text class="dp-cancel-txt">取消排队</text>
          </div>
        </div>
      </div>
    </template>

    <!-- ==================== playing：游戏中 ====================
         KMS 视频平面会盖在整块 960x266 上（native 侧渲染），
         所以根节点用 .page-play（完全透明），UI 只画"不透明的那几块"。
         控制条占底部 40px、一键隐藏后换成 16px 唤起条 ——
         用 flex 列排布而不是绝对定位，所以控制条永远不会压到画面中央。 -->
    <template v-if="mode === 'playing'">
      <div :class="railVisible ? 'pl-video' : 'pl-video-mid'">
        <div :class="railVisible ? 'pl-touch' : 'pl-touch-mid'" @touchstart="onTouchStart" @touchmove="onTouchMove" @touchend="onTouchEnd">
          <div v-if="!videoReady" class="pl-ph-fill">
            <text class="pl-ph-title">云·原神</text>
            <div class="pl-ph-gap"></div>
            <text class="pl-ph-sub">{{ phSub }}</text>
          </div>
        </div>
      </div>
      <div v-if="railVisible" class="pl-rail">
        <div class="pl-btn" @click="leaveGame('返回')">
          <text class="pl-btn-txt">返回</text>
        </div>
        <div class="pl-btn" @click="togglePause">
          <text :class="paused ? 'pl-btn-txt pl-btn-txt-on' : 'pl-btn-txt'">{{ paused ? '继续' : '暂停' }}</text>
        </div>
        <div class="pl-btn" @click="cycleGear">
          <text class="pl-btn-txt">{{ gearBtnText }}</text>
        </div>
        <div class="pl-btn" @click="toggleMute">
          <text :class="muted ? 'pl-btn-txt pl-btn-txt-on' : 'pl-btn-txt'">{{ muted ? '已静音' : '音量' }}</text>
        </div>
        <div class="pl-btn pl-btn-danger" @click="leaveGame('断开')">
          <text class="pl-btn-txt pl-btn-txt-danger">断开</text>
        </div>
        <div class="pl-spring"></div>
        <text :class="statOk ? 'pl-stat pl-stat-ok' : 'pl-stat'">{{ statText }}</text>
        <div class="pl-btn" @click="railVisible = false">
          <text class="pl-btn-txt">隐藏</text>
        </div>
      </div>
      <div v-else class="pl-handle" @click="railVisible = true">
        <text class="pl-handle-txt">▲ 点击显示控制条</text>
      </div>
    </template>

    <!-- ==================== error：错误页 ====================
         266 = 顶部金线 1 + 主体 265。错误码本身放在最大字号上，
         因为用户打电话报障时说的就是"错误码多少"。 -->
    <template v-if="mode === 'error'">
      <div class="deco-topline"></div>
      <div class="er-wrap">
        <text :class="errUnknown ? 'er-code er-code-unknown' : 'er-code'">{{ errCodeText }}</text>
        <text class="er-msg">{{ errMsg }}</text>
        <text class="er-detail">{{ errDetail }}</text>
        <div class="er-btns">
          <div class="btn btn-primary" @click="retry">
            <text class="btn-text btn-on-text">重试</text>
          </div>
          <div class="btn-gap"></div>
          <div class="btn" @click="backHome">
            <text class="btn-text">返回主页</text>
          </div>
        </div>
      </div>
    </template>

  </div>
</template>

<script>
/* 云·原神（词典笔版）单页状态机。
 *
 * 状态：boot → login → home → dispatching → playing，任何一步出错进 error。
 * 为什么坚持单页而不是多页：Falcon 的页面栈会重复触发 onLoad/onShow，
 * 播放态切页最容易把 KMS 平面留在屏幕上（本设备最脏的故障）；单页只有一个
 * 生命周期，teardown 只需要在一处写。
 *
 * 依赖边界（都由别人交付，这里只调用，不实现）：
 *   services/player.js   native 播放层（lead 写，接口已冻结）
 *   services/ys_client.js 协议层适配器（真实现 services/ys/index.js 不存在时自动回退 mock）
 *   services/screen.js   屏幕保活（从斗鱼移植）
 *   services/qrcode.js   纯 JS 二维码编码器（从 B 站移植）
 */
import * as player from '../../services/player.js';
import * as screen from '../../services/screen.js';
import * as ys from '../../services/ys_client.js';
import { encodeQr } from '../../services/qrcode.js';

/* 统一日志出口。Falcon 只有 console.warn/error 会落盘到 /userdata/applog/，
 * console.log 不落盘 —— 所以排查用的日志一律走 warn。 */
function logWarn(msg) {
  try {
    console.warn('[ys] ' + msg);
  } catch (e) {
    /* 日志本身不能带崩流程 */
  }
}

/* 视频区矩形（逻辑坐标 960x266）。取满画布：
 * KMS 平面铺满整块合成面，视觉上才是"游戏占满屏幕"。
 * 底部 40px 会被控制条压住，但控制条可一键隐藏 —— 这是宽高比 3.6:1 的
 * 口袋里最合理的取舍（若把视频缩到 226 高，隐藏控制条后底部会留一条黑边）。 */
const VIDEO_RECT = { x: 0, y: 0, w: 960, h: 266 };

/* 米哈游错误码 → 人话。词典笔做不了极验，所以 -3101 必须明确告诉用户"去手机上完成"，
 * 否则用户会对着笔干等。 */
const ERR_TEXT = {
  '-100': '登录已失效，请重新扫码',
  '-3001': '参数不合法',
  '-3006': '操作过于频繁，请稍后再试',
  '-3101': '请在手机上完成安全验证',
  '-3208': '账号或密码错误',
  '-3501': '二维码已过期，请点刷新',
  '-502': '登录凭证无效'
};

/* 二维码：4 格 quiet + 4px 一格。236 容器内若装不下就整体降到 3px 一格
 * （版 9/10 的 61/65 格在 4px 下会超出 236 被裁掉，裁掉就扫不出来）。 */
const QR_QUIET = 4;
const QR_BOX_INNER = 232; /* 236 - 2*1px 金边，留 2px 余量 */

/* grid → 行/段。每段只带一个静态 class（.qr-l-N / .qr-s-N），零 inline style。
 * 这是 PenBili 验证过的做法：Falcon 不支持 inline style，也不可靠地支持运行时几何。 */
function gridToRows(grid, size) {
  const cells = size + QR_QUIET * 2;
  const cell = cells * 4 <= QR_BOX_INNER ? 4 : 3;
  const prefix = cell === 4 ? 'qr-l-' : 'qr-s-';
  const rowCls = cell === 4 ? 'qr-row' : 'qr-row-s';
  const segCls = cell === 4 ? 'qr-seg' : 'qr-seg-s';
  const rows = [];
  const white = [{ dark: false, cls: prefix + cells }];

  function push(segs, dark, len) {
    if (len <= 0) return;
    const last = segs.length ? segs[segs.length - 1] : null;
    if (last && last.dark === dark) {
      last.n += len;
      last.cls = prefix + last.n;
      return;
    }
    segs.push({ dark: dark, n: len, cls: prefix + len });
  }

  for (let i = 0; i < QR_QUIET; i++) rows.push(white);
  for (let y = 0; y < size; y++) {
    const segs = [];
    push(segs, false, QR_QUIET);
    let run = !!(grid[y] && grid[y][0]);
    let len = 0;
    for (let x = 0; x < size; x++) {
      const d = !!(grid[y] && grid[y][x]);
      if (d === run) {
        len++;
      } else {
        push(segs, run, len);
        run = d;
        len = 1;
      }
    }
    push(segs, run, len);
    push(segs, false, QR_QUIET);
    rows.push(segs);
  }
  for (let i = 0; i < QR_QUIET; i++) rows.push(white);
  return { rows: rows, rowCls: rowCls, segCls: segCls };
}

/* 分钟 → "X小时Y分钟"。0 分钟单独提示"时长不足"，因为 0 和 "--" 是两种状态。 */
function fmtMinutes(t) {
  const n = typeof t === 'number' && isFinite(t) ? Math.max(0, Math.round(t)) : 0;
  const h = Math.floor(n / 60);
  const m = n % 60;
  if (n <= 0) return '0 分钟';
  if (h > 0) return m > 0 ? h + ' 小时 ' + m + ' 分钟' : h + ' 小时';
  return m + ' 分钟';
}

function num(v) {
  const n = Number(v);
  return isFinite(n) ? n : 0;
}

export default {
  data() {
    return {
      mode: 'boot',

      /* boot */
      bootHint: '正在初始化…',
      ver: '1.0.0',

      /* login */
      qrRows: [],
      qrRowCls: 'qr-row',
      qrSegCls: 'qr-seg',
      qrErr: '',
      loginState: 'Created',
      loginStateText: '正在获取二维码…',
      ticket: '',

      /* 账号与钱包 */
      nickname: '',
      uid: '',
      walletText: '--',
      walletUnit: '',
      walletBad: false,
      walletFullText: '--',
      freeText: '--',
      cardText: '--',

      /* 画质 */
      gears: [],
      gearIndex: 0,
      gearName: '标清',

      /* home 弹层：'' | 'confirm' | 'settings' */
      overlay: '',
      ctaOn: false,

      /* dispatching */
      dispStep: 0,
      dispSteps: buildDispSteps(),
      queueText: '',
      nodeText: '正在连接云端节点…',

      /* playing */
      videoReady: false,
      railVisible: true,
      paused: false,
      muted: false,
      phSub: '正在准备画面…',
      statText: '等待画面',
      statOk: false,
      latencyText: '延迟 --',

      /* 协议层 / native 状态 */
      nativeOk: false,
      protoReady: false,

      /* error */
      errCode: '',
      errMsg: '',
      errDetail: '',
      errUnknown: false,
      retryKind: 'login'
    };
  },

  computed: {
    /* 播放态根节点必须完全透明，否则不透明的合成面会盖住 KMS 视频平面 */
    rootClass() {
      return this.mode === 'playing' ? 'page page-play' : 'page';
    },
    /* 数据源标签说明的是「native 传输层是否可用」，不是「用的是不是 mock」。
     * 协议层现在是编译期静态 import 进来的（见 services/ys_client.js 顶部注释），
     * 所以 isMock() 恒 false，拿它做判断等于永远显示"协议层"，没有信息量。
     * 真正会变的是 native 模块 yshttp：没装载时 HTTP 打不出去，页面只能靠
     * 单次调用级的 mock 兜底。用 data 里的 nativeOk 而不是直接调函数，
     * 是因为 computed 会被缓存，而 native 探测是懒加载、会随时间变化。 */
    sourceText() {
      return this.nativeOk ? '数据源：米哈游协议层' : '数据源：协议层（native 未就绪）';
    },
    sourceDotCls() {
      return this.nativeOk ? 'dot' : 'dot dot-warn';
    },
    netDotCls() {
      if (this.mode === 'playing') return this.statOk ? 'dot' : 'dot-warn';
      return 'dot';
    },
    stateDotCls() {
      if (this.loginState === 'Scanned') return 'dot dot-warn';
      if (this.loginState === 'Expired') return 'dot dot-bad';
      if (this.loginState === 'Confirmed') return 'dot';
      return 'dot dot-warn';
    },
    avatarChar() {
      const n = this.nickname || '';
      return n ? n.substring(0, 1) : '旅';
    },
    uidText() {
      return this.uid ? 'UID ' + this.uid : '尚未绑定米游社账号';
    },
    gearBtnText() {
      return this.gearName || '画质';
    },
    hintText() {
      if (!this.gears.length) return '正在获取画质档位…';
      return '档位越高越清晰，也越吃带宽与时长';
    },
    errCodeText() {
      return this.errUnknown ? '错误码: ' + this.errCode : this.errCode;
    }
  },

  created() {
    /* 记账容器。Falcon 上最容易出的 bug 就是定时器泄漏 ——
     * 页面销毁后回调还在跑，直接读写已释放的 native 句柄。 */
    this._timers = new Set();
    this._intervals = new Set();
    this._dead = false;
    this._gen = 0;
    this._keepOn = false;
    this._pollT = 0;
    this._statT = 0;
    this._bgPaused = false;

    /* 协议层是编译期静态 import 的（见 ys_client.js），这里只探测两件运行期事实：
     * ① protoReady —— 协议层函数齐不齐（缺函数说明打包被 tree-shake 了）
     * ② nativeOk  —— native 传输层 yshttp 是否装载成功（决定能不能真的发 HTTP）
     * 用 console.warn 而不是 log：只有 warn/error 会落盘到 /userdata/applog。 */
    this.protoReady = ys.isReady();
    this.nativeOk = ys.nativeUsable();
    logWarn(
      '页面创建 protoReady=' + this.protoReady +
      ' nativeOk=' + this.nativeOk + ' player可用=' + player.isAvailable() +
      (this.nativeOk ? '' : ' 原因=' + (ys.nativeReason() || '未知'))
    );
    this.bootTick();
  },

  methods: {
    /* ---------- 计时器记账（照抄斗鱼的做法，见其 teardown） ----------
     * 用 Set 记账而不是数组：定时器回调里要 delete 自己，Set.delete 是 O(1)，
     * 数组会越删越慢且可能删错下标。 */
    later(fn, ms) {
      const self = this;
      const t = setTimeout(function () {
        self._timers.delete(t);
        try {
          fn();
        } catch (e) {
          logWarn('定时回调异常: ' + ((e && e.message) || e));
        }
      }, ms);
      this._timers.add(t);
      return t;
    },
    every(fn, ms) {
      const self = this;
      const t = setInterval(function () {
        if (self._dead) return;
        try {
          fn();
        } catch (e) {
          logWarn('周期回调异常: ' + ((e && e.message) || e));
        }
      }, ms);
      this._intervals.add(t);
      return t;
    },
    sleep(ms) {
      return new Promise((resolve) => {
        this.later(resolve, ms);
      });
    },

    /* ---------- 屏幕保活 ----------
     * 设备 /etc/input-event-daemon.conf 的 [Idle]：270s 调暗、300s 熄屏。
     * hal-screen bright_time 改不动这个硬编码计时器，所以必须靠 20s 一次的续屏
     * （screen.keepScreenOn() 内部就是 20s 的 setTimeout 链，失败时 1s 重试）。
     * 挂载点：login / home / dispatching / playing 四个态；
     * 离开这四个态、onHide、onUnload 都必须停 —— 否则后台还在续屏，等于永不熄屏。 */
    startKeepScreen() {
      if (this._keepOn) return;
      try {
        this._keepOn = !!screen.keepScreenOn();
        logWarn('屏幕保活启动 mode=' + this.mode + ' ticks=' + screen.keepScreenTicks());
      } catch (e) {
        logWarn('屏幕保活启动失败: ' + ((e && e.message) || e));
        this._keepOn = false;
      }
    },
    stopKeepScreen(where) {
      let wasOn = this._keepOn;
      let ticks = 0;
      try {
        wasOn = wasOn || screen.isKeepingScreen();
        ticks = screen.keepScreenTicks();
        screen.keepScreenOff();
      } catch (e) {
        logWarn('屏幕保活停止异常: ' + ((e && e.message) || e));
      }
      this._keepOn = false;
      /* 这行日志带 ticks：两次进出播放态后应该看到重新从 0 计数，
       * 如果看到两套编号交错，就是定时器泄漏。 */
      if (wasOn) logWarn('屏幕保活停止（' + (where || '') + '）ticks=' + ticks);
    },
    /* 四个"需要用户在场"的态统一开保活，其余态统一关 */
    syncKeep() {
      if (this.mode === 'login' || this.mode === 'home' || this.mode === 'dispatching' || this.mode === 'playing') {
        this.startKeepScreen();
      } else {
        this.stopKeepScreen('mode=' + this.mode);
      }
    },

    /* ---------- boot ---------- */
    async bootTick() {
      const myGen = this._gen;
      this.bootHint = '正在连接云端…';
      await this.sleep(420);
      if (this._dead || myGen !== this._gen) return;
      this.bootHint = '正在检查登录状态…';
      await this.sleep(360);
      if (this._dead || myGen !== this._gen) return;
      let logged = false;
      try {
        logged = !!ys.isLoggedIn();
      } catch (e) {
        logged = false;
      }
      if (logged) {
        this.gotoHome();
      } else {
        this.gotoLogin();
      }
    },

    /* ---------- login ---------- */
    gotoLogin() {
      this.mode = 'login';
      this.syncKeep();
      this.startLogin();
    },
    /* 每次换票都 gen++：上一轮的轮询回调看到 gen 变了会自己作废，
     * 这样"连点刷新"不会出现两个轮询同时改 UI。 */
    async startLogin() {
      this._gen++;
      const myGen = this._gen;
      this.stopPoll();
      this.loginState = 'Created';
      this.loginStateText = '正在获取二维码…';
      this.qrErr = '';
      this.qrRows = [];
      try {
        const r = await ys.loginBeginQr();
        if (this._dead || myGen !== this._gen) return;
        this.ticket = (r && r.ticket) || '';
        this.renderQr((r && r.url) || '');
        this.loginStateText = '等待扫码';
        logWarn('二维码已生成 ticket=' + (this.ticket ? '有' : '空') + ' url长度=' + ((r && r.url && r.url.length) || 0));
        this.startPoll();
      } catch (e) {
        if (this._dead || myGen !== this._gen) return;
        this.loginStateText = '获取二维码失败';
        this.qrErr = '网络异常：' + ((e && e.message) || '未知错误');
        logWarn('loginBeginQr 失败: ' + ((e && e.message) || e));
      }
    },
    /* 登录三步流程条的着色：已到达的步骤亮金色。
     * 做成方法而不是计算属性，是因为模板里要按索引分别判断（:class 里不能写内联逻辑）。 */
    flowCls(step) {
      const reached = this.loginState === 'Confirmed' ? 3 : this.loginState === 'Scanned' ? 2 : 1;
      return step < reached ? 'login-flow-step login-flow-step-on' : 'login-flow-step';
    },
    renderQr(url) {
      if (!url) {
        this.qrErr = '服务端没有返回二维码内容';
        return;
      }
      let r = null;
      try {
        r = encodeQr(url);
      } catch (e) {
        r = null;
      }
      if (!r || !r.ok) {
        this.qrErr = '二维码生成失败：' + ((r && r.reason) || '编码异常');
        return;
      }
      const built = gridToRows(r.grid, r.size);
      this.qrRows = built.rows;
      this.qrRowCls = built.rowCls;
      this.qrSegCls = built.segCls;
      this.qrErr = '';
      logWarn('二维码渲染完成 size=' + r.size + ' version=' + r.version + ' 行数=' + built.rows.length);
    },
    startPoll() {
      this.stopPoll();
      const myGen = this._gen;
      /* 2s 一次：米游社扫码态变化通常在 1 秒内可见，
       * 再快没必要（还得考虑设备是 4xA35，且轮询走的是网络） */
      this._pollT = this.every(() => {
        this.pollOnce(myGen);
      }, 2000);
    },
    stopPoll() {
      if (this._pollT) {
        try {
          clearInterval(this._pollT);
        } catch (e) {}
        this._intervals.delete(this._pollT);
        this._pollT = 0;
      }
    },
    async pollOnce(myGen) {
      if (this._dead || myGen !== this._gen || !this.ticket) return;
      let r = null;
      try {
        r = await ys.loginPollStatus(this.ticket);
      } catch (e) {
        if (this._dead || myGen !== this._gen) return;
        logWarn('loginPollStatus 异常: ' + ((e && e.message) || e));
        return;
      }
      if (this._dead || myGen !== this._gen) return;
      const st = (r && r.status) || 'Created';
      if (st === 'Scanned') {
        if (this.loginState !== 'Scanned') logWarn('二维码已扫描，等待手机确认');
        this.loginState = 'Scanned';
        this.loginStateText = '已扫码，请在手机上确认';
        return;
      }
      if (st === 'Expired') {
        this.loginState = 'Expired';
        this.loginStateText = '二维码已过期，请点刷新';
        this.stopPoll();
        logWarn('二维码已过期');
        return;
      }
      if (st === 'Confirmed') {
        this.loginState = 'Confirmed';
        this.loginStateText = '登录成功，正在进入…';
        this.stopPoll();
        await this.finishLogin(myGen);
      }
    },
    async finishLogin(myGen) {
      let r = null;
      try {
        r = await ys.loginComplete(this.ticket);
      } catch (e) {
        if (this._dead || myGen !== this._gen) return;
        this.showError(-502, '登录凭证无效：' + ((e && e.message) || '换票失败'), 'login');
        return;
      }
      if (this._dead || myGen !== this._gen) return;
      if (!r || r.ok === false) {
        this.showError(-502, '登录凭证无效，请重新扫码', 'login');
        return;
      }
      this.nickname = r.nickname || '';
      this.uid = r.uid ? String(r.uid) : '';
      logWarn('登录完成 nickname=' + (this.nickname ? '有' : '空') + ' uid=' + (this.uid ? '有' : '空'));
      this.gotoHome();
    },

    /* ---------- home ---------- */
    async gotoHome() {
      this.mode = 'home';
      this.overlay = '';
      this.syncKeep();
      await this.loadHome();
    },
    async loadHome() {
      const myGen = this._gen;
      /* 钱包与档位互不依赖，但分开 try：任一失败不应让另一块空白 */
      try {
        const w = await ys.fetchWallet();
        if (this._dead || myGen !== this._gen) return;
        this.applyWallet(w);
      } catch (e) {
        if (this._dead || myGen !== this._gen) return;
        this.walletText = '--';
        this.walletUnit = '获取失败';
        this.walletFullText = '获取失败';
        logWarn('fetchWallet 失败: ' + ((e && e.message) || e));
      }
      try {
        const g = await ys.fetchGears();
        if (this._dead || myGen !== this._gen) return;
        const list = (g || []).slice(0, 5).map((it) => {
          const br = num(it && it.bitRate);
          return {
            id: it && it.id != null ? it.id : 0,
            name: (it && it.name) || '未知',
            bit: br > 0 ? Math.round(br / 1000) + 'M' : '--'
          };
        });
        if (list.length) {
          this.gears = list;
          if (this.gearIndex >= list.length) this.gearIndex = 0;
          this.gearName = list[this.gearIndex].name;
        }
        logWarn('画质档位 ' + this.gears.length + ' 档，当前=' + this.gearName);
      } catch (e) {
        if (this._dead || myGen !== this._gen) return;
        logWarn('fetchGears 失败: ' + ((e && e.message) || e));
      }
    },
    applyWallet(w) {
      const t = num(w && w.totalTime);
      this.walletBad = t <= 0;
      const h = Math.floor(t / 60);
      const m = Math.round(t % 60);
      if (t <= 0) {
        this.walletText = '0';
        this.walletUnit = '分钟 · 时长不足';
      } else if (h > 0) {
        this.walletText = String(h);
        this.walletUnit = '小时 ' + m + ' 分钟';
      } else {
        this.walletText = String(m);
        this.walletUnit = '分钟';
      }
      this.walletFullText = fmtMinutes(t);
      this.freeText = fmtMinutes(num(w && w.freeTime));
      const card = num(w && w.playCard);
      this.cardText = card > 0 ? '有 ' + card + ' 张' : '无';
      logWarn('钱包 totalTime=' + t + 'min → ' + this.walletFullText);
    },
    pickGear(gi) {
      if (gi < 0 || gi >= this.gears.length) return;
      this.gearIndex = gi;
      this.gearName = this.gears[gi].name;
      logWarn('选择画质档位=' + this.gearName);
    },

    /* ---------- 计费确认 → 调度 ----------
     * paasDispatch 一旦成功就开始扣时长，所以 UI 上必须有二次确认：
     * 用户在这个界面点两下才会真的产生费用。 */
    askDispatch() {
      if (this.walletBad) {
        this.showError(-3001, '剩余时长不足，无法开始游戏', 'home');
        return;
      }
      this.overlay = 'confirm';
    },
    async doDispatch() {
      this.overlay = '';
      this.mode = 'dispatching';
      this.syncKeep();
      this._gen++;
      const myGen = this._gen;
      this.dispStep = 0;
      this.nodeText = '正在连接云端节点…';
      this.queueText = '';

      /* 本地节奏推进步骤条：真实相位由 dispatch() 的 Promise 决定，
       * 这个定时器只负责让用户看到"在动"，所以它不参与正确性判断。 */
      const advance = () => {
        if (this._dead || myGen !== this._gen) return;
        if (this.dispStep < 3) {
          this.dispStep++;
          this.later(advance, 900);
        }
      };
      this.later(advance, 900);

      /* 队列信息是同步快照（mock 直接返回，真实现返回真实排队） */
      try {
        const q = ys.queueSnapshot();
        if (q) {
          this.nodeText = q.node || '云端节点';
          const total = num(q.total);
          const pos = num(q.position);
          this.queueText = total > 0 ? '前方排队 ' + pos + ' 人 / 共 ' + total + ' 人' : '当前无需排队';
          logWarn('队列快照 node=' + this.nodeText + ' ' + this.queueText);
        }
      } catch (e) {
        logWarn('queueSnapshot 失败: ' + ((e && e.message) || e));
      }

      let r = null;
      try {
        r = await ys.dispatch({ gearIndex: this.gearIndex });
      } catch (e) {
        if (this._dead || myGen !== this._gen) return;
        this.showError(-3001, '申请实例失败：' + ((e && e.message) || '未知错误'), 'dispatch');
        return;
      }
      if (this._dead || myGen !== this._gen) return;
      const sdkParam = r && r.sdkParam;
      if (!sdkParam) {
        this.showError(-3001, '调度返回缺少 sdkParam，无法播放', 'dispatch');
        return;
      }
      this.dispStep = 3;
      logWarn('调度成功，sdkParam 长度=' + String(sdkParam).length);

      this.mode = 'playing';
      this.videoReady = false;
      this.railVisible = true;
      this.paused = false;
      this.phSub = '正在连接视频流…';
      this.statText = '连接中';
      this.syncKeep();
      this.startStatTimer();

      let res = null;
      try {
        res = await player.start({ sdkParam: sdkParam, rect: VIDEO_RECT });
      } catch (e) {
        res = { ok: false, code: 'THROW', error: (e && e.message) || String(e) };
      }
      if (this._dead || myGen !== this._gen) return;
      if (res && res.ok) {
        this.videoReady = true;
        this.statOk = true;
        logWarn('画面已上屏');
      } else {
        /* 不弹错误页：native 模块缺失时用户仍应看到完整 UI 并可以返回/重试，
         * 否则调试阶段只能看到一片 error。把原因放在占位框里。 */
        this.phSub = '画面未上屏：' + ((res && (res.error || res.code)) || '未知原因');
        this.statText = '未上屏';
        logWarn('player.start 失败 code=' + ((res && res.code) || '?') + ' err=' + ((res && res.error) || ''));
      }
    },
    async cancelDispatch() {
      this._gen++;
      try {
        await ys.leaveQueue();
      } catch (e) {
        logWarn('leaveQueue 失败: ' + ((e && e.message) || e));
      }
      logWarn('用户取消调度');
      this.mode = 'home';
      this.syncKeep();
      this.loadHome();
    },

    /* ---------- playing ---------- */
    startStatTimer() {
      if (this._statT) return;
      this._statT = this.every(() => {
        this.pollStatus();
      }, 2000);
    },
    stopStatTimer() {
      if (!this._statT) return;
      try {
        clearInterval(this._statT);
      } catch (e) {}
      this._intervals.delete(this._statT);
      this._statT = 0;
    },
    pollStatus() {
      if (this.mode !== 'playing' || this._dead) return;
      let s = null;
      try {
        s = player.status();
      } catch (e) {
        return;
      }
      if (!s) return;
      const fps = num(s.fps);
      const lat = num(s.latencyMs);
      const br = num(s.bitrateKbps);
      this.statText = Math.round(fps) + 'fps · ' + Math.round(br / 1000) + 'Mbps';
      this.latencyText = lat > 0 ? '延迟 ' + Math.round(lat) + 'ms' : '延迟 --';
      this.statOk = s.state === 'playing';
      /* 注意：s.screenOff 为真只说明屏幕被系统熄了，**不代表播放失败**。
       * 熄屏时 planeActive 会变 0，如果按"planeActive=0 就是错误"处理，
       * 用户只是让屏幕自动熄灭就会被弹到错误页。所以这里不做任何错误判定。 */
      if (s.state === 'paused') this.paused = true;
      if (s.state === 'error') {
        this.phSub = '播放异常：' + (s.lastErrorMsg || s.lastError || '未知');
      }
    },
    onTouchStart(e) {
      this.feedTouch(e, 'down');
    },
    onTouchMove(e) {
      this.feedTouch(e, 'move');
    },
    onTouchEnd(e) {
      this.feedTouch(e, 'up');
    },
    /* 触摸 → 归一化坐标。
     * pageX/pageY 在 Falcon 里是**相对被触摸元素**的坐标，而触摸层正好铺满
     * video rect，所以 (pageX - rect.x) 就等于相对 rect 的偏移。
     * 关键：**不钳位** —— 超出 [0,1] 是游戏判断"拖出画面=取消"的信号，钳了会误操作。 */
    feedTouch(e, action) {
      if (this.mode !== 'playing') return;
      const list = (e && e.changedTouches) || [];
      for (let i = 0; i < list.length; i++) {
        const t = list[i];
        if (!t) continue;
        const nx = (num(t.pageX) - VIDEO_RECT.x) / VIDEO_RECT.w;
        const ny = (num(t.pageY) - VIDEO_RECT.y) / VIDEO_RECT.h;
        const fid = typeof t.identifier === 'number' ? t.identifier : 0;
        try {
          player.touch(nx, ny, action, fid);
        } catch (err) {
          /* 播放层未就位时忽略，不要因为一次触摸失败打断 UI */
        }
      }
    },
    async togglePause() {
      const next = !this.paused;
      try {
        if (next) await player.pause();
        else await player.resume();
      } catch (e) {
        logWarn('暂停/继续失败: ' + ((e && e.message) || e));
        return;
      }
      this.paused = next;
      logWarn(next ? '已暂停' : '已继续');
    },
    cycleGear() {
      if (!this.gears.length) return;
      const gi = (this.gearIndex + 1) % this.gears.length;
      this.gearIndex = gi;
      this.gearName = this.gears[gi].name;
      /* 画质在会话中途切换需要重连（服务端按档位分配实例），
       * 这里只记录选择，下次开始游戏时生效 —— 不做静默重连，避免用户正在打怪时黑屏。 */
      logWarn('画质切换到 ' + this.gearName + '（下次开始游戏生效）');
    },
    toggleMute() {
      this.muted = !this.muted;
      /* player.js 目前没有音量接口（音频在 native 侧由 gst 管线控制），
       * 所以这里只切换 UI 状态并留日志，等 native 暴露 volume 后再接。 */
      logWarn('音量开关 → ' + (this.muted ? '静音' : '有声') + '（native 音量接口待接入）');
    },
    async leaveGame(where) {
      logWarn('离开游戏（' + where + '）');
      this._gen++;
      this.stopStatTimer();
      this.mode = 'home';
      this.videoReady = false;
      this.railVisible = true;
      this.syncKeep();
      try {
        await player.stop();
        logWarn('画面已释放');
      } catch (e) {
        logWarn('player.stop 异常: ' + ((e && e.message) || e));
      }
      this.loadHome();
    },

    /* ---------- error ---------- */
    showError(code, detail, retryKind) {
      this.errCode = String(code == null ? '' : code);
      const known = ERR_TEXT[this.errCode];
      this.errUnknown = !known;
      this.errMsg = known || ('未知错误（错误码 ' + this.errCode + '）');
      this.errDetail = detail || '';
      this.retryKind = retryKind || 'login';
      this.mode = 'error';
      this.syncKeep();
      logWarn('进入错误页 code=' + this.errCode + ' msg=' + this.errMsg + ' detail=' + this.errDetail);
    },
    retry() {
      logWarn('用户重试 retryKind=' + this.retryKind);
      if (this.retryKind === 'dispatch') {
        this.mode = 'home';
        this.syncKeep();
        this.loadHome();
        return;
      }
      if (this.retryKind === 'home') {
        this.gotoHome();
        return;
      }
      this.gotoLogin();
    },
    backHome() {
      if (!this.nickname && !ys.isLoggedIn()) {
        this.gotoLogin();
        return;
      }
      this.gotoHome();
    },

    /* ---------- 退出登录 ---------- */
    async doLogout() {
      this.overlay = '';
      this._gen++;
      this.stopPoll();
      this.stopStatTimer();
      try {
        await player.stop();
      } catch (e) {}
      try {
        await ys.logout();
      } catch (e) {
        logWarn('logout 失败: ' + ((e && e.message) || e));
      }
      this.nickname = '';
      this.uid = '';
      this.videoReady = false;
      logWarn('已退出登录');
      this.gotoLogin();
    },

    /* ---------- 生命周期（由 BasePage 转发，见 base-page.js） ---------- */
    onShow() {
      logWarn('onShow mode=' + this.mode);
      this.syncKeep();
      if (this.mode === 'playing' && this._bgPaused) {
        this._bgPaused = false;
        const myGen = this._gen;
        player.resume().then(
          () => {
            if (this._dead || myGen !== this._gen) return;
            this.paused = false;
            logWarn('回前台已恢复画面');
          },
          (e) => logWarn('回前台恢复失败: ' + ((e && e.message) || e))
        );
      }
    },
    onHide() {
      logWarn('onHide mode=' + this.mode);
      /* 退后台：只暂停 + 停保活，**不 teardown**。
       * 保活必须连定时器一起停，否则后台还每 20s 唤醒屏幕（烧屏 + 耗电）。 */
      if (this.mode === 'playing' && this.videoReady && !this.paused) {
        this._bgPaused = true;
        try {
          const p = player.pause();
          if (p && p.then) p.then(null, function () {});
        } catch (e) {}
        this.paused = true;
      }
      this.stopKeepScreen('onHide');
    },
    onUnload() {
      logWarn('onUnload');
      this.teardown('onUnload');
    },
    teardown(where) {
      if (this._dead) return;
      this._dead = true;
      this._gen++;
      this.stopPoll();
      this.stopStatTimer();
      this._intervals.forEach((t) => {
        try {
          clearInterval(t);
        } catch (e) {}
      });
      this._intervals.clear();
      this._timers.forEach((t) => {
        try {
          clearTimeout(t);
        } catch (e) {}
      });
      this._timers.clear();
      this.stopKeepScreen(where);
      /* KMS 平面不会随页面销毁自动消失，必须显式 stop ——
       * 否则桌面会被一帧冻结的游戏画面盖住，这是本设备最脏的故障。 */
      try {
        const p = player.stop();
        if (p && p.then) {
          p.then(
            () => logWarn('销毁时画面已释放'),
            (e) => logWarn('销毁时 player.stop 失败: ' + ((e && e.message) || e))
          );
        }
      } catch (e) {
        logWarn('销毁时 player.stop 异常: ' + ((e && e.message) || e));
      }
    }
  }
};

/* 步骤条数据：把"点"和"连接线"摊平成一个数组，模板里只用一个 v-for。
 * 为什么不用 <template v-for> 或嵌套循环：Falcon 的模板编译器对这两者都没验证过，
 * 摊平数组是最保守的写法。 */
function buildDispSteps() {
  const labels = ['登录云端账号', '测速与选节点', '分配渲染节点', '启动游戏实例'];
  const out = [];
  for (let i = 0; i < labels.length; i++) {
    out.push({ kind: 's', si: i, label: labels[i] });
    if (i < labels.length - 1) out.push({ kind: 'l', si: i });
  }
  return out;
}
</script>

<style lang="less" scoped>
/* 每个 .vue 必须自带 scoped style 并 @import base.less，否则 Falcon 侧黑屏
 * （2026-09-22 真机证据）。样式全部集中在 styles/base.less，按功能分节、
 * 单类名、零 inline style、零 position:absolute。 */
@import "../../styles/base.less";
</style>
