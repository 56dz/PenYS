// 屏幕几何（纯函数，Node 可单测）
//
// 设备 profile（X6 Pro 真机截屏 + cfg.json 证实）：
//   logical  : 960 x 266  （Falcon UI 合成面）
//   physical : 266 x 960  （面板，direction=270）
//   触控     : direction=270, tp_xoffset=113
//
// 本项目布局（v3 定稿）：左栏 76 + 内容 884 = 960（主页）；
// 播放态 v4 布局（用户手绘稿，m11950）：左 2 : 中 4.4 : 右 2.6 ⇒ 213 : 469 : 278 = 960。
//   左列上 1.8 高（192）= 视频标题 + 简介；左列下 0.7 高（74）= 点赞/投币/收藏 三个按钮；
//   中列 469x240 = 视频，其下 26px = 可拖动进度条；
//   右列上 2.25 高（240）= 评论区；右列下 0.25 高（26）= 输入框 + 发送按钮。
//   高度基准：1.8 + 0.7 = 2.25 + 0.25 = 2.5 单位 ⇒ 1 单位 = 266/2.5 = 106.4px。
//
// 为什么视频区高度是 240 而不是 266：X6 Pro 的视频走 KMS overlay 平面，
// **盖在 Falcon UI 之上**，UI 画不上去。所以进度条必须落在视频矩形之外。

export const LOGICAL = { width: 960, height: 266 };
export const PHYSICAL = { width: 266, height: 960 };
export const DIRECTION = 270;

// 面板列偏移：266 宽的逻辑列在 480 宽面板里的起始列。
// native（player.c `#define PANEL_X_OFFSET 107`）在拼 render-rectangle 时加上它；
// screen.js 只产出 266 列内的坐标，所以 x 端是 0..266 而不是 107..373。
export const PANEL_X_OFFSET = 107;

// 主页左栏宽度（76 + 884 = 960）
export const BAR_WIDTH = 76;

// 主页视频网格列数：.grid 870 宽 / .vrow 174（166 + margin-right 8）⇒ 5 列。
// 用户要求 m11950-3：加载更多后条目数必须能被列数整除，否则最后一行填不满。
export const GRID_COLS = 5;

// 把条目列表裁到列数的整数倍（尾部余数丢掉），并报告是否发生了裁剪。
// 只裁尾部、不重排，保证已有卡片位置不变。
/* 给「条目可能很少」的场景用的安全版：够一行以上才裁末行，
 * 否则原样返回。浏览分区（实测有只挂 1~3 间房的小分类）和搜索结果
 * （斗鱼「标题直接命中」常常只有 1~2 间）都必须走这个，
 * 直接 trimToGridRows 会把整页裁成空白 —— 界面显示「没有搜到」但实际有结果。
 * trimToGridRows 本身保持原语义，单测仍覆盖它的纯裁行行为。 */
export function trimGridSafe(items, cols) {
  const list = Array.isArray(items) ? items : [];
  const n = Math.max(1, Math.round(Number(cols) || GRID_COLS));
  if (list.length <= n) return { items: list, trimmed: 0 };
  return trimToGridRows(list, n);
}

export function trimToGridRows(items, cols) {
  const list = Array.isArray(items) ? items : [];
  const n = Math.max(1, Math.round(Number(cols) || GRID_COLS));
  const rem = list.length % n;
  if (rem === 0) return { items: list, trimmed: 0 };
  return { items: list.slice(0, list.length - rem), trimmed: rem };
}

// 播放态三列宽度（用户手绘稿 m11950：左 2 : 中 4.4 : 右 2.6）
//   213 + 469 + 278 = 960
export const LEFT_COL_WIDTH = 213;
export const MID_COL_WIDTH = 469;
export const RIGHT_COL_WIDTH = 278;

// 左列上下两块高度（1.8 : 0.7 单位，1 单位 = 106.4px）
export const LEFT_INFO_H = 192;
export const LEFT_BTN_H = 74;

// 右列上下两块高度（2.25 : 0.25 单位）
export const RIGHT_BODY_H = 240;
export const RIGHT_INPUT_H = 26;

// 视频下方进度条高度
export const SEEK_H = 26;

/* 进度条轨道几何（defect-inventory P1-14 修复）。
 *
 * 以前 index.vue 里把 `(x - 8) / 453` 和 `443 * ratio` 两组数字**写死**在页面里：
 * 布局一改（中列宽变、padding 变、圆点直径变）就静默错位，而且没有任何测试能发现。
 * 现在统一从列宽推导，页面只调 seekRatioFromX / seekFillWidth，单测直接覆盖这两个函数。
 *
 * 布局事实（用户手绘稿 m11950）：.seek 占满中列 469，左右 padding 各 8 → 轨道 453；
 * 圆点直径 10 与填充同排 → 填充最大 443。
 * pageX 是「相对被触摸元素」的坐标（不是整页坐标）：在 .seek 内点 x=37 拿到的就是 37，
 * 所以只需减 8px 左 padding，不用再减中列的绝对 x。 */
export const SEEK_PAD_X = 8;
export const SEEK_TRACK_WIDTH = MID_COL_WIDTH - SEEK_PAD_X * 2; // 469 - 16 = 453
export const SEEK_DOT_W = 10;
export const SEEK_FILL_MAX = SEEK_TRACK_WIDTH - SEEK_DOT_W; // 443

// 触点 x → [0,1] 比例；非数字返回 null（页面据此忽略这次触摸）
export function seekRatioFromX(x, trackWidth) {
  const n = Number(x);
  if (!isFinite(n)) return null;
  const w = Number(trackWidth) > 0 ? Number(trackWidth) : SEEK_TRACK_WIDTH;
  const r = (n - SEEK_PAD_X) / w;
  if (!isFinite(r)) return null;
  return r < 0 ? 0 : r > 1 ? 1 : r;
}

// [0,1] 比例 → 填充像素宽（右端留出圆点直径，否则圆点会顶出轨道）
export function seekFillWidth(ratio, fillMax) {
  const r = Number(ratio);
  if (!isFinite(r)) return 0;
  const max = Number(fillMax) > 0 ? Number(fillMax) : SEEK_FILL_MAX;
  const w = Math.round(max * (r < 0 ? 0 : r > 1 ? 1 : r));
  return w < 0 ? 0 : w > max ? max : w;
}

// 播放态视频区（逻辑面坐标）：x 紧接左列，底部 26px 留给进度条
export const VIDEO_AREA = { x: LEFT_COL_WIDTH, y: 0, width: MID_COL_WIDTH, height: 240 };

// 直播态视频区：没有进度条，整列 266 全给画面（用户反馈 m07570-3）。
//
// 为什么必须单独一个矩形而不是复用 VIDEO_AREA：KMS 视频平面是硬件 overlay，
// 它盖在 falcon UI 之上，所以 falcon 侧把 .pvideo 撑到 266（.pvideo-live）之后，
// 若平面仍是 240 高，画面下方 26px 会留一条黑边，而 hole 的点击热区却是 266 ——
// 用户点那条黑边能暂停、看着却像没画面。两边必须严格一致。
export const LIVE_VIDEO_AREA = { x: LEFT_COL_WIDTH, y: 0, width: MID_COL_WIDTH, height: 266 };

// 旧名兼容（图文态仍用「图标栏」这个概念时不再需要，保留导出避免既有 import 报错）
export const ICON_BAR_WIDTH = LEFT_COL_WIDTH;
export const ICON_BAR_GAP = 0;

// 视频列逻辑宽度（= VIDEO_AREA.width，保留旧名给既有调用方）
export const VIDEO_COL_WIDTH = VIDEO_AREA.width;

function num(v, fallback) {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return isFinite(n) && n > 0 ? Math.round(n) : fallback;
}

// 逻辑空间信箱适配：返回 { x, y, width, height }（居中）
export function fitLogicalRect(videoWidth, videoHeight, logical) {
  const box = logical || LOGICAL;
  const vw = num(videoWidth, 640);
  const vh = num(videoHeight, 360);
  const boxAspect = box.width / box.height;
  const aspect = vw / vh;
  let w;
  let h;
  if (aspect >= boxAspect) {
    w = box.width;
    h = Math.round(box.width / aspect);
  } else {
    h = box.height;
    w = Math.round(box.height * aspect);
  }
  if (w > box.width) w = box.width;
  if (h > box.height) h = box.height;
  return {
    x: Math.round((box.width - w) / 2),
    y: Math.round((box.height - h) / 2),
    width: w,
    height: h
  };
}

// 本项目的视频逻辑矩形：信箱适配后居中放在视频区 VIDEO_AREA 内
export function probeLogicalVideoRect(videoWidth, videoHeight) {
  const fit = fitLogicalRect(videoWidth, videoHeight, {
    width: VIDEO_AREA.width,
    height: VIDEO_AREA.height
  });
  return { x: VIDEO_AREA.x + fit.x, y: VIDEO_AREA.y + fit.y, width: fit.width, height: fit.height };
}

// 逻辑矩形 → 物理矩形（**真·旋转变换**，不是"面板内居中"近似）
//
// 由真机像素测量反推（详见 docs/x6pro-video-playback.md §2.1）：
// 逻辑面上高亮色块的 bbox (10,0)-(932,170) 落在面板 (108,28)-(276,948)，
// 唯一自洽的映射是
//     panel_x = PANEL_X_OFFSET + logical_y      （逻辑 y 沿面板 +x）
//     panel_y = 960 − logical_x                 （逻辑 x 沿面板 −y，反向）
// C 侧的 PANEL_X_OFFSET=107 由 native 加，这里只给 266 宽列内的坐标，
// 所以 x 端就是 logical_y，矩形则是两端点取包围盒：
//     phys.x = rect.y
//     phys.y = panel.height − (rect.x + rect.width)
//     phys.w = rect.height,  phys.h = rect.width
//
// 旧实现是 x=(266-h)/2（恒 0）、y=(960-w)/2（面板内居中）。它在
// "视频本来就水平居中于 960 逻辑面"时与真变换重合（老布局 16:9 差 1px），
// 一旦左右栏宽度变化或视频不再占满整列高度，就会偏出去（新布局 16:9 偏 65px）。
export function logicalRectToPhysicalRect(rect, logical, physical) {
  const box = logical || LOGICAL;
  const panel = physical || PHYSICAL;
  const x = Math.round(num(rect.x, 0));
  const y = Math.round(typeof rect.y === 'number' ? rect.y : 0);
  const w = Math.round(num(rect.width, box.height));
  const h = Math.round(num(rect.height, box.width));
  return {
    x: y,
    y: panel.height - (x + w),
    width: h,
    height: w
  };
}

// native 需要的物理视频矩形（含偶数对齐，避免行字节错位）
export function probeVideoRects(videoWidth, videoHeight) {
  const logical = probeLogicalVideoRect(videoWidth, videoHeight);
  const physical = logicalRectToPhysicalRect(logical, LOGICAL, PHYSICAL);
  return { logical: logical, physical: physical };
}

// 播放态视频矩形：信箱适配到 VIDEO_AREA(448x240) 并在区内居中。
//
// 不能用全屏 960x266 作 box：21:9 等比 16:9 更宽的视频会按高适配得 w>448 →
// blit 越出视频区、溢出到左右栏（真机实测 bug）。也不能只约束宽度：
// 视频区高度是 240（底部 26px 留给进度条，overlay 平面盖住 UI 画不上去）。
export function playVideoRects(videoWidth, videoHeight, area) {
  // area 缺省 = 点播视频区（240 高）。直播传 LIVE_VIDEO_AREA（266 高）。
  // 只换 box 的 height/x/width 来源，信箱适配与物理换算复用同一套逻辑，
  // 避免为直播另写一份换算（旋转变换错了极难排查，见上面 logicalRectToPhysicalRect 注释）。
  const box = area || VIDEO_AREA;
  const fit = fitLogicalRect(videoWidth, videoHeight, {
    width: box.width,
    height: box.height
  });
  const logical = {
    x: box.x + fit.x,
    y: box.y + fit.y,
    width: fit.width,
    height: fit.height
  };
  const physical = logicalRectToPhysicalRect(logical, LOGICAL, PHYSICAL);
  return { logical: logical, physical: physical };
}

// 逻辑点 → 物理点（四种 direction 的纯函数；触控/调试对拍用）
export function logicalToPhysicalPoint(point, direction, logical, physical) {
  const box = logical || LOGICAL;
  const panel = physical || PHYSICAL;
  const x = Math.round(typeof point.x === 'number' ? point.x : 0);
  const y = Math.round(typeof point.y === 'number' ? point.y : 0);
  const cx = Math.min(Math.max(x, 0), box.width - 1);
  const cy = Math.min(Math.max(y, 0), box.height - 1);
  const dir = ((Math.round(direction / 90) * 90) % 360 + 360) % 360;
  if (dir === 0) return { x: cx, y: cy };
  if (dir === 90) return { x: cy, y: box.width - 1 - cx };
  if (dir === 180) return { x: box.width - 1 - cx, y: box.height - 1 - cy };
  // 270
  return { x: box.height - 1 - cy, y: cx };
}

// ---------------- 屏幕保活（缺陷 C 修复）----------------
//
// 背景（docs/audit/blackout-root-cause.md 真机时间线）：
//   570s  hal-screen down  → 亮度 113 → 5（几乎全黑）
//   600s  hal-screen off   → 背光断电 + DRM crtc disable → gst/ffmpeg 进程被回收
// 两者差 30s，用户在这 30s 内碰一下屏幕就是"黑一下又亮"= 用户报的"闪黑一次"。
// 息屏后 `pidof gst-launch-1.0` / `pidof ffmpeg` 均为空 —— 进程已死，所以第 1 次点击
// 只能亮屏、出不了画，第 2 次点击（重新拉起管线）才恢复。
//
// 对策：播放态（含暂停态）每 KEEP_INTERVAL_MS 续期一次屏幕。
// 必须 < 30s —— `hal-screen keep` 自身只续 30 秒。
//
// 续期手段（Falcon 内置 JSAPI `brightness` 模块，符号表
// /etc/miniapp/jsapis/libjsapi_export.so 确认）：
//   JSBrightnessProxy::keepScreenOn / startAlwaysScreenOn / stopAlwaysScreenOn
//   JSBrightnessProxy::getBrightness / setBrightness(int) / getScreenState
//   JSGlobalProxy 有同样一套；YDALAPI::YBrightnessModule 提供 keepScreenOn
//
// 真机教训（2026-10，缺陷 C 排查）：只调 keepScreenOn() 是**不够**的。
//   - keepScreenOn/hal-screen keep 只续「30s 不动作窗口」，它**不恢复已经掉下去的亮度**：
//     一旦被 idle 调到 bl_br=5，只有 hal-screen set / reset（JS 侧 setBrightness）能拉回。
//   - 设备上残留过 `[Idle] 1s = hal-screen down` 的测试配置，导致 keepScreenOn 生效
//     也只有 1.5s 的亮度回升 —— 所以"保活生效了吗"必须靠**持续采样的日志**证明，
//     不能只看调用是否发出。本段代码因此把每次调用的返回值、异常、亮度、屏幕状态
//     全打进日志。
//
// JS 层不能跑 shell，所以不用 system()/native.exec；模块不可用时只打一次日志降级
// （保活失败绝不能影响播放本身）。
export const KEEP_INTERVAL_MS = 20000;

/* 亮度兜底阈值（硬兜底，P0-1 要求 3）
 * 真机实测：正常 bl_br=113；被 idle 调暗后 bl_br=5（用户感知"屏幕变暗"）。
 * 阈值 20：离 5 有安全余量，又远低于正常档位；恢复档位取 sys_br 默认值 80。
 * **语义坑（真机实测）**：hal-screen get / JSAPI getBrightness() 返回的 80 是
 * 「设置档位」不是实际 bl_br —— 屏幕被调暗到 5 时 getBrightness() 依然返回 80。
 * 所以主判据必须是 getScreenState()（见 needsBrightRestore），
 * 这两个亮度常量只在 state 读不到时当兜底判据用。 */
export const KEEP_BRIGHT_MIN = 20;
export const KEEP_BRIGHT_RESTORE = 80;

/* 「息屏超时」兜底（P0-1 要求 3 的第二种打法，比 20s 轮询干净）
 * 设备实测：`/etc/input-event-daemon.conf` 的 `[Idle]` 是硬的 30s 调暗 / 60s 熄屏，
 * 而 `hal-screen bright_time`（默认 60）**只影响它自己那层**，改成 1080 之后
 * 30s/60s 的 idle 仍然照常触发 —— 所以不能只靠它，但它是 zero-risk 的额外一层：
 * 首次保活时调一次 setCloseScreenTime(1080)，若系统认这个值就能少轮询很多次。
 * 1080s = 18 分钟：覆盖 10 分钟的息屏设置 + 余量，又低于 1200s
 * （`/usr/bin/check_stop_input_deamon_times.sh` 超过 1200s 会 killall -USR2 重置）。 */
export const KEEP_CLOSE_SCREEN_SEC = 1080;

// 当前命中的保活后端（命中即缓存；未命中每次 tick 会再试同步形态）
//   { name, keep, always, stopAlways, getBright, setBright, getState }
let keepBackend = null;
let keepBackendProbed = false;
let keepBackendLoading = false;
let keepWarned = false;
let keepTicks = 0; // keepScreenOnce 调用计数（日志里的 #N）
let alwaysArmed = false; // 本轮保活会话是否已调过 startAlwaysScreenOn（keepScreenOff 会复位）

function logLine(msg) {
  // console.log 不落盘（storage.js logWarn 用的是 console.warn），统一走 warn
  try {
    console.warn('[screen] ' + msg);
  } catch (e) {
    /* 忽略 */
  }
}

function warnOnce(msg) {
  if (keepWarned) return;
  keepWarned = true;
  logLine(msg);
}

function errText(e) {
  if (!e) return '未知错误';
  return String((e && e.message) || e);
}

function numText(v) {
  return v == null ? 'null' : String(v);
}

/* getScreenState() 返回值语义（真机实测；与 hal-screen 的 screen_state 是两套编号）
 *   0 = 亮屏（SCREEN_ON）
 *   3 = 调暗（DIM）。真机实测 `hal-screen down` 之后 bl_br=5 / bl_power=0（背光仍上电
 *       但亮度没了）时返回 3，用户感知是"屏幕变暗"。**这个状态能被救回来**：
 *       实测下一轮 keep 就把它拉回 state=0 + br=113。
 *   1 = 熄屏（OFF）。真机实测 bl_power=4 / screen_on=0 / `hal-screen state` 报
 *       SCREEN_OFF 时返回 1。这是断电状态，可能来自 idle 60s 也可能来自用户按电源键。
 *       播放中一律救（用户诉求是"播视频时不许熄屏"）；用户真按了电源键会走 onHide
 *       → 停保活，那时这里根本不会被调用，不存在和他抢屏幕的问题。
 *   其它 = 未知，一律按"非亮屏"处理（宁可当作息屏，看门狗不该在息屏时恢复）
 * 只有 0 能确定为亮屏 —— 页面 isScreenOff() 用的是 `s !== 0`，与此一致。 */
export function screenStateName(v) {
  const n = Number(v);
  if (v == null || !isFinite(n)) return '未知';
  if (n === 0) return 'ON';
  if (n === 1) return 'OFF';
  if (n === 3) return 'DIM';
  if (n === 4) return '未知(4)';
  return '未知(' + n + ')';
}

/* 是否需要走「屏幕兜底」把屏幕拉回来（纯函数，便于单测）
 *
 * 为什么不能只看 getBrightness()：真机实测 `hal-screen down` 后硬件 bl_br=5 / bl_power=0，
 * 但 JSAPI getBrightness() 仍返回 80（= 配置档位 sys_br，与 `hal-screen get` 的
 * get_brightness 同义）。它反映的是"亮度设置值"，不是"当前实际背光"，所以
 * `bright <= 20` 这个判据在真机上永远不成立（兜底成了死代码）。
 *   判定改用 getScreenState()：
 *     3(DIM) —— 被 idle 调暗（bl_br=5 / bp=0），必须救
 *     1(OFF) —— 已断电（bp=4），**播放中也要救**：用户诉求是"播视频时不许熄屏"，
 *               且实测 keepScreenOn() 能把 bp=4 拉回 bp=0（16:39:57 实证）。
 *               用户主动按电源键时页面会走 onHide → 停保活，这里不会跟他抢。
 *     0(ON)  —— 不用救
 * bright 参数仍然参与判断：若 state 读不到（null）而亮度确实极低，也救一次。 */
export function needsBrightRestore(bright, state) {
  const s = state == null ? null : Number(state);
  if (s === 3 || s === 1) return true;             // DIM / OFF：都得拉回来
  if (s === 0) return false;                       // 亮屏：不动
  const n = Number(bright);
  return bright != null && isFinite(n) && n <= KEEP_BRIGHT_MIN; // state 拿不到时退回亮度判据
}

/* keepScreenOn() 之后是否需要再补一次（深熄态下的确定性问题）。
 * 真机实测（2026-11-01，hal-screen keep 单独验证）：从 bp=4（背光断电）状态，
 * 第 1 次 keep 只把 /tmp/.screen_state 置 1、bp 仍是 4、brightness 变 113；
 * **第 2 次 keep 才把 bp 拉回 0**（真正点亮）。所以 keep 完复检 state，
 * 仍是 OFF(1) 就再补一次。抽成纯函数是为了能单测（Node 里没有 JSAPI 后端）。 */
export function keepRetryNeeded(state) {
  if (state == null) return false; // 拿不到状态时无从判断，交给 ③ 兜底
  return Number(state) === 1;
}

/* 设置息屏超时（一次性，管很久）。真机实测 hal-screen bright_time 本身
 * 改不动 input-event-daemon 的 [Idle] 30s/60s，所以这只是额外一层保险：
 * 系统若认 JSAPI 这个值，就能少触发几次 idle。返回给日志用的注记串。 */
function applyCloseScreenTime(b) {
  if (!b || typeof b.setClose !== 'function') return ' closeScreen=缺接口';
  try {
    const r = b.setClose(KEEP_CLOSE_SCREEN_SEC);
    if (r === false) return ' closeScreen=设' + KEEP_CLOSE_SCREEN_SEC + 's返回false';
    let back = '';
    if (typeof b.getClose === 'function') {
      try {
        back = ' 回读=' + numText(b.getClose());
      } catch (e) {
        back = ' 回读异常';
      }
    }
    return ' closeScreen=' + KEEP_CLOSE_SCREEN_SEC + 's ok' + back;
  } catch (e) {
    return ' closeScreen=抛错 ' + errText(e);
  }
}

/* 把一个模块/实例对象包成后端。三种形态都能适配：
 *   扁平命名空间（$falcon.jsapi.brightness）、ESM default、类实例（new Brightness()）。
 * 缺 keepScreenOn 一律视为不是亮度后端；其余能力（always/亮度读写/状态）有则用。 */
function makeBackend(name, holder) {
  if (!holder || (typeof holder !== 'object' && typeof holder !== 'function')) return null;
  const pick = (k) => (typeof holder[k] === 'function' ? holder[k] : null);
  const keep = pick('keepScreenOn');
  if (!keep) return null;
  const always = pick('startAlwaysScreenOn');
  const stopAlways = pick('stopAlwaysScreenOn');
  const getBright = pick('getBrightness');
  const setBright = pick('setBrightness');
  const getState = pick('getScreenState');
  // 息屏超时（一次调用保很久，比每 20s 续期更干净）
  const setClose = pick('setCloseScreenTime');
  const getClose = pick('getCloseScreenTime');
  // 屏幕开关（1=亮 / 0=灭）。真机实测：亮度/背光已被断电（bp=4）时 setBrightness
  // 只能改档位、不给背光上电；screenOnOff(1) 才是"开屏"那个动作。
  const screenOnOff = pick('screenOnOff');
  return {
    name: name,
    // 全部用 call(holder) 绑定：类实例方法的 this 必须指向实例
    keep: () => keep.call(holder),
    always: always ? () => always.call(holder) : null,
    stopAlways: stopAlways ? () => stopAlways.call(holder) : null,
    getBright: getBright ? () => getBright.call(holder) : null,
    setBright: setBright ? (v) => setBright.call(holder, v) : null,
    getState: getState ? () => getState.call(holder) : null,
    setClose: setClose ? (v) => setClose.call(holder, v) : null,
    getClose: getClose ? () => getClose.call(holder) : null,
    onOff: screenOnOff ? (v) => screenOnOff.call(holder, v) : null
  };
}

// 类形态：new Cls() 后取实例方法
function makeBackendFromClass(name, Cls) {
  if (typeof Cls !== 'function') return null;
  let inst = null;
  try {
    inst = new Cls();
  } catch (e) {
    return null;
  }
  return makeBackend(name, inst);
}

// 探测日志：三种形态分别打一行，最后打一行汇总（只打一次，避免重试刷屏）
let probeSummaryLogged = false;
function noteProbe(shape, hit, detail) {
  logLine('probe 形态=' + shape + ' → ' + (hit ? '命中' : 'miss(' + detail + ')'));
}

function logProbeSummary() {
  if (probeSummaryLogged) return;
  probeSummaryLogged = true;
  if (!keepBackend) {
    logLine('后端探测结论：全部未命中 → 保活降级（需 native 侧兜底）');
    return;
  }
  const b = keepBackend;
  logLine(
    '后端探测结论：采用 ' + b.name +
    ' 能力[keep=1 always=' + (b.always ? 1 : 0) + ' stopAlways=' + (b.stopAlways ? 1 : 0) +
    ' getBright=' + (b.getBright ? 1 : 0) + ' setBright=' + (b.setBright ? 1 : 0) +
    ' getState=' + (b.getState ? 1 : 0) +
    ' setClose=' + (b.setClose ? 1 : 0) + ' getClose=' + (b.getClose ? 1 : 0) + ']'
  );
}

// 同步形态 ①：$falcon.jsapi.brightness（扁平命名空间，运行时若已注入最省事）
// probeSync 会被 getScreenState() 每秒轮询到 → 未命中时也只打一次探测日志
let syncProbeLogged = false;
function probeSync() {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  const mod = f && f.jsapi && f.jsapi.brightness;
  const hit = makeBackend('$falcon.jsapi.brightness', mod);
  if (hit || !syncProbeLogged) {
    syncProbeLogged = true;
    noteProbe('$falcon.jsapi.brightness(扁平)', !!hit, mod ? '缺 keepScreenOn' : '未注入');
  }
  if (hit) {
    keepBackend = hit;
    keepBackendProbed = true;
    logProbeSummary();
  }
  return hit;
}

/* 异步形态 ②③：裸模块 'brightness'（构建器保留为运行时 require，同
 * src/services/input.js 的 `import globalModule from 'global'`）→ 全局代理 'global'。
 * 只在首次调用时探一次，不阻塞调用方。 */
function probeAsync() {
  if (keepBackendLoading || keepBackend) return;
  keepBackendLoading = true;
  Promise.resolve()
    .then(() => import('brightness'))
    .then((m) => {
      const d = m && m.default ? m.default : m;
      // 形态 ②：ESM default 上的扁平方法
      let hit = makeBackend('brightness.default(扁平)', d);
      noteProbe('brightness.default(扁平)', !!hit, '缺 keepScreenOn');
      if (hit) return hit;
      // 形态 ③：new Brightness()（真机旧日志里命中的就是这一形态）
      const Cls = d && (d.Brightness || (d.default && d.default.Brightness));
      hit = makeBackendFromClass('brightness.Brightness(new)', Cls);
      noteProbe('brightness.Brightness(new)', !!hit, Cls ? '实例缺 keepScreenOn' : '类不存在');
      return hit || null;
    })
    .catch((e) => {
      noteProbe('brightness 模块', false, '加载失败 ' + errText(e));
      return null;
    })
    .then((hit) => {
      if (hit) return hit;
      return import('global')
        .then((gm) => {
          const g = gm && gm.default ? gm.default : gm;
          let h = makeBackendFromClass('global.Global(new)', g && g.Global);
          noteProbe('global.Global(new)', !!h, '类不存在或实例缺 keepScreenOn');
          if (h) return h;
          h = makeBackend('global.default(扁平)', g);
          noteProbe('global.default(扁平)', !!h, '缺 keepScreenOn');
          return h || null;
        })
        .catch((e) => {
          noteProbe('global 模块', false, '加载失败 ' + errText(e));
          return null;
        });
    })
    .then((hit) => {
      keepBackendLoading = false;
      keepBackendProbed = true;
      if (hit) keepBackend = hit;
      else warnOnce('无 brightness JSAPI，屏幕保活降级（需 native 侧兜底）');
      logProbeSummary();
    });
}

// 读亮度（拿不到返回 null）
function readBright() {
  const b = keepBackend;
  if (!b || typeof b.getBright !== 'function') return null;
  try {
    const v = Number(b.getBright());
    return isFinite(v) ? v : null;
  } catch (e) {
    return null;
  }
}

// 写亮度 → { ok, err }
function writeBright(val) {
  const b = keepBackend;
  if (!b || typeof b.setBright !== 'function') return { ok: false, err: 'setBrightness 能力缺失' };
  try {
    const r = b.setBright(val);
    if (r === false) return { ok: false, err: 'setBrightness 返回 false' };
    return { ok: true, err: '' };
  } catch (e) {
    return { ok: false, err: errText(e) };
  }
}

function readState() {
  const b = keepBackend;
  if (!b || typeof b.getState !== 'function') return null;
  try {
    const v = Number(b.getState());
    return isFinite(v) ? v : null;
  } catch (e) {
    return null;
  }
}

// 开/关屏（1=亮 / 0=灭）→ { ok, err }。亮度档位恢复不了断电的背光，这个才能。
function writeScreenOn(val) {
  const b = keepBackend;
  if (!b || typeof b.onOff !== 'function') return { ok: false, err: 'screenOnOff 能力缺失' };
  try {
    const r = b.onOff(val);
    if (r === false) return { ok: false, err: 'screenOnOff 返回 false' };
    return { ok: true, err: '' };
  } catch (e) {
    return { ok: false, err: errText(e) };
  }
}

/* 续期一次。返回值语义：true=本轮至少有一条动作成功（不代表系统一定接受），
 * false=后端还没就绪 / 所有动作都失败（keeper 据此用短间隔快速重试）。
 *
 * 每调用一次就打一行（P0-1 要求 1，每 20s 一行）：
 *   [screen] keep #3 backend=brightness.Brightness(new) ok bright=113 state=0(ON)
 *   [screen] keep #4 backend=... err=keepScreenOn 抛错 xxx bright=null state=null
 */
export function keepScreenOnce() {
  keepTicks++;
  const n = keepTicks;
  if (!keepBackend) probeSync();
  if (!keepBackend) {
    if (!keepBackendProbed) probeAsync();
    // 后端还在异步探测 / 根本不存在：只在第一次提示，避免每 1s 重试刷屏
    warnOnce('brightness JSAPI 尚未就绪或缺失 → 本轮保活跳过（需 native 侧兜底）');
    return false;
  }
  const b = keepBackend;
  const errs = [];
  let ok = false;
  let alwaysNote = '';
  let closeNote = '';

  // ① 每个保活会话只做一次：startAlwaysScreenOn（持续亮屏，语义上正对 onShow/onHide）
  //    + setCloseScreenTime(1080)（改系统息屏超时，一次管很久）。
  //    两者都是"设一次就长期有效"的 API，比每 20s 续期干净；实测它们不一定
  //    能压过 input-event-daemon 的 30s/60s [Idle]，所以 ② 的轮询仍然保留。
  //    用 alwaysArmed 而不是 n===1：onHide 会 stopAlwaysScreenOn()，下次 onShow
  //    必须重新武装，否则第二个会话就没有持续亮屏保护了。
  if (!alwaysArmed) {
    if (typeof b.always === 'function') {
      try {
        const r = b.always();
        if (r === false) errs.push('startAlwaysScreenOn 返回 false');
        else {
          alwaysNote = ' always=on';
          ok = true;
        }
      } catch (e) {
        errs.push('startAlwaysScreenOn 抛错 ' + errText(e));
      }
    }
    closeNote = applyCloseScreenTime(b);
    alwaysArmed = true;
  }

  // ② 每次续 30s 窗口 —— 这才是保活的真正手段（等价 hal-screen keep，但不恢复亮度）
  let retryNote = '';
  if (typeof b.keep === 'function') {
    try {
      const r = b.keep();
      if (r === false) errs.push('keepScreenOn 返回 false');
      else ok = true;
    } catch (e) {
      errs.push('keepScreenOn 抛错 ' + errText(e));
    }
  }
  /* ②b 深熄时单次 keep 不保证能上电 —— 真机实测（2026-11-01，hal-screen keep）：
   * 从 bp=4（背光断电）状态，第 1 次 keep 只把 /tmp/.screen_state 置成 1、bp 仍是 4、
   * brightness 变成 113；**第 2 次 keep 才把 bp 拉回 0**（即真正点亮）。
   * 所以这里 keep 完立刻复检一次 state：仍是 OFF(1) 就再补发一次 keep。
   * 多花一次 JSAPI 调用换"一次 tick 内点亮"的确定性，比等下一个 20s 周期划算。 */
  let midState = readState();
  if (typeof b.keep === 'function' && keepRetryNeeded(midState)) {
    try {
      const r2 = b.keep();
      if (r2 === false) retryNote = ' retry=1(返回false)';
      else {
        retryNote = ' retry=1';
        midState = readState();
      }
    } catch (e) {
      retryNote = ' retry=1(抛错 ' + errText(e) + ')';
    }
  }

  // ③ 硬兜底：状态/亮度采样 + 恢复。keepScreenOn() 只续 30s 空闲窗口，不保证
  //    把已经被 idle 调暗/断电的屏幕拉回来，所以每轮都采样 getScreenState()+getBrightness()：
  //      DIM(3)  → setBrightness(80) 把背光档位拉回（实测能恢复 br=113）
  //      OFF(1)  → screenOnOff(1) 给背光上电（实测 setBrightness 不能上电，只能改档位）
  //    注意 getBrightness() 读的是配置档位、被调暗时仍是 80，所以主判据是 state。
  const bright = readBright();
  /* 复用 ②b 复检拿到的 state（retry 后已刷新），省一次 JSAPI 调用；
   * midState 为 null（后端没有 getScreenState）时才回退再读。 */
  const state = midState != null ? midState : readState();
  let fix = '';
  if (needsBrightRestore(bright, state)) {
    const sNum = state == null ? null : Number(state);
    if (sNum === 1) {
      // 已断电：先开屏，再把亮度档位补回来
      const on = writeScreenOn(1);
      if (on.ok) {
        ok = true;
        fix = ' 兜底=screenOnOff(1)ok';
      } else {
        fix = ' 兜底=screenOnOff(1)err=' + on.err;
        errs.push('开屏兜底失败 ' + on.err);
      }
      const w = writeBright(KEEP_BRIGHT_RESTORE);
      if (w.ok) fix += '+setBrightness(' + KEEP_BRIGHT_RESTORE + ')ok';
      else fix += '+setBrightness err=' + w.err;
    } else {
      const w = writeBright(KEEP_BRIGHT_RESTORE);
      if (w.ok) {
        ok = true;
        fix = ' 兜底=setBrightness(' + KEEP_BRIGHT_RESTORE + ')ok';
      } else {
        fix = ' 兜底=setBrightness(' + KEEP_BRIGHT_RESTORE + ')err=' + w.err;
        errs.push('亮度兜底失败 ' + w.err);
      }
    }
    logLine(
      '!!! 屏幕状态异常 state=' + numText(state) + '(' + screenStateName(state) + ')' +
      ' bright=' + numText(bright) + ' → 走兜底' + fix + ' !!!'
    );
  }

  logLine(
    'keep #' + n + ' backend=' + b.name + ' ' + (ok ? 'ok' : 'err=' + (errs.length ? errs.join('；') : '未知')) +
    ' bright=' + numText(bright) +
    ' state=' + numText(state) + (state == null ? '' : '(' + screenStateName(state) + ')') +
    alwaysNote + closeNote + retryNote + fix
  );
  return ok;
}

// 当前命中的保活后端名（诊断用；未命中为空串）
export function keepScreenBackend() {
  if (!keepBackend) probeSync();
  return keepBackend ? keepBackend.name : '';
}

// 读亮度（页面/探针用；拿不到返回 null）
export function getBrightness() {
  if (!keepBackend) probeSync();
  return readBright();
}

// 写亮度（页面/探针用）→ { ok, err }
export function setBrightness(v) {
  if (!keepBackend) probeSync();
  return writeBright(v);
}

/* 读屏幕电源状态（缺陷 C / 看门狗门禁用）。
 * 返回 0=亮屏、3=调暗、1=熄屏断电、null=拿不到（模块不支持）。
 * 语义详见 screenStateName() 的注释。
 *
 * 为什么看门狗需要它：息屏后 gst 的 vsink 不再消费帧 → native 侧 videoStallMs 持续
 * 增长，而音频进程独立于平面仍在写 → "视频 8s 无新帧但音频在播" 判定成立，看门狗
 * 误以为管线卡死而 seek 重启（把本可自愈的会话杀掉重建）。真机表现就是用户说的
 * "点一次只亮屏不出画、要点第二次"。息屏期间画面停滞是**正常现象**，不是故障。 */
export function getScreenState() {
  if (!keepBackend) probeSync();
  if (!keepBackend) {
    if (!keepBackendProbed) probeAsync();
    return null;
  }
  return readState();
}

// 保活核心：可注入时钟与调用，便于 Node 单测（不依赖真机模块）
//   opts.call()        续期动作（默认 keepScreenOnce）
//   opts.intervalMs    周期（默认 KEEP_INTERVAL_MS）
//   opts.setTimer/clearTimer  定时器实现（默认 setTimeout 链，避免 setInterval 堆叠）
//   opts.immediate     启动时是否立刻续一次（默认 true）
export function createScreenKeeper(opts) {
  const o = opts || {};
  const call = typeof o.call === 'function' ? o.call : keepScreenOnce;
  const intervalMs = Math.max(1000, Math.round(Number(o.intervalMs) || KEEP_INTERVAL_MS));
  /* 后端探测是异步的（import('brightness')）：第一次 tick 往往还没探完 → call() 返回 false。
   * 若此时仍等满 20s 才重试，就留下一个"最多 20s 没人保活"的窗口，正好可能跨过
   * bright_time 的调暗阈值（真机实测：起播 13s 后亮度掉到 5 才被后续 tick 救回来）。
   * 所以未成功时用短间隔快速重试，直到后端就绪。 */
  const retryMs = Math.max(200, Math.round(Number(o.retryMs) || 1000));
  const setT = typeof o.setTimer === 'function' ? o.setTimer : (fn, ms) => setTimeout(fn, ms);
  const clearT = typeof o.clearTimer === 'function' ? o.clearTimer : (t) => clearTimeout(t);
  let timer = null;
  let active = false;
  let ticks = 0;
  let misses = 0;
  const tick = () => {
    if (!active) return;
    ticks++;
    const sent = call() !== false; // 只有明确的 false（后端未就绪/全部失败）才算未发出
    misses = sent ? 0 : misses + 1;
    // 用 setTimeout 链而不是 setInterval：单次续期若被系统拖慢，也不会堆积回调
    timer = setT(tick, sent ? intervalMs : retryMs);
  };
  return {
    start() {
      if (active) return false;
      active = true;
      ticks = 0;
      misses = 0;
      if (o.immediate === false) {
        timer = setT(tick, intervalMs);
      } else {
        // 立刻续一次；未发出（后端还在异步探测）则用短间隔快速重试
        const sent = call() !== false;
        misses = sent ? 0 : 1;
        timer = setT(tick, sent ? intervalMs : retryMs);
      }
      return true;
    },
    stop() {
      if (!active) return false;
      active = false;
      if (timer != null) clearT(timer);
      timer = null;
      return true;
    },
    isActive() {
      return active;
    },
    ticks() {
      return ticks;
    },
    misses() {
      return misses;
    },
    intervalMs() {
      return intervalMs;
    }
  };
}

let keeper = null;

// 进入播放态（播放/暂停都算）→ 启动 20s 周期保活；重复调用幂等
export function keepScreenOn() {
  if (!keeper) keeper = createScreenKeeper({});
  /* #N 按会话重置：每轮保活都从 #1 开始。两个会话就是两条独立的 #1..#N 序列，
   * 若看到交错的两条序列（例如两个 #3），就说明有并行定时器 —— 泄漏证据。
   * 必须在 start() 之前重置：start() 在 immediate 模式下会同步调一次 call()。 */
  const wasActive = keeper.isActive();
  if (!wasActive) keepTicks = 0;
  const started = keeper.start();
  // 每次真正启动都打一行：两次 onShow 之间必须能看到 "ticks=0" 重启。
  if (started) {
    logLine(
      '保活启动 interval=' + KEEP_INTERVAL_MS + 'ms backend=' + (keepScreenBackend() || '探测中') + ' ticks=0'
    );
  }
  return keeper.isActive();
}

// 离开播放页/切后台 → 停止（定时器必须清干净，onUnload 后不能再跑）
export function keepScreenOff() {
  if (!keeper) return false;
  const t = keeper.ticks();
  const stopped = keeper.stop();
  if (stopped) {
    // 成对 API：不停掉持续亮屏会让系统一直亮屏（烧屏风险）
    if (keepBackend && typeof keepBackend.stopAlways === 'function') {
      try {
        keepBackend.stopAlways();
      } catch (e) {
        logLine('stopAlwaysScreenOn 抛错 ' + errText(e));
      }
    }
    alwaysArmed = false; // 下次 onShow 重新武装 startAlwaysScreenOn
    logLine('保活停止 ticks=' + t + '（定时器已清，后台不再保活）');
  }
  return stopped;
}

// 当前是否在保活（页面据此避免重复启动）
export function isKeepingScreen() {
  return !!(keeper && keeper.isActive());
}

// 累计续期次数（诊断用：两次进出播放态后应回到 0 重新计数）
export function keepScreenTicks() {
  return keeper ? keeper.ticks() : 0;
}

// 物理矩形是否落在面板内（native 侧入参校验的前置检查）
export function isRectInsidePanel(rect, panel) {
  const p = panel || PHYSICAL;
  if (!rect) return false;
  const x = Math.round(rect.x);
  const y = Math.round(rect.y);
  const w = Math.round(rect.width);
  const h = Math.round(rect.height);
  if (w <= 0 || h <= 0) return false;
  if (x < 0 || y < 0) return false;
  if (x + w > p.width || y + h > p.height) return false;
  return true;
}
