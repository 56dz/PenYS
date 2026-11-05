# 信令与媒体面：最新一手结论

这份文档记录的是**真机抓包验证过的事实**，不是推测。凡是推测都会明确标成「假设」。
上一版 `webrtc-flow` 之类的描述（连上 WSS 就发 client hello、等 offer）是**错的**，
照那个实现的表现就是「一直卡在服务器 offer 连接超时」。

## 1. 信令时序（真机已验证）

```
WSS 升级 (wss://<edge>/<pod>/<port>/rtc-sdk)
  → 客户端发 StartGameReq  (帧 type=2 proxy，Packet cmd=20001)
  → 服务端回 StartGameRsp  (帧 type=2 proxy，Packet cmd=20002，field1 retcode==1)
  → 客户端发 client hello  (帧 type=3 handshake，JSON {"client_type":"web","type":"client hello"})
  → 服务端回 server hello  (帧 type=3，JSON {"session_id":N,"type":"server hello"})
  → 服务端回 offer         (帧 type=1 signaling，JSON {"type":"offer","sdp":...})
  → 客户端回 answer        (帧 type=1，JSON {"type":"answer","sdp":...})
  → candidates 双向，10s 一次裸空帧心跳
```

三个关键点：

1. **`server hello` 不需要任何回应**。它的 `session_id` 只用于 :5443 的输入控制通道
   （`GameControlChannel.setSessionId`）。把它当成「续跑信号」去等，就是官方 SDK 里
   根本不存在的逻辑。
2. **`client hello` 是 StartGameRsp 成功之后才发的**。官方 `cg-sdk 6.1.0.65` 里
   `handleStartGameRsp()` 校验 retcode==1 且 gameConfig/gameAppId 非空，然后才
   `startRtcStream() → sendHandShake()`。
3. `StartGameReq.aid` = passport `verifyCookieToken` 返回的 `data.user_info.aid`，
   **不在 sdk_param 里，也不在 gamerLogin 响应里**（真机实测它只回 `new_user` 一个键）。
   aid 缺失时服务端照样回 StartGameRsp(retcode=1)、照样下发 offer，
   所以它不是一个「会报错」的字段，但它是账号维度的，缺了不该指望媒体面正常。

### 帧格式

```
信令帧（8 字节大端头）：[0..3]=type  [4..7]=len  [8..]=payload
  type 1 signaling(JSON) / 2 proxy(二进制) / 3 handshake(JSON) / 4 keepalive
  心跳：直接发空文本帧 ""（没有 8 字节头）

Packet（proxy 帧的载荷）：minPacketSize = 12
  [0..1] magic 0x4567 (69,103)
  [2..3] cmd_id BE
  [4..5] head_len (StartGameReq 场景恒 0)
  [6..9] msg_len BE
  [10..] PacketHead + msg
  [-2..] magic tail 0x89AB (137,171)
```

`StartGameReq` 字段：1 game_token、2 aid、3 sdk_version("6.1.0.65")、4 terminal_type
（`ClientTerminalType.WebTouch = 9`，**不是** `PROFILE.cloudClientType` 的 18）、
6 sid、7 pod_id、8 has_rtc_connected、9 since_start_sec、10 link_tasks_ms、12 game_config。
proto3 语义：空串/0 不写出。

## 2. 媒体面：当前卡在 DTLS 握手（未解决）

信令层现在能稳定跑到：

```
sig: StartGameRsp retcode=1
sig: client hello sent
sig: json type=offer
sig: answer sent (video pt=98 audio pt=111)
udp: connected to 120.241.227.139:32803 (rtc_udp_port=32803)
ice: binding request 112 bytes (...)
ice: no reply #0..#3
session ERROR DTLS_FAIL: handshake timed out after 6000 ms (6 retransmissions)
```

### 已经排除的可能

| 猜测 | 证据 | 结论 |
|---|---|---|
| UDP 出网被封 | 笔向 `74.125.250.129:19302` 发裸 STUN，收到 `0x0101` 回包 | 排除 |
| 我们构造的 STUN 包格式错 | 按 RFC5389 用 Node crypto 复算 MESSAGE-INTEGRITY，与设备字节完全一致 | 排除 |
| 端口写错（用了 TCP 口） | 已改成 `rtc_udp_port`（svr/rtc 成对，如 32802/32803） | 已修 |
| 笔的问题 | 同一份字节从 PC（同公网出口）回放，同样 0 回应 | 排除 |
| IP 不可达 | 同 IP 的 TCP `svr_port` 能连上（`curl` 收到 RST，说明有服务在听） | IP 对，UDP 面沉默 |

### 剩下的假设（按可能性排序，下一位从这里继续）

1. ~~**answer 里的 `a=candidate` 填错了**~~ —— 已改，**尚未真机验证**（笔已关机）。
   原来 `cfg.candidate_ip/candidate_port` 填的是 `game_svr_addr`（服务端自己的地址），
   等于向服务端宣告「我的 candidate 就是你的地址」。现在：
   - `w_open_udp()` 在**发 answer 之前**就把媒体 socket 建好（`connect()` 会绑定本地地址 +
     临时端口），`getsockname()` 拿到真实的本地 endpoint；
   - answer 里写的是 `a=candidate:1 1 udp 2130706431 <本机IP> <本机端口> typ host`；
   - answer 之后再用 `sig_send_candidate()` 把同一个 candidate **通过信令 trickle 出去**
     （浏览器 SDK 就是这么做的：`onicecandidate` → `{type:'candidates'}`）。
   若边缘按「已信令的 remote candidate」白名单过滤 check，这一条才是关键；
   若它不是这么实现的，那这一步只是让 SDP 不再撒谎。
   真机验证方法：开机后跑一轮，看
   `udp: connected to ... local=<ip>:<port>`、`sig: local candidate sent (...)`、
   以及 `ice: reply #0` 是否出现。
2. 媒体边缘对「源地址未通过某种准入」的流量静默丢弃：可能要求先连 TCP `svr_port`
   完成可靠通道/加入流程，UDP 口才对这对 5-tuple 开放。
3. `rtc_udp_port` 不是公网媒体口，真正的媒体地址在别的机器上（比如 WSS 边缘主机）。
4. 需要一次**真实浏览器**的完整抓包（PC Chrome 开 ys.mihoyo.com/cloud）作为一手对照：
   看它的 STUN 是发往哪个 IP:端口、USERNAME/pwd 是否就是 offer 里的那组、
   以及 answer 里的 candidate 长什么样。

## 3. 调试工作流（真机）

```sh
# 构建
cd PenYS/native && powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1
cd PenYS && node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p

# 部署（MSYS 必须关路径改写，否则 push 目标被改成 Windows 路径）
MSYS_NO_PATHCONV=1 env/adb/adb.exe push PenYS/8002026110200003.1_0_0.amr /tmp/penys.amr
MSYS_NO_PATHCONV=1 env/adb/adb.exe shell "touch /tmp/penys_log; miniapp_cli install /tmp/penys.amr"
MSYS_NO_PATHCONV=1 env/adb/adb.exe shell "rm -f /tmp/penys.log; miniapp_cli start 8002026110200003 index"
```

坑：

- `miniapp_cli` **没有 stop**；重启只能重装（install 会重启进程）。
- 页面名不能带 `--`：`start <appid> --index` 会 `could not load module filename '--index.js'`，黑屏。
- 日志只在 `/tmp/penys_log` 存在时写 `/tmp/penys.log`。
- 截图：`miniapp_cli capture /tmp/cap.png`（960x266 逻辑画布）；`captureFB` 不可用。
- 触摸注入没用：`/dev/input/event1` 由 weston/seatd 独占，`send_event touch` 写进去
  app 收不到。真机取证只能靠临时自动起播钩子（见下）。

### 临时自动起播钩子（取证用，不要提交）

在 `src/pages/index/index.vue` 的 `bootTick()` 里 `this.gotoHome();` 之后加：

```js
this.later(() => {
  if (this._dead) return;
  logWarn('DEV_AUTOSTART: 自动开始游戏');
  this.doDispatch();
}, 2600);
```

## 4. 这次一并修掉的两个坑

- **HttpOnly cookie 被整行丢弃**：`native/csrc/net/ys_http.c` 里
  `strncmp(p, "#HttpOnly_", 11)` —— 字面量只有 10 字节，第 11 个字节是 `'\0'`，
  于是永远不相等，`cookie_token_v2/ltoken_v2/account_id_v2` 全丢，表现为
  「重启后登录态没了、verifyCookieToken 返回 -100」。已改成
  `JAR_HTTPONLY` / `JAR_HTTPONLY_LEN` 宏。
- **重启后顶栏显示「未登录」**：`nickname/uid` 原先只在 `finishLogin()` 里赋值，
  冷启动恢复会话的路径从不赋值（钱包走 cookie jar，所以看着还能显示剩余时长）。
  现在 `syncProfileFromSession()` 在 bootstrap 后补一次，并且 passport 只给 `mid`
  不给 `nickname` 时用 `mid` 兜底。

## 5. 主机侧测试（不需要词典笔）

`native/tests/sdp_cand_test.c` 用一份结构忠实于真实 offer 的 SDP 跑
`sdp_build_answer()`，锁死 answer 里几条一旦写错就只表现为「服务端静默丢包」的东西：

- `a=candidate` 必须是**我们自己的** `ip:port`（`cfg.local_cand_ip/local_cand_port`），
  不能是 `game_svr_addr`——填错时对端会认为 check 来自陌生 peer，直接丢弃，没有任何报错；
- 只给旧的 `candidate_ip/candidate_port` 时回退路径仍然有效；
- 什么都不给时 answer 里不出现 `a=candidate`（纯 trickle 模式）；
- answer 必须带我们生成的 `a=ice-ufrag` / `a=ice-pwd`（服务端拿它校验 STUN USERNAME
  里的 `remote:local`，缺了同样是不回包）；
- `a=group:BUNDLE` 不能包含被拒绝的 m-line（我们拒绝 `m=application`，所以只有 `0 1`）；
- 被接受的 m-line 端口要写我们真实的媒体端口（不是 offer 里的 `9`），`c=` 要写我们真正的
  源地址——浏览器就是这么答的，把 `9`/`0.0.0.0` 留给 ICE 去猜，会让只认 m-line 的边缘
  把 DTLS 打到 9 号端口；
- answer 里要有 `a=ice-options:trickle`：我们随后确实通过信令补发了 candidate，
  声明与行为不一致时严格的 SDP 校验器会整份拒绝。

跑法（zig 交叉编译到 Windows 主机，和真机库同一套工具链）：

```sh
ZIG_GLOBAL_CACHE_DIR="$TEMP/penys-zig-cache" ZIG_LOCAL_CACHE_DIR="$TEMP/penys-zig-cache-local" ./env/pyzig/ziglang/zig.exe cc -target x86_64-windows-gnu -O1   -I PenYS/native/csrc/webrtc -o .tmp/sdp_cand_test.exe   PenYS/native/tests/sdp_cand_test.c PenYS/native/csrc/webrtc/sdp.c
./.tmp/sdp_cand_test.exe      # 退出码 0 = 全过
```

注意：`sdp.c` 里 `o=` 的随机数走 `RAND_bytes`（真机上是 libcrypto），主机测试在自己的
文件里给了一个确定性 stub，所以不必链接 OpenSSL。

## 6. 一条命令验证真机

```sh
sh PenYS/scripts/device-verify.sh      # 在工作区根目录（含 PenYS/ 与 env/ 的那一层）跑
```

它做四件事：push 新包 → `miniapp_cli install`（顺带重启 app，因为 `miniapp_cli` 没有 stop）
→ 清空 `/tmp/penys.log` 并按正确写法 `start <appid> index`（页面名不能带 `--`）→ 等 25 秒后
把关键日志行抓出来并逐行解释。adbd 认证、MSYS 路径改写这些坑都已经在脚本里处理掉了。

仍然需要人做的事：在设备上点「开始游戏」（触摸注入在这台机器上无效），或者临时把
`DEV_AUTOSTART` 钩子加回 `bootTick()` 后重新打包。

## 7. 拿一次浏览器的「一手对照」怎么做

这一节是留给「ICE 仍然 no reply」时用的最后手段。我们复现不出服务端的行为时，
唯一可靠的做法是让官方 web 客户端在同一网络下跑一次，把它真正发的包抓下来。

**注意：这一步会真的申请云端实例、按分钟计费。** 只在对账号费用无异议时做。

1. 在同一台 PC、同一个公网出口下，用 Chrome 打开 https://ys.mihoyo.com/cloud/ ，登录后
   点开始游戏，等它出画面。
2. 抓 `chrome://webrtc-internals`：里面能看到 offer/answer 全文、所有 local/remote
   candidate、以及最终选中的 candidate pair。**重点看 remote candidate 被改写成什么**
   （预期是 `gameSvrAddr` 的 IP + `rtcUdpPort`，这正是我们的做法），以及 answer 里
   `m=` 端口、`a=candidate`、`a=ice-ufrag` 与浏览器实际发 STUN 的 5-tuple 是否一致。
3. 抓包：Wireshark 选 PC 的物理网卡，过滤 `udp and ip.addr == <媒体IP>`。重点看
   - 浏览器第一个 STUN Binding Request 的**目的端口**、USERNAME、以及服务端是否回
     Binding Success；
   - 如果服务端先发 check（ICE-lite 一般也允许），它的目的地址是谁；
   - DTLS ClientHello 什么时候发、发往哪个 5-tuple、服务端有无 HelloVerifyRequest。
4. 把 pcap 与 webrtc-internals 的文本一起存到 `.tmp/browser-capture/`，再用
   `.tmp/stun_verify.js` 的方式逐字段对比我们的实现。

对照要点（任意一条不同，就可能是静默丢包的原因）：
`candidate 目的地址/端口`、`USERNAME` 的 `remote:local` 组成、`answer 里的 ufrag/pwd`、
`DTLS 目标 5-tuple`、`m-line 端口`、以及服务端是否真的回 STUN。
