#!/usr/bin/env node
/* player.js 的离线自检。
 *
 * 为什么值得写：player.js 是 UI 和 native 之间唯一的缝。它出错的方式都很安静——
 * rect 传 NaN 导致 KMS 平面变 0x0、stop 抛异常导致 onUnload 后面的清理全跳过、
 * touch 参数顺序写错导致手势静默丢失。这些在真机上都表现为「画面不出现」或
 * 「点了没反应」，非常难定位。所以这里用一个假的 ysp layer 模块把每条路径都跑一遍。
 *
 * 跑法：node PenYS/tools/test_player.js
 */

import { strict as assert } from 'node:assert';

// ---- 假的 native 模块 -------------------------------------------------------
// 状态机：idle → connecting → playing → paused → idle
const fake = {
  state: 'idle',
  calls: [],
  failStart: false,
  failStop: false,
  // start 连续被轮询 N 次后才变成 playing，用来验证「轮询到 playing」而不是
  // 「start 返回即成功」
  playingAfterPolls: 2,

  start(sdkParam, x, y, w, h) {
    this.calls.push(['start', sdkParam, x, y, w, h]);
    if (this.failStart) return { ok: false, code: 'E_TEST', error: 'forced' };
    this.state = 'connecting';
    this._polls = 0;
    return { ok: true };
  },
  stop() {
    this.calls.push(['stop']);
    if (this.failStop) return { ok: false, code: 'E_STOP' };
    this.state = 'idle';
    return { ok: true };
  },
  pause() { this.calls.push(['pause']); this.state = 'paused'; return { ok: true }; },
  resume() { this.calls.push(['resume']); this.state = 'playing'; return { ok: true }; },
  setRect(x, y, w, h) { this.calls.push(['setRect', x, y, w, h]); return { ok: true }; },
  touch(nx, ny, action, fingerId) {
    this.calls.push(['touch', nx, ny, action, fingerId]);
    return { ok: true };
  },
  status() {
    if (this.state === 'connecting') {
      this._polls = (this._polls || 0) + 1;
      if (this._polls >= this.playingAfterPolls) this.state = 'playing';
    }
    return {
      ok: true,
      state: this.state,
      fps: this.state === 'playing' ? 60 : 0,
      latencyMs: 42,
      bitrateKbps: 9000,
      audioOk: this.state === 'playing',
      planeActive: this.state === 'playing',
    };
  },
  info() { return { ok: true, version: 'test' }; },
};

globalThis.require = (name) => {
  if (name === 'ysplayer') return { default: fake };
  throw new Error('unknown module ' + name);
};

const player = await import('../src/services/player.js');

const results = [];
function check(name, fn) {
  try { fn(); results.push(['PASS', name]); }
  catch (e) { results.push(['FAIL', name + ': ' + e.message]); }
}

// ---- 用例 -------------------------------------------------------------------

check('模块可加载', () => {
  assert.equal(player.isAvailable(), true);
  assert.equal(player.unavailableReason(), '');
});

check('status 归一化', () => {
  fake.state = 'idle';
  const s = player.status();
  assert.equal(s.state, 'idle');
  assert.equal(typeof s.fps, 'number');
  assert.equal(s.lastError, '');
});

check('start 拒绝非法 rect', async () => {
  fake.state = 'idle';
  for (const r of [null, {}, { x: 0, y: 0, w: 0, h: 100 }, { x: NaN, y: 0, w: 10, h: 10 }]) {
    const res = await player.start({ sdkParam: 'x'.repeat(64), rect: r });
    assert.equal(res.ok, false, 'rect 应当被拒绝: ' + JSON.stringify(r));
    assert.equal(res.code, 'BAD_RECT');
  }
});

check('start 拒绝过短 sdkParam', async () => {
  const res = await player.start({ sdkParam: 'short', rect: { x: 0, y: 0, w: 100, h: 100 } });
  assert.equal(res.ok, false);
  assert.equal(res.code, 'BAD_SDK_PARAM');
});

check('start 轮询到 playing 才返回', async () => {
  fake.state = 'idle';
  fake.failStart = false;
  fake.playingAfterPolls = 3;
  fake.calls.length = 0;
  const res = await player.start({
    sdkParam: 'A'.repeat(128),
    rect: { x: 12.6, y: 0.2, w: 427, h: 240 },
  });
  assert.equal(res.ok, true, '应当成功: ' + JSON.stringify(res));
  assert.equal(fake.state, 'playing');
  const startCall = fake.calls.find((c) => c[0] === 'start');
  // rect 必须是取整后的整数，否则 KMS dst rect 会有亚像素抖动
  assert.deepEqual(startCall.slice(2), [13, 0, 427, 240], 'rect 未取整: ' + JSON.stringify(startCall));
});

check('start 失败直接返回错误码', async () => {
  fake.state = 'idle';
  fake.failStart = true;
  const res = await player.start({ sdkParam: 'A'.repeat(128), rect: { x: 0, y: 0, w: 100, h: 100 } });
  assert.equal(res.ok, false);
  assert.equal(res.code, 'E_TEST');
  fake.failStart = false;
});

check('pause / resume', () => {
  fake.state = 'playing';
  assert.equal(player.pause().ok, true);
  assert.equal(player.status().state, 'paused');
  assert.equal(player.resume().ok, true);
  assert.equal(player.status().state, 'playing');
});

check('setRect 拒绝非法值', () => {
  assert.equal(player.setRect({ x: 0, y: 0, w: -1, h: 10 }).code, 'BAD_RECT');
  assert.equal(player.setRect({ x: 0, y: 0, w: 10, h: 10 }).ok, true);
});

check('touch 两种调用形态等价', () => {
  fake.calls.length = 0;
  const a = player.touch({ nx: 0.25, ny: 0.75, action: 'down', fingerId: 2 });
  const b = player.touch(0.25, 0.75, 'down', 2);
  assert.equal(a.ok, true);
  assert.equal(b.ok, true);
  const ta = fake.calls.filter((c) => c[0] === 'touch');
  assert.deepEqual(ta[0], ta[1], '两种形态传下去的参数不一致: ' + JSON.stringify(ta));
  assert.deepEqual(ta[0].slice(1), [0.25, 0.75, 0, 2], 'action 未映射成数字: ' + JSON.stringify(ta[0]));
});

check('touch 拒绝未知 action 与非数字坐标', () => {
  assert.equal(player.touch(0.1, 0.1, 'cancel?', 0).code, 'BAD_ACTION');
  assert.equal(player.touch('a', 0.1, 'down', 0).code, 'BAD_COORD');
});

check('touch 不做钳位（交给 native）', () => {
  fake.calls.length = 0;
  player.touch(1.8, -0.4, 'move', 0);
  const t = fake.calls.find((c) => c[0] === 'touch');
  assert.deepEqual(t.slice(1), [1.8, -0.4, 2, 0], 'JS 侧不应钳位: ' + JSON.stringify(t));
});

check('stop 等到 idle 才返回', async () => {
  fake.state = 'playing';
  const res = await player.stop();
  assert.equal(res.ok, true);
  assert.equal(fake.state, 'idle');
});

check('stop 幂等：重复调用不抛异常', async () => {
  const r1 = await player.stop();
  const r2 = await player.stop();
  assert.equal(r1.ok, true);
  assert.equal(r2.ok, true);
});

check('stop 失败时返回 ok:false 而不是抛异常', async () => {
  fake.state = 'playing';
  fake.failStop = true;
  const res = await player.stop();
  assert.equal(res.ok, false);
  assert.equal(res.code, 'E_STOP');
  fake.failStop = false;
  fake.state = 'idle';
});

// ---- 汇总 -------------------------------------------------------------------

let failed = 0;
for (const [st, name] of results) {
  if (st === 'FAIL') failed++;
  console.log((st === 'PASS' ? '  ok  ' : ' FAIL ') + name);
}
console.log('');
console.log(`${results.length - failed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
