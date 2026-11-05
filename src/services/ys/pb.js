/* protobuf 零依赖解码器：只做「读」，不做「写」。
 *
 * 用途：把 paasDispatch 拿回来的 sdk_param（base64 -> proto.paas.SdkStartGameParams）
 * 解成对象，喂给 native 的 session。
 *
 * 三处 QuickJS 适配（都是踩过才知道的）：
 *   1. 不用 Buffer / BigInt。package.json 里 quickjs.bigNum=false，没有 BigInt；
 *      varint 用浮点算（QuickJS 的 number 是 double，53 位以内精确，
 *      protobuf 的 uint32 字段完全够用）。
 *   2. 自己实现 base64 -> Uint8Array。不能走 native 的 b64Decode：
 *      native 用 JS_NewStringLen 返回二进制，QuickJS 会按 UTF-8 解释，
 *      0x80~0xFF 的字节会被 mangle，长度和内容都不可信。
 *   3. 递归深度上限 6，避免畸形数据把栈打爆。
 */

const NAMES = {
  1: 'sid',
  2: 'ca_id',
  3: 'pod_id',
  4: 'game_token',
  5: 'resolution',
  6: 'target_fps',
  7: 'cmd_line',
  8: 'game_svr_addr',
  9: 'rtc_udp_port',
  10: 'additional_game_svr_addrs',
  11: 'game_server_wss_url',
  12: 'additional_game_server_wss_urls',
  13: 'big_isp',
  14: 'game_control_channel_url',
  15: 'additional_game_control_channel_urls',
  16: 'is_ipv6',
  17: 'game_svr_addr_ipv6',
  18: 'rtc_udp_port_ipv6',
  19: 'additional_game_svr_addrs_ipv6',
  20: 'game_server_wss_url_ipv6',
  21: 'additional_game_server_wss_urls_ipv6',
  22: 'game_control_channel_url_ipv6'
};

const B64CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
var B64MAP = null;

function b64Map() {
  if (B64MAP) return B64MAP;
  B64MAP = {};
  for (var i = 0; i < B64CHARS.length; i++) B64MAP[B64CHARS.charAt(i)] = i;
  return B64MAP;
}

/* base64 -> Uint8Array。容忍 -_ 变体、空白、缺 padding。 */
export function b64ToBytes(b64) {
  if (!b64 || typeof b64 !== 'string') return new Uint8Array(0);
  var map = b64Map();
  var out = [];
  var buf = 0;
  var bits = 0;
  for (var i = 0; i < b64.length; i++) {
    var ch = b64.charAt(i);
    if (ch === '=') break;
    var v = map[ch];
    if (v === undefined) {
      if (ch === '-') v = 62;
      else if (ch === '_') v = 63;
      else continue;            /* 空白、换行、非法字符一并跳过 */
    }
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push((buf >> bits) & 0xff);
    }
  }
  var arr = new Uint8Array(out.length);
  for (var j = 0; j < out.length; j++) arr[j] = out[j];
  return arr;
}

export function bytesToB64(bytes) {
  var map = b64Map();
  var out = '';
  var i = 0;
  for (; i + 2 < bytes.length; i += 3) {
    var n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += B64CHARS.charAt((n >> 18) & 63) + B64CHARS.charAt((n >> 12) & 63) +
           B64CHARS.charAt((n >> 6) & 63) + B64CHARS.charAt(n & 63);
  }
  var rem = bytes.length - i;
  if (rem === 1) {
    var n1 = bytes[i] << 16;
    out += B64CHARS.charAt((n1 >> 18) & 63) + B64CHARS.charAt((n1 >> 12) & 63) + '==';
  } else if (rem === 2) {
    var n2 = (bytes[i] << 16) | (bytes[i + 1] << 8);
    out += B64CHARS.charAt((n2 >> 18) & 63) + B64CHARS.charAt((n2 >> 12) & 63) +
           B64CHARS.charAt((n2 >> 6) & 63) + '=';
  }
  return out;
}

/* varint 读取。返回 {value, pos}，越界时 pos = -1。 */
export function readVarint(bytes, pos) {
  var result = 0;
  var shift = 0;
  while (pos < bytes.length) {
    var b = bytes[pos++];
    result += (b & 0x7f) * Math.pow(2, shift);
    if ((b & 0x80) === 0) return { value: result, pos: pos };
    shift += 7;
    if (shift > 63) return { value: 0, pos: -1 };
  }
  return { value: 0, pos: -1 };
}

/* 判断一段字节像不像 UTF-8 文本。length-delimited 字段既可能是 string
 * 也可能是嵌套 message，没有 schema 只能猜。启发式：允许 \t \n \r，
 * 禁止其他控制字符与 0x7f~0xa0。 */
function looksLikeText(bytes) {
  if (bytes.length === 0) return true;
  for (var i = 0; i < bytes.length; i++) {
    var b = bytes[i];
    if (b === 9 || b === 10 || b === 13) continue;
    if (b < 0x20) return false;
    if (b >= 0x7f && b <= 0xa0) return false;
  }
  return true;
}

/* UTF-8 字节 -> JS 字符串（QuickJS 没有 TextDecoder）。 */
export function utf8Decode(bytes) {
  var out = '';
  var i = 0;
  while (i < bytes.length) {
    var b0 = bytes[i];
    if (b0 < 0x80) {
      out += String.fromCharCode(b0);
      i += 1;
    } else if ((b0 & 0xe0) === 0xc0 && i + 1 < bytes.length) {
      var b1 = bytes[i + 1];
      out += String.fromCharCode(((b0 & 0x1f) << 6) | (b1 & 0x3f));
      i += 2;
    } else if ((b0 & 0xf0) === 0xe0 && i + 2 < bytes.length) {
      var c1 = bytes[i + 1], c2 = bytes[i + 2];
      out += String.fromCharCode(((b0 & 0x0f) << 12) | ((c1 & 0x3f) << 6) | (c2 & 0x3f));
      i += 3;
    } else if ((b0 & 0xf8) === 0xf0 && i + 3 < bytes.length) {
      var d1 = bytes[i + 1], d2 = bytes[i + 2], d3 = bytes[i + 3];
      var cp = ((b0 & 0x07) << 18) | ((d1 & 0x3f) << 12) | ((d2 & 0x3f) << 6) | (d3 & 0x3f);
      cp -= 0x10000;
      out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
      i += 4;
    } else {
      out += '?';
      i += 1;
    }
  }
  return out;
}

export function utf8Encode(str) {
  var out = [];
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i);
    if (c < 0x80) {
      out.push(c);
    } else if (c < 0x800) {
      out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      var lo = str.charCodeAt(i + 1);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        var cp2 = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
        out.push(0xf0 | (cp2 >> 18), 0x80 | ((cp2 >> 12) & 0x3f),
                 0x80 | ((cp2 >> 6) & 0x3f), 0x80 | (cp2 & 0x3f));
        i++;
      } else {
        out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
      }
    } else {
      out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    }
  }
  return new Uint8Array(out);
}

function toHex(bytes, limit) {
  var n = Math.min(bytes.length, limit || 16);
  var s = '';
  for (var i = 0; i < n; i++) {
    var h = bytes[i].toString(16);
    s += h.length < 2 ? '0' + h : h;
  }
  return s;
}

/* 通用解码：返回平坦的 [{field, wire, value, ...}]。
 * wire 0=varint 1=64bit 2=length-delimited 5=32bit */
export function decode(bytes, depth) {
  depth = depth || 0;
  var out = [];
  if (!bytes || typeof bytes.length !== 'number') return out;
  var pos = 0;
  var guard = 0;
  while (pos < bytes.length) {
    if (++guard > 100000) break;
    var tag = readVarint(bytes, pos);
    if (tag.pos < 0) break;
    pos = tag.pos;
    var field = Math.floor(tag.value / 8);
    var wire = tag.value % 8;
    if (field <= 0) break;

    if (wire === 0) {
      var v = readVarint(bytes, pos);
      if (v.pos < 0) break;
      pos = v.pos;
      out.push({ field: field, wire: 0, value: v.value, name: NAMES[field] });
    } else if (wire === 1) {
      if (pos + 8 > bytes.length) break;
      var s8 = bytes.subarray ? bytes.subarray(pos, pos + 8) : bytes.slice(pos, pos + 8);
      out.push({ field: field, wire: 1, value: toHex(s8, 8), name: NAMES[field] });
      pos += 8;
    } else if (wire === 5) {
      if (pos + 4 > bytes.length) break;
      var f0 = bytes[pos], f1 = bytes[pos + 1], f2 = bytes[pos + 2], f3 = bytes[pos + 3];
      var buf = new ArrayBuffer(4);
      var dv = new Uint8Array(buf);
      dv[0] = f0; dv[1] = f1; dv[2] = f2; dv[3] = f3;
      var f32 = new Float32Array(buf)[0];
      out.push({ field: field, wire: 5, value: f32, name: NAMES[field] });
      pos += 4;
    } else if (wire === 2) {
      var len = readVarint(bytes, pos);
      if (len.pos < 0) break;
      pos = len.pos;
      var n = len.value;
      if (n < 0 || pos + n > bytes.length) break;
      var slice = bytes.subarray ? bytes.subarray(pos, pos + n) : bytes.slice(pos, pos + n);
      pos += n;
      if (looksLikeText(slice)) {
        out.push({ field: field, wire: 2, value: utf8Decode(slice), name: NAMES[field] });
      } else if (depth < 6) {
        var sub = decode(slice, depth + 1);
        if (sub.length) {
          out.push({ field: field, wire: 2, value: sub, nested: true, name: NAMES[field] });
        } else {
          out.push({ field: field, wire: 2, value: toHex(slice, 32), name: NAMES[field] });
        }
      } else {
        out.push({ field: field, wire: 2, value: toHex(slice, 32), name: NAMES[field] });
      }
    } else {
      break;      /* 不认识的 wire type，停止而不是瞎猜 */
    }
  }
  return out;
}

/* sdk_param（base64）-> 扁平对象。repeated 字段收成数组。
 * 这是给 session.c 的输入契约：字段名就是 NAMES 里的 snake_case，
 * rtc_udp_port / target_fps / big_isp 是 number，其余是 string。 */
export function decodeSdkParam(b64) {
  var bytes = typeof b64 === 'string' ? b64ToBytes(b64) : b64;
  var fields = decode(bytes);
  var obj = {};
  for (var i = 0; i < fields.length; i++) {
    var f = fields[i];
    var name = f.name || ('field_' + f.field);
    var v = f.value;
    if (Object.prototype.hasOwnProperty.call(obj, name)) {
      if (Object.prototype.toString.call(obj[name]) === '[object Array]') obj[name].push(v);
      else obj[name] = [obj[name], v];
    } else {
      obj[name] = v;
    }
  }
  obj.__fields = fields;
  obj.__byteLength = bytes.length;
  return obj;
}

/* 文本视图，给日志用 */
export function format(bytes) {
  var fields = decode(bytes);
  var lines = [];
  for (var i = 0; i < fields.length; i++) {
    var f = fields[i];
    lines.push('  #' + f.field + ' ' + (f.name || '?') + ' = ' + f.value);
  }
  return lines.join('\n');
}

/* 从 sdk_param 里挑出「客户端真正要用的四个地址」，顺带修 pitfalls #15
 * （服务端偶尔把 control channel 的 URL 拼错，回退到 additional_* 列表）。 */
export function extractEndpoints(obj) {
  function asArray(v) {
    if (v === undefined || v === null || v === '') return [];
    if (Object.prototype.toString.call(v) === '[object Array]') return v;
    return [v];
  }
  function pickWss(main, extra) {
    var list = asArray(main).concat(asArray(extra));
    for (var i = 0; i < list.length; i++) {
      var s = String(list[i]);
      if (s.indexOf('wss://') === 0 && /^wss:\/\/[A-Za-z0-9._:/-]+$/.test(s)) return s;
    }
    return '';
  }
  var sig = pickWss(obj.game_server_wss_url, obj.additional_game_server_wss_urls);
  var ctl = pickWss(obj.game_control_channel_url, obj.additional_game_control_channel_urls);
  /* WSS 地址必须补 /rtc-sdk（pitfalls #11），但已经带路径的不要重复拼 */
  if (sig && sig.indexOf('/rtc-sdk') < 0 && sig.indexOf('/', 6) < 0) sig += '/rtc-sdk';
  if (ctl && ctl.indexOf('/rtc-sdk') < 0 && ctl.indexOf('/', 6) < 0) ctl += '/rtc-sdk';
  return {
    sid: String(obj.sid || ''),
    gameToken: String(obj.game_token || ''),
    gameSvrAddr: String(obj.game_svr_addr || ''),
    rtcUdpPort: typeof obj.rtc_udp_port === 'number' ? obj.rtc_udp_port : parseInt(obj.rtc_udp_port, 10) || 0,
    signalingUrl: sig,
    controlUrl: ctl,
    resolution: String(obj.resolution || ''),
    targetFps: typeof obj.target_fps === 'number' ? obj.target_fps : parseInt(obj.target_fps, 10) || 0
  };
}

export var FIELD_NAMES = NAMES;

export default {
  b64ToBytes: b64ToBytes,
  bytesToB64: bytesToB64,
  decode: decode,
  decodeSdkParam: decodeSdkParam,
  extractEndpoints: extractEndpoints,
  format: format,
  readVarint: readVarint,
  utf8Decode: utf8Decode,
  utf8Encode: utf8Encode,
  FIELD_NAMES: NAMES
};
