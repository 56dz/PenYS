# PenYS · 云·原神 词典笔客户端

有道词典笔 X6 Pro（RK3562 / Falcon mini-app）上的《云·原神》客户端。

> ⚠ **非官方客户端**。本项目通过逆向的米哈游云游戏协议接入，与米哈游官方无关。
> 使用存在账号风险与计费消耗，请自行评估。

设备只有 **960×266 的超宽逻辑画布**（物理 480×960 竖屏、DRM rotation=1），没有键盘、没有浏览器、
没有 `webrtcbin` / libnice / libsrtp —— 所以从 HTTP 协议、DTLS-SRTP、WSS 信令到视频上屏，
整个链路都是自己实现的。

---

## 项目现状（全部真机实测）

| 能力 | 结论 | 证据 |
|---|---|---|
| 米哈游 HTTP 接口直连 | ✅ | 真机 `POST passport createQRLogin` → `retcode:0` |
| 扫码登录协议链 | ✅ | `queryQRLoginStatus` 真机轮询，`status:"Created"` 形状已确认 |
| 六步调度（login→SpeedLimit→statusCheck→listPingServer→RTT→getNodesInfo） | ✅ | 未登录即可跑通，`fetchGears` 返回 5 档 `top=9000/4800/3200/1800/1200` |
| DTLS 1.2 + SRTP 密钥导出 | ✅ | 真机握手成功，协商 `SRTP_AES128_CM_SHA1_80`，E2E 22/22 |
| SRTP 加解密（RFC 3711 AES-CM + HMAC-SHA1-80） | ✅ | 真机 75/75；`protect` 7.80 µs/pkt，60fps 占单核 0.093%；零堆分配 |
| WSS 建连 + RTT 测速 | ✅ | 真机 101 升级，256 ms 建连 / 39 ms RTT |
| 硬解上屏 `mppvideodec ! kmssink` | ✅ | 真实 RTP 流上屏（`rtpprobe`），坐标系换算已裁定 |
| Falcon 打包 + 原生模块加载 | ✅ | `.amr` 装机 → `/tmp/penys.log` 出现 `ysplayer 1.0.0 module init` |
| 前端三态 UI（扫码 / 首页 / 云游戏） | ✅ | `miniapp_cli capture` 截图核对，云原神深青蓝 + 金饰风格 |
| 协议层 JS 在无 native 环境下不炸 | ✅ | 20/20：`ys/index.js` import 后所有接口优雅返回 `NO_NATIVE` |
| RTP → 视频管线 `video_pipe.c` | ⏳ 编写中 | 头文件契约已冻结，`video_pipe.h` |
| 信令栈集成审查 | ⏳ | `sig_selftest.c` / `sdp_selftest.c` |
| **真实会话端到端** | ❌ 未做 | 需用户扫码 + `paasDispatch`（**成功即扣时长**） |
| A/V 同步 | ❌ | 需真实流数据 |

详见 [docs/feasibility.md](docs/feasibility.md)（可行性报告，含真机证据）与 [docs/tasks.md](docs/tasks.md)（任务清单）。

---

## 架构

```
┌──────────────────────  前端（Falcon Vue2 / QuickJS，960×266）──────────────────────┐
│ src/pages/index/index.vue     扫码 / 首页 / 云游戏 三态                            │
│ src/services/player.js        播放状态机（status / touch / start / stop）          │
│ src/services/ys_client.js     协议层入口（静态 import 真实现，失败回退 ys_mock）    │
│ src/services/screen.js        20s 轮询续屏（词典笔 300s 自动熄屏）                 │
│ src/services/qrcode.js        QR 编码（encodeQr）→ 分段行静态渲染                  │
└──────────────────────────────────┬───────────────────────────────────────────────┘
                                   │ import('yshttp') / import('ysws')
┌──────────────────────────────────▼───────────────────────────────────────────────┐
│ 协议层（纯 JS，可脱离 native 单测）  src/services/ys/                              │
│   login.js   扫码登录（createQRLogin → 轮询 → webVerifyForGame → combo 取票）      │
│   dispatch.js 六步调度 + paasDispatch 签名（HMAC-SHA256，签名对象 ≠ 最终 body）     │
│   pb.js      sdk_param protobuf 解码（手写 varint 扫描器）                         │
│   wallet.js  时长查询   storage.js 会话/cookie jar 持久化   const.js 端点与 key     │
│   http.js    yshttp 原生模块加载器（同步 load() + ensureNative() 异步预热）        │
└──────────────────────────────────┬───────────────────────────────────────────────┘
                                   │ custom_init_jsapis()
┌──────────────────────────────────▼───────────────────────────────────────────────┐
│ native 层（zig 交叉编译 aarch64 → libs/libjsapi_ysplayer.so → 打进 .amr）          │
│   net/       ys_http.c(libcurl 请求/cookie jar) ys_crypto.c(RSA/HMAC/SHA)         │
│              ys_ws.c(WSS 帧) ys_jsapi.c(registerCModuleLoader: yshttp / ysws)    │
│   webrtc/    sig.c 信令状态机  dtls.c DTLS-SRTP  sdp.c SDP offer/answer           │
│              wss.c WebSocket 传输   udp_probe.c                                  │
│   rtp/       srtp.c RFC3711 加解密   rtp_reorder.c 乱序重组                       │
│   session/   session.c 单 worker 状态机（唯一持有管线的线程）                       │
│   video/     video_pipe.c  RTP → appsrc → h264parse → mppvideodec → kmssink       │
│   input/     touch.c  触摸报文（归一化坐标、10 指槽位、proto3 编码）               │
│   core/      log / queue / thread                                                 │
└──────────────────────────────────────────────────────────────────────────────────┘
```

**三条铁律**（来自 skill 与真机教训）：

1. **一个 worker 线程独占媒体管线**，JS 只通过命令队列与状态快照与它通信；
2. **JS 只信物理事实**：状态一律从 GStreamer bus / KMS 平面实测，不从「我发过什么」推断；
3. **状态机单向幂等**：`IDLE → CONNECTING → … → PLAYING → IDLE`，重复 stop 是 no-op。

---

## 构建

```sh
# 1) native（交叉编译到 aarch64，zig 在 env/pyzig/ziglang/zig.exe）
powershell -File native/build.ps1

# 2) 前端（QuickJS AMR）
node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p

# 3) 部署到设备（push + miniapp_cli install + start + 抓日志）
powershell -File tools/deploy.ps1
```

要求：Node.js（构建前端）、zig 0.16+、adb（`env/adb/adb.exe`），设备需已 root 并授权。
链接用的 `native/stubs-aarch64*` 是**从设备导出的 import stubs，只用于链接、不打包**。

手动装机等价命令：

```sh
adb push 8002026110200003.1_0_0.amr /tmp/ys.amr
adb shell miniapp_cli install /tmp/ys.amr        # {"ret":0}
adb shell app_switch 8080222437664451
adb shell miniapp_cli start 8002026110200003 index
adb shell miniapp_cli capture /tmp/x.png && adb pull /tmp/x.png
adb shell cat /tmp/penys.log
```

---

## 设备约束（X6 Pro 实测）

- 屏幕：物理 **480×960 竖屏，DRM rotation=1**；Falcon 合成面 **960×266**（超宽横条）
- SoC：RK3562，4×A35，983MB RAM（约 288MB 可用）
- GStreamer **1.22.0**，`mppvideodec` 硬解（h264/h265/av1/vp8/vp9），**无任何可用编码器**
- **没有任何 WebRTC 插件**（`webrtcbin` 不存在，libnice/libsrtp/usrsctp 缺失）→ 必须自建
- OpenSSL **1.1.1l**（有库无头文件 → 手写 `ossl_min.h` 声明），含 DTLS 与 SRTP 密钥导出符号
- KMS：`/dev/dri` 存在，视频平面 `Esmart1-win0`（plane-id 76，zpos 2，**会盖住 UI**）
- 无 python / node / openssl CLI / nm / ldd；有 `strace` / `gdb` / `curl` / `ffmpeg`
- **自动熄屏：270s 调暗、300s 熄屏、900s 睡眠**（`/etc/input-event-daemon.conf`），
  `hal-screen bright_time` 改不动它 → 前端必须 **20s 轮询续屏**

---

## 协议链路（四段）

1. **登录**：`POST /account/ma-cn-passport/web/createQRLogin` → 轮询 `queryQRLoginStatus`
   → `ma-cn-session/web/webVerifyForGame` 换 token。token 全部由服务端 `Set-Cookie` 下发，
   必须自己维护 cookie jar（`ys_http.c`）。
2. **取票**：`combo` 接口拿 `combo_token`，`si` 用 HMAC-SHA256 签名。
3. **调度**：`login → getSpeedLimitConfig → statusCheck → listPingServer → WSS RTT 测速
   → getNodesInfo → paasDispatch`。`paasDispatch` 的**签名对象 ≠ 最终 body**（`regions` 先
   `JSON.stringify` 成字符串参与签名，最终再覆盖成数组），签名 key 与 `si` 的 key 不同。
4. **会话**：解 `sdk_param` protobuf → 连 `wss://…/rtc-sdk`（服务端发 offer，客户端回 answer）
   → DTLS 握手 → SRTP → DataChannel/控制通道收发触摸包。ICE 候选的 `ip:port` 被改写成
   `game_svr_addr` + `rtc_udp_port`（**没有 STUN/TURN，退化为直连**）。

> ⚠ **`paasDispatch` 成功即开始计费**。调试时严禁随手调用；真实会话测试需要单独安排时间窗。

---

## 已知限制

1. **尚未与真实游戏服完成端到端握手**：信令栈只在 mock 服务端与 echo 上验证过，真实
   `sdk_param` 会话需要用户扫码后才能跑（也是唯一未真机验证的环节）。
2. **A/V 同步未验证**：需要 RTP 时间戳 + RTCP SR 对齐，尚无真实流数据。
3. **极验（Aigis）无法处理**：设备 http jsapi 拿不到响应头，账号密码登录遇 `-3101` 即不可用，
   当前只支持扫码登录。
4. **视频平面覆盖 UI**：云游戏画面走 KMS 独立平面（zpos 2），playing 态的控制条只能做浮层，
   不能靠普通 DOM 层级压过去。
5. **未发送 RTCP Receiver Report**：服务端拥塞控制是否会因此误判降码率，待验证。

---

## 许可

[GPL-3.0](LICENSE)（沿用上游 `yunyuanshen-api` 的许可证）。

仅供学习与研究。不包含任何米哈游的素材、资源或代码。
