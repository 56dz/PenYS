# 云·原神 for 有道词典笔 X6 Pro — 可行性报告

> 结论先行：**可行，但必须自建一条精简 WebRTC 栈**（设备上没有任何现成实现）。
> 最硬的三块——DTLS-SRTP 握手、WSS 信令与测速、硬解上屏——**已在真机逐个验证通过**。
>
> 本报告所有结论都附真机输出；没有验证到的地方明确标注"未验证"。

---

## 一、设备硬约束（真机实测）

| 项 | 实测值 | 影响 |
|---|---|---|
| SoC / OS | RK3562 · 4×A35 · Buildroot 2021.11 · Linux 5.10.160 aarch64 · root | 算力够做 SRTP，不够做软解高分辨率 |
| 屏幕 | 物理面板 **480×960 竖屏，DRM rotation=1**；Falcon 合成面 **960×266** | UI 是超宽横条；视频平面另走 KMS |
| 内存 | 983MB 总 / 约 288MB 可用 | 云游戏解码 buffer 要控制 |
| GStreamer | **1.22.0**，84 个插件 | 硬解链路完整 |
| 编解码 | `mppvideodec` 硬解 h264/h265/av1/vp8/vp9；**无任何可用编码器** | 只能当播放端，符合云游戏定位 |
| WebRTC | **完全没有**：无 `webrtcbin`、无 `libnice`/`libsrtp`/`libsctp` | ⚠ 必须自建 |
| OpenSSL | `libssl.so.1.1` = **1.1.1l**，导出 DTLS + SRTP profile 协商 + `SSL_export_keying_material` | ✅ 自建的密码学底座可用 |
| WSS | 无 JSAPI、curl 无 ws/wss 协议 → 必须 native | ✅ 已用 OpenSSL 自建成功 |
| 工具链 | 有 `ffmpeg`/`curl`/`gst-*`/`strace`/`gdb`；**无 python/node/openssl CLI/nm/ldd** | 调试靠 strace/gdb |
| 网络 | wlan0 通，DNS/UDP/HTTPS 全通；可直连米哈游全线域名 | 前提满足 |
| 设备→host | **设备 ping 不通 host** | 联调不能用 host 当服务器 |
| 自动熄屏 | `/etc/input-event-daemon.conf` 的 `[Idle]` 段：<br>`270s = hal-screen down; hal-gsensor wakeup 1`<br>`300s = len_onoff.sh off; hal-screen off; freq_adj low`<br>`900s = system_sleep_wakeup`<br>`hal-screen bright_time` 实测 = **300**（不是默认 60） | ⚠ **云游戏的隐藏杀手，见下节** |

### ⚠ 自动熄屏：这是云游戏的隐藏杀手（真机读配置 + PenDouyu 现场时间线）

用户 m00701 专门提醒「词典笔自动 5 分钟熄屏」。我在真机上读了**原始配置**，
比参考项目的记录还要严格：

```
/etc/input-event-daemon.conf
[Idle]
270s = hal-screen down; hal-gsensor wakeup 1     ← 调暗到 bl_br=5，几乎全黑
300s = len_onoff.sh off; hal-screen off; freq_adj low   ← 背光断电 + CRTC disable + 降频
900s = system_sleep_wakeup                        ← 系统睡眠
reset = sleep 0.1; freq_adj high; hal-screen reset; system_post_wakeup;
```

即 **270 秒调暗、300 秒熄屏**（不是笼统的"5 分钟"），而且 **600s 之后还会
`freq_adj low`**——CPU 降频会同时毁掉 SRTP 加解密的实时性和 WSS 心跳。

PenDouyu 的真机现场（`PenDouyu/src/services/screen.js:246-253`）：
```
570s  hal-screen down  → 亮度 113 → 5（几乎全黑）
600s  hal-screen off   → 背光断电 + DRM crtc disable → gst/ffmpeg 进程被回收
```
息屏后 `pidof` 里播放进程为空——**进程已经死了**。用户看到「黑一下又亮」：
第 1 次点击只能亮屏、出不了画，第 2 次点击（重新拉起管线）才恢复。

**对云原神的具体危害（四条，都不是"画面暗一点"这么轻）**：

1. **KMS 平面停止提交**。息屏时内核 disable CRTC，`kmssink` 不再翻页。
   此时量帧率/延迟会得到「管线卡死」的**假阳性**——探针数据全部作废。
2. **音画不同步的假阳性**。`alsasink` 不依赖 CRTC，音频继续播而视频停了。
   A/V 同步探针若跨越一次熄屏，会量出巨大的偏移。
3. **WSS 心跳断**。信令要求每 10s 一条裸空帧；进程被挂起或回收 → 心跳停 →
   服务端判定客户端离线 → **踢掉云游戏会话**，已扣的时长白费。
4. **DTLS 超时**。握手/重传需要定时器；`freq_adj low` 降频后 1s 级的
   retransmit 定时器会漂移。

**对策（三层，缺一不可）**：
- 播放态（含暂停态）每 **20s** 调一次 `keepScreenOn()`——`hal-screen keep`
  本身只续 **30 秒**，间隔必须 <30s。
- **只调 keep 不够**：它只续「不动作窗口」，**不恢复已经掉下去的亮度**。
  亮度被调到 `bl_br=5` 之后只有 `setBrightness()` 能拉回。
- 主判据必须是 `getScreenState()`，**不是** `getBrightness()`——真机实测
  屏幕已暗到 5 时 `getBrightness()` 仍返回 80（返回的是配置档位，不是实际背光）。
- 深熄态（`bl_power=4`）下**第 1 次 keep 只置 `/tmp/.screen_state=1`、背光仍断电，
  第 2 次 keep 才真正点亮** → keep 完必须复检 state，仍是 OFF(1) 就再补一次。

#### 实测否证：`hal-screen bright_time` **改不动** `[Idle]` 计时器

曾以为把 `bright_time` 设大就能少轮询。真机上直接验了一遍：

```
$ hal-screen bright_time 10          # 设成 10 秒
$ hal-screen bright_time
bright_time:10
$ sleep 20                           # 等 20 秒（>10s）
$ hal-screen state
screen_state:0 sys_br:80 bl_br:113 map2brightness:113
get_screen_state: SCREEN_ON          # ← 屏幕没暗，也没有熄
```

结论：`bright_time` 只影响 `hal-screen` 自己那层的"多久算超时"，
**`[Idle]` 段的 270s/300s/900s 是 input-event-daemon 里硬编码的，不受它控制**。
所以「设大 bright_time 就不用轮询保活」这条路是**走不通的**，必须老实每 20s 续一次。

（测完已把设备 `bright_time` 恢复成原值 300，不留副作用。）

`hal-screen keep` 实测返回 0，且 `/tmp/.screen_state` 内容为 `screen_on=1`
——这是 native 侧判断屏幕状态最便宜的信号源（读一个 <32 字节的文件）。

**这解释了为什么阶段 10（屏幕保活）被单独列成一个阶段**，而不是 UI 的一个小功能：
它同时约束 UI 定时器、native 心跳、DTLS 超时判定，以及**所有真机长测数据的可信度**。



---

## 二、可行性验证矩阵

### ✅ 已验证通过（有真机输出）

**1. 米哈游 HTTP 接口可从设备直连**
```
POST https://passport-api.mihoyo.com/account/ma-cn-passport/web/createQRLogin  → retcode:0
{"url":"https://user.mihoyo.com/login-platform/mobile.html?...","ticket":"157acdbf-..."}
```

**2. DTLS 1.2 + SRTP profile 协商 + 密钥导出**（自建探针 `dtls_probe`，真机）
```
[client] HANDSHAKE OK, version=DTLSv1.2
  selected_srtp_profile = SRTP_AES128_CM_SHA1_80
[client] setup_srtp rc=0
  keying_material(60) = 7dd803c177a8a0df621ac7fe36082dd522db446658d0fe7000c481cb64b09bb3...
  client_write_key   = 7dd803c177a8a0df621ac7fe36082dd5
  server_write_key   = 22db446658d0fe7000c481cb64b09bb3
```
→ 自建 WebRTC 最难的密码学环节**不再是风险**。

**3. WSS 信令 / RTT 测速**（自建探针 `ws_probe`，真机连米哈游真实 wssping 服务器）
```
[ws] TLS ok in 191 ms, version=TLSv1.3
[ws] upgrade response: HTTP/1.1 101 Switching Protocols
[ws] pings sent=20 recvd=20 avg_rtt=39 ms
[ws] DONE PASS
```
→ 调度第 5 步（`WebSocketRttTester`）可在设备上原样复刻。

**4. 硬解上屏链路**：`rtph264depay → h264parse → mppvideodec → kmssink`（详见 media 报告）

### ⚠ 已验证为「不能用」，需绕行

| 能力 | 状态 | 绕行方案 |
|---|---|---|
| `webrtcbin` | 不存在 | 自建 DTLS-SRTP（已验通） |
| libnice / ICE | 不存在 | 米哈游**无 STUN/TURN**，candidate 被服务端改写成固定 `game_svr_addr:rtc_udp_port` → 退化成直连固定 UDP，只需一个 socket |
| SCTP / DataChannel | 不存在 | 用 `game_control_channel_url`(:5443 WebSocket) 传触摸，**报文格式与 DataChannel 完全一致** → 完全绕开 SCTP |
| Falcon http jsapi 读响应头 | 拿不到 | native libcurl + cookie jar |
| 扫码登录在 JS 层 | 不可行 | 同上，下沉到 native |

### ❓ 尚未验证（风险点，诚实列出）

1. **真实服务端 offer SDP 的完整兼容性** —— answer 生成逻辑目前只对构造的 offer 做过 round-trip，未与米哈游真实游戏服握手。
2. **音频编码格式** —— 推测 Opus（`rtpopusdepay`/`opusdec` 可用），但未拿到真实流。
3. **A/V 同步** —— 需要 RTP 时间戳与 RTCP SR 对齐，未实流验证。
4. **触摸在真实会话中是否生效** —— 报文格式已知，但未经真实游戏验证。
5. **风控** —— 非官方客户端有被判定异常的风险（见风险章节）。

---

## 三、架构方案

```
┌─ Falcon UI (Vue2 / QuickJS) ────────────────────────────────┐
│ 登录扫码 · 主页 · 调度进度 · 游戏控制条 · 触摸映射层         │
└──────────────┬──────────────────────────────────────────────┘
               │ JSAPI
┌──────────────▼──────────────────────────────────────────────┐
│ native libjsapi_ysplayer.so                                  │
│  ├─ worker 线程（唯一管线所有者）                            │
│  ├─ HTTP 段：libcurl + cookie jar（登录/调度）               │
│  ├─ 信令段：自建 WSS（TCP+SNI+TLS+RFC6455）                  │
│  ├─ 安全段：DTLS 1.2 握手 → EXTRACTOR-dtls_srtp → SRTP 密钥  │
│  ├─ 媒体段：SRTP 解封 → RTP 重组 → appsrc → mppvideodec      │
│  │          → kmssink（视频平面） + alsasink（音频）          │
│  └─ 输入段：GameControlChannel(WSS:5443) 发触摸 Packet       │
└──────────────┬──────────────────────────────────────────────┘
               │ DRM/KMS plane + ALSA
┌──────────────▼──────────────────────────────────────────────┐
│ RK3562 硬件：MPP 解码器 · KMS 视频平面 · 音频 DAC            │
└─────────────────────────────────────────────────────────────┘
```

**三条铁律**（来自 skill，必须遵守）：
1. 一个 worker 线程独占整条管线，JS 线程只投递命令，绝不阻塞。
2. JS 只信可观测的物理事实（KMS 平面真的变了），不信 native 上报的"我以为"。
3. 状态机单向 `IDLE → CONNECTING → PLAYING ⇄ PAUSED → TEARDOWN → IDLE`，每次转换幂等可重入。

---

## 四、工作量与风险

| 模块 | 状态 | 风险 |
|---|---|---|
| UI 骨架（云原神风格） | 可独立完成，无外部依赖 | 低 |
| native HTTP + cookie jar + 调度链 | 技术路径清晰 | 中（风控） |
| 自建 WSS | **已验证跑通** | 低 |
| DTLS-SRTP 握手 + 密钥导出 | **已验证跑通** | 低 |
| SRTP 加解密（RFC 3711） | 算法成熟，libcrypto 可用 | 低-中 |
| RTP 重组 + 硬解上屏 | 元素齐全 | 中（延迟调优） |
| SDP 协商 | 逻辑清晰 | **中-高**（未对真实服务端验证） |
| 触摸输入 | 报文格式已知 | 中 |
| A/V 同步 | — | **高**（未验证） |

**风险提示（必须让用户知道）**
1. 这是**非官方客户端**，使用第三方逆向协议接入米哈游云游戏，存在账号被风控/封禁的可能。
2. `paasDispatch` 成功即**开始计费/扣时长**，测试要克制（只在必要时打一次，之后全用 fixture）。
3. 云游戏时长按分钟计费，调试期间会持续消耗。

---

## 五、验证过程中沉淀的 6 个关键技术坑

这几条是花真机时间换来的，后续开发必须继承：

1. **`getaddrinfo` 必须用 `AF_INET`**。用 `AF_UNSPEC` 时会先试 IPv6 并等超时，TCP 建连从 **116ms** 劣化到 **15087ms**。这个坑会毁掉整个体验。
2. **`BIO_new_dgram` 不会自动获知 peer**。必须手动 `BIO_ctrl(bio, 44 /*SET_PEER*/, ...)` + `BIO_ctrl(bio, 32 /*SET_CONNECTED*/, ...)`，否则 OpenSSL 发 `sendto()` 时传 `AF_UNSPEC` sockaddr，内核返回 `EINVAL`。
3. **SRTP profile 必须在 `SSL_new()` 之前设到 CTX 上**。设在握手后，`SSL_get_selected_srtp_profile()` 两侧都返回 `null`。
4. **`SRTP_PROTECTION_PROFILE` 结构体是 `{ const char *name; unsigned long id; }`**——name 在前。写反会 SIGSEGV。
5. **`SSL_get_cipher` 在 1.1.1l 里不存在**，用 `SSL_get_version`。
6. **`SSL_export_keying_material()` 的 label 长度必须精确，`"EXTRACTOR-dtls_srtp"` 是 19 字节不是 20**。多算一个字节会把结尾 `\0` 一起哈希进导出器，两侧算出的密钥**完全不同**，但握手本身完全正常（`SSL_connect`/`SSL_accept` 都返回 1，`SSL_get_selected_srtp_profile()` 也正常返回 profile）。症状是"握手成功、profile 协商成功、但密钥对不上"，极易误判成 OpenSSL 版本或 SRTP profile 问题。**永远用 `sizeof(label) - 1`，不要硬编码长度**。真机复现：
```
[compare] server km = 5ee408ead3b4ff1223977b72a2a7e89f...   ← 错用了 20
[compare] client km = cfeaae371363b4ff440d866255ca126b...   ← 正确用了 19
[compare] MISMATCH
```
改成 19 后同一对进程立刻 `[compare] MATCH`。
