// 纯 JS 二维码编码器（QuickJS 无 DOM/canvas，登录二维码用 div 网格渲染）
//
// 范围：byte 模式（URL），纠错 M，版本 1..10（17+4v ≤ 57），掩码可指定（默认按惩罚选优）。
// 正确性锚点：api-mock/fixtures/qr_reference.json（npm qrcode 库 ECC-M、maskPattern=0 的
// 逐位参考矩阵），test/run.js 固定 version+mask 对拍 —— 任何位差都会被抓到。
//
// 结构（ISO/IEC 18004）：
//   data = mode(0100) + count(8bit, v1-9) + bytes + terminator + pad(0xEC/0x11)
//   RS 纠错 → 分块交织 → 功能图形(finder/separator/timing/alignment/dark/format/version)
//   → 掩码(跳功能图形) → format/version info BCH

// ---- 版本表（v1..v10）----
// 每块纠错码字数（ECC_M）
const ECC_M_PER_BLOCK = [0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26];
// [组1块数, 组1数据码字, 组2块数, 组2数据码字]（ECC_M）
const BLOCKS_M = [
  null,
  [1, 16, 0, 0],
  [1, 28, 0, 0],
  [1, 44, 0, 0],
  [2, 32, 0, 0],
  [2, 43, 0, 0],
  [4, 27, 0, 0],
  [4, 31, 0, 0],
  [2, 38, 2, 39],
  [3, 36, 2, 37],
  [4, 43, 1, 44]
];
// 对齐图形中心坐标（v1 无）
const ALIGN_CENTERS = [
  null,
  [],
  [6, 18],
  [6, 22],
  [6, 26],
  [6, 30],
  [6, 34],
  [6, 22, 38],
  [6, 24, 42],
  [6, 26, 46],
  [6, 28, 50]
];

function dataCodewords(ver) {
  const b = BLOCKS_M[ver];
  return b[0] * b[1] + b[2] * b[3];
}

// byte 模式容量（mode 4bit + count 8bit 占 12bit → (数据码字*8 - 12) / 8）
export function byteCapacity(ver) {
  return dataCodewords(ver) - 2; // 12bit 头 + 最多4bit terminator ≈ 2 字节
}

export function pickVersion(byteLen) {
  for (let v = 1; v <= 10; v++) {
    if (byteCapacity(v) >= byteLen) return v;
  }
  return 0; // 超容量
}

// ---- GF(256)（本原多项式 0x11d）----
const EXP = new Array(512);
const LOG = new Array(256);
(function initGf() {
  let x = 1;
  for (let i = 0; i < 255; i++) {
    EXP[i] = x;
    LOG[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11d;
  }
  for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
})();

function gfMul(a, b) {
  if (a === 0 || b === 0) return 0;
  return EXP[LOG[a] + LOG[b]];
}

// RS 生成多项式 g(x) = Π (x - α^i)
function rsGenerator(degree) {
  let poly = [1];
  for (let i = 0; i < degree; i++) {
    const next = new Array(poly.length + 1).fill(0);
    for (let j = 0; j < poly.length; j++) {
      // poly*(x + α^i)：×x 落 next[j]，×α^i 落 next[j+1]（索引0=最高次）
      next[j] ^= poly[j];
      next[j + 1] ^= gfMul(poly[j], EXP[i]);
    }
    poly = next;
  }
  return poly;
}

function rsEncode(dataBytes, eccLen) {
  const gen = rsGenerator(eccLen);
  const rem = new Array(eccLen).fill(0);
  for (let i = 0; i < dataBytes.length; i++) {
    const factor = dataBytes[i] ^ rem[0];
    rem.shift();
    rem.push(0);
    if (factor !== 0) {
      for (let j = 0; j < eccLen; j++) {
        rem[j] ^= gfMul(gen[j + 1], factor);
      }
    }
  }
  return rem;
}

// ---- 位缓冲 ----
function BitBuf() {
  this.bits = [];
}
BitBuf.prototype.put = function (val, len) {
  for (let i = len - 1; i >= 0; i--) this.bits.push((val >> i) & 1);
};
BitBuf.prototype.toBytes = function () {
  const out = [];
  for (let i = 0; i < this.bits.length; i += 8) {
    let b = 0;
    for (let j = 0; j < 8; j++) b = (b << 1) | (this.bits[i + j] || 0);
    out.push(b);
  }
  return out;
};

// ---- 数据段构造 + 纠错 + 交织 ----
export function buildCodewords(bytes, ver) {
  const bb = new BitBuf();
  bb.put(0b0100, 4); // byte mode
  bb.put(bytes.length, 8); // v1-9 用 8bit；v10-26 用 16bit（本实现 v≤9 用8，v10 也按8→错？）
  // ISO：count bits = 8 (v1-9) / 16 (v10-26)
  if (ver >= 10) {
    // 修正：v10 需 16bit —— 重新构造
    const bb2 = new BitBuf();
    bb2.put(0b0100, 4);
    bb2.put(bytes.length, 16);
    for (let i = 0; i < bytes.length; i++) bb2.put(bytes[i], 8);
    return finalizeBits(bb2, ver);
  }
  for (let i = 0; i < bytes.length; i++) bb.put(bytes[i], 8);
  return finalizeBits(bb, ver);
}

function finalizeBits(bb, ver) {
  const total = dataCodewords(ver);
  // terminator（最多 4 个 0）+ 补齐到字节 + pad
  const remainBits = total * 8 - bb.bits.length;
  bb.put(0, Math.min(4, Math.max(0, remainBits)));
  while (bb.bits.length % 8 !== 0) bb.bits.push(0);
  const data = bb.toBytes();
  let pad = 0xec;
  while (data.length < total) {
    data.push(pad);
    pad = pad === 0xec ? 0x11 : 0xec;
  }

  // 分块
  const spec = BLOCKS_M[ver];
  const eccLen = ECC_M_PER_BLOCK[ver];
  const blocks = [];
  let off = 0;
  for (let g = 0; g < 2; g++) {
    const count = g === 0 ? spec[0] : spec[2];
    const dlen = g === 0 ? spec[1] : spec[3];
    for (let i = 0; i < count; i++) {
      const d = data.slice(off, off + dlen);
      off += dlen;
      blocks.push({ data: d, ecc: rsEncode(d, eccLen) });
    }
  }
  // 交织：数据按列、纠错按列
  const out = [];
  const maxData = Math.max(spec[1], spec[3]);
  for (let i = 0; i < maxData; i++) {
    for (let b = 0; b < blocks.length; b++) {
      if (i < blocks[b].data.length) out.push(blocks[b].data[i]);
    }
  }
  for (let i = 0; i < eccLen; i++) {
    for (let b = 0; b < blocks.length; b++) out.push(blocks[b].ecc[i]);
  }
  return out;
}

// ---- 功能图形 ----
function makeGrid(size) {
  const g = [];
  for (let i = 0; i < size; i++) g.push(new Array(size).fill(0));
  return g;
}
function makeReserved(size) {
  const r = [];
  for (let i = 0; i < size; i++) r.push(new Array(size).fill(false));
  return r;
}

function drawFinder(grid, res, row, col) {
  for (let r = -1; r <= 7; r++) {
    for (let c = -1; c <= 7; c++) {
      const rr = row + r;
      const cc = col + c;
      if (rr < 0 || cc < 0 || rr >= grid.length || cc >= grid.length) continue;
      const on =
        r >= 0 && r <= 6 && (c === 0 || c === 6) ||
        c >= 0 && c <= 6 && (r === 0 || r === 6) ||
        r >= 2 && r <= 4 && c >= 2 && c <= 4;
      grid[rr][cc] = on ? 1 : 0;
      res[rr][cc] = true;
    }
  }
}

function drawAlignment(grid, res, ver) {
  const centers = ALIGN_CENTERS[ver];
  for (let i = 0; i < centers.length; i++) {
    for (let j = 0; j < centers.length; j++) {
      const cr = centers[i];
      const cc = centers[j];
      // 跳过与 finder 重叠的三个角
      if ((cr <= 8 && cc <= 8) || (cr <= 8 && cc >= grid.length - 9) || (cr >= grid.length - 9 && cc <= 8)) continue;
      for (let r = -2; r <= 2; r++) {
        for (let c = -2; c <= 2; c++) {
          const on = Math.max(Math.abs(r), Math.abs(c)) !== 1;
          grid[cr + r][cc + c] = on ? 1 : 0;
          res[cr + r][cc + c] = true;
        }
      }
    }
  }
}

function drawTiming(grid, res) {
  const size = grid.length;
  for (let i = 8; i < size - 8; i++) {
    const on = i % 2 === 0;
    grid[6][i] = on ? 1 : 0;
    res[6][i] = true;
    grid[i][6] = on ? 1 : 0;
    res[i][6] = true;
  }
}

// format info：BCH(15,5)，ecc M=0b00；两处放置
function formatBits(mask) {
  const data = (0b00 << 3) | mask; // M
  let d = data << 10;
  for (let i = 4; i >= 0; i--) {
    if (d & (1 << (i + 10))) d ^= 0x537 << i; // G(x)=10100110111
  }
  const bits = ((data << 10) | d) ^ 0x5412;
  return bits; // 15 bit
}

function drawFormat(grid, res, mask) {
  const bits = formatBits(mask);
  const size = grid.length;
  const get = (i) => (bits >> i) & 1;
  // 第一份（左上）：bit0-5 沿左侧竖列 col8→rows0-5，bit6=(7,8)，bit7=(8,8)，
  //                bit8=(8,7)，bit9-14 沿顶部横行 row8→cols5-0
  // （坐标：grid[row][col]；bit0 = LSB）
  for (let i = 0; i <= 5; i++) grid[i][8] = get(i);
  grid[7][8] = get(6);
  grid[8][8] = get(7);
  grid[8][7] = get(8);
  for (let i = 9; i <= 14; i++) grid[8][14 - i] = get(i);
  // 第二份：bit0-7 沿顶部横行 row8→右侧 cols size-1..size-8；
  //         bit8-14 沿右侧竖列 col8→底部 rows size-7..size-1
  for (let i = 0; i <= 7; i++) grid[8][size - 1 - i] = get(i);
  for (let i = 8; i <= 14; i++) grid[size - 15 + i][8] = get(i);
  grid[size - 8][8] = 1; // dark module
  for (let i = 0; i <= 5; i++) res[i][8] = true;
  res[7][8] = true;
  res[8][8] = true;
  res[8][7] = true;
  for (let i = 9; i <= 14; i++) res[8][14 - i] = true;
  for (let i = 0; i <= 7; i++) res[8][size - 1 - i] = true;
  for (let i = 8; i <= 14; i++) res[size - 15 + i][8] = true;
}

// version info（v≥7）：BCH(18,6)
function versionBits(ver) {
  let d = ver << 12;
  for (let i = 5; i >= 0; i--) {
    if (d & (1 << (i + 12))) d ^= 0x1f25 << i;
  }
  return ((ver << 12) | d) & 0x3ffff;
}

function drawVersion(grid, res, ver) {
  if (ver < 7) return;
  const bits = versionBits(ver);
  const size = grid.length;
  const get = (i) => (bits >> i) & 1;
  for (let i = 0; i < 18; i++) {
    const r = Math.floor(i / 3);
    const c = i % 3;
    const v = get(i);
    grid[size - 11 + c][r] = v;
    grid[r][size - 11 + c] = v;
    res[size - 11 + c][r] = true;
    res[r][size - 11 + c] = true;
  }
}

// ---- 数据放置（右下起 zigzag 双列，跳过功能图形/时序第6列）----
function placeData(grid, res, codewords) {
  const size = grid.length;
  let bitIdx = 0;
  const totalBits = codewords.length * 8;
  let upward = true;
  for (let col = size - 1; col > 0; col -= 2) {
    if (col === 6) col = 5; // 跳过时序列
    for (let n = 0; n < size; n++) {
      const row = upward ? size - 1 - n : n;
      for (let k = 0; k < 2; k++) {
        const c = col - k;
        if (res[row][c]) continue;
        let bit = 0;
        if (bitIdx < totalBits) {
          bit = (codewords[bitIdx >> 3] >> (7 - (bitIdx & 7))) & 1;
          bitIdx++;
        }
        grid[row][c] = bit;
      }
    }
    upward = !upward;
  }
}

// ---- 掩码（仅非功能模块）----
function maskFn(m, i, j) {
  switch (m) {
    case 0: return (i + j) % 2 === 0;
    case 1: return i % 2 === 0;
    case 2: return j % 3 === 0;
    case 3: return (i + j) % 3 === 0;
    case 4: return (Math.floor(i / 2) + Math.floor(j / 3)) % 2 === 0;
    case 5: return ((i * j) % 2) + ((i * j) % 3) === 0;
    case 6: return (((i * j) % 2) + ((i * j) % 3)) % 2 === 0;
    default: return (((i + j) % 2) + ((i * j) % 3)) % 2 === 0;
  }
}

function applyMask(grid, res, mask) {
  const size = grid.length;
  for (let i = 0; i < size; i++) {
    for (let j = 0; j < size; j++) {
      if (res[i][j]) continue;
      if (maskFn(mask, i, j)) grid[i][j] ^= 1;
    }
  }
}

// 惩罚评分（4 规则）
function penalty(grid) {
  const size = grid.length;
  let score = 0;
  // 规则1：同色 run ≥5
  for (let i = 0; i < size; i++) {
    for (const dir of [0, 1]) {
      let run = 1;
      for (let j = 1; j < size; j++) {
        const cur = dir === 0 ? grid[i][j] : grid[j][i];
        const prev = dir === 0 ? grid[i][j - 1] : grid[j - 1][i];
        if (cur === prev) {
          run++;
          if (j === size - 1 && run >= 5) score += 3 + (run - 5);
        } else {
          if (run >= 5) score += 3 + (run - 5);
          run = 1;
        }
      }
    }
  }
  // 规则2：2x2 同色
  for (let i = 0; i < size - 1; i++) {
    for (let j = 0; j < size - 1; j++) {
      const v = grid[i][j];
      if (v === grid[i][j + 1] && v === grid[i + 1][j] && v === grid[i + 1][j + 1]) score += 3;
    }
  }
  // 规则3：1:1:3:1:1 模式（1011101 + 0000）
  const pat1 = [1, 0, 1, 1, 1, 0, 1];
  const zeros = [0, 0, 0, 0];
  for (let i = 0; i < size; i++) {
    for (const dir of [0, 1]) {
      const at = (k) => (dir === 0 ? grid[i][k] : grid[k][i]);
      for (let j = 0; j + 11 <= size; j++) {
        let m1 = true;
        for (let k = 0; k < 7; k++) if (at(j + k) !== pat1[k]) { m1 = false; break; }
        if (!m1) continue;
        const before = j >= 4 && at(j - 1) === 0 && at(j - 2) === 0 && at(j - 3) === 0 && at(j - 4) === 0;
        const after = j + 11 <= size && at(j + 7) === 0 && at(j + 8) === 0 && at(j + 9) === 0 && at(j + 10) === 0;
        if (before || after) score += 40;
      }
    }
  }
  void zeros;
  // 规则4：暗模块比例
  let dark = 0;
  for (let i = 0; i < size; i++) for (let j = 0; j < size; j++) dark += grid[i][j];
  const pct = (dark * 100) / (size * size);
  score += Math.floor(Math.abs(pct - 50) / 5) * 10;
  return score;
}

// ---- 主入口 ----
// opts: { version?: number(1..10), mask?: number(0..7) }（均不给则自动选版+掩码选优）

// 功能图形 + 预留区（encodeQr 与反解诊断共用，保证 reserved 完全一致）
export function buildFunctionPattern(ver) {
  const size = 17 + 4 * ver;
  const grid = makeGrid(size);
  const res = makeReserved(size);
  drawFinder(grid, res, 0, 0);
  drawFinder(grid, res, size - 7, 0);
  drawFinder(grid, res, 0, size - 7);
  drawTiming(grid, res);
  drawAlignment(grid, res, ver);
  drawFormat(grid, res, 0); // 占位（选定掩码后会重写）
  drawVersion(grid, res, ver);
  return { grid: grid, res: res, size: size };
}

export function encodeQr(text, opts) {
  const o = opts || {};
  const bytes = utf8Bytes(String(text == null ? '' : text));
  const ver = o.version || pickVersion(bytes.length);
  if (!ver) return { ok: false, reason: '内容超容量(' + bytes.length + 'B)' };
  const cw = buildCodewords(bytes, ver);
  const size = 17 + 4 * ver;

  const fn = buildFunctionPattern(ver);
  const base = fn.grid;
  const res = fn.res;

  placeData(base, res, cw);

  // 选定掩码
  let mask = typeof o.mask === 'number' ? o.mask : -1;
  let best = null;
  const candidates = mask >= 0 ? [mask] : [0, 1, 2, 3, 4, 5, 6, 7];
  let bestScore = Infinity;
  for (const m of candidates) {
    const g = base.map((row) => row.slice());
    applyMask(g, res, m);
    drawFormat(g, res, m); // format 位随掩码变化
    const sc = penalty(g);
    if (sc < bestScore) {
      bestScore = sc;
      best = g;
    }
  }
  return { ok: true, size: size, version: ver, mask: candidates.length === 1 ? candidates[0] : undefined, grid: best };
}

// 行 → run-length（渲染用）：[{ y, runs: [[x,len], ...] }]，只含暗段
export function rowRuns(grid) {
  const out = [];
  for (let y = 0; y < grid.length; y++) {
    const runs = [];
    let x = 0;
    while (x < grid.length) {
      if (grid[y][x]) {
        let len = 1;
        while (x + len < grid.length && grid[y][x + len]) len++;
        runs.push([x, len]);
        x += len;
      } else x++;
    }
    out.push(runs);
  }
  return out;
}

function utf8Bytes(str) {
  const out = [];
  for (let i = 0; i < str.length; i++) {
    let c = str.charCodeAt(i);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      const cp = 0x10000 + ((c - 0xd800) << 10) + (str.charCodeAt(i + 1) - 0xdc00);
      i++;
      out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    } else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
  }
  return out;
}
