# 云·原神 for 有道词典笔 X6 Pro — 任务清单

> 状态图例：`✅ 完成` / `🔄 进行中` / `⬜ 待办` / `⛔ 阻塞`
> 本清单按**依赖顺序**排列，不是按优先级。

---

## 阶段 0 · 可行性验证 ✅ 完成

| # | 任务 | 状态 | 证据 |
|---|---|---|---|
| 0.1 | 设备画像（屏幕/SoC/GStreamer/库/网络） | ✅ | `docs/feasibility.md` §一 |
| 0.2 | 米哈游 HTTP 接口连通性（真机 createQRLogin） | ✅ | `retcode:0` |
| 0.3 | DTLS 1.2 + SRTP profile 协商 + 密钥导出 | ✅ | `dtls_probe` 真机输出 |
| 0.4 | WSS 建连 + RFC6455 upgrade + RTT 测速 | ✅ | `ws_probe` 真机输出，256ms / 39ms |
| 0.5 | 硬解上屏 `mppvideodec ! kmssink` | ✅ | media 报告 |
| 0.6 | 交叉编译工具链（zig cc → aarch64） | ✅ | 多个探针已编译运行 |
| 0.7 | 输出可行性报告 + 任务清单 | ✅ | 本文件 + `docs/feasibility.md` |

**阶段 0 结论：可行。核心风险（设备无 WebRTC）已被"自建精简 WebRTC 栈"击穿，关键环节全部真机验通。**

---

## 阶段 1 · 基础工程骨架

| # | 任务 | 状态 | 负责 | 依赖 |
|---|---|---|---|---|
| 1.1 | `PenYS/` 项目结构、`package.json`、appid、构建脚本 | 🔄 | ui-shell | 0.6 |
| 1.2 | `src/app.js` / `app.json` / 设计宽度 960 视口 | 🔄 | ui-shell | 1.1 |
| 1.3 | less 主题变量（云原神配色：墨蓝 + 米金） | 🔄 | ui-shell | 1.1 |
| 1.4 | `native/build.ps1`（zig 交叉编译，aarch64） | ⬜ | lead | 1.1 |
| 1.5 | `native/iot-miniapp-sdk/include/`（从 PenDouyu 复制） | ⬜ | lead | 1.1 |
| 1.6 | `native/stubs-aarch64/`（import stubs，从 PenDouyu 复制） | ⬜ | lead | 1.1 |
| 1.7 | `tools/inspect_elf.py` + `tools/deploy.ps1`（改造自 PenDouyu） | ⬜ | lead | 1.4 |

---

## 阶段 2 · UI（可并行，不依赖 native）

| # | 任务 | 状态 | 负责 | 依赖 |
|---|---|---|---|---|
| 2.1 | 启动页 `boot`（品牌 + 加载动画） | 🔄 | ui-shell | 1.2 |
| 2.2 | 扫码登录页 `login`（二维码复用 PenBili 的 qrcode.js） | 🔄 | ui-shell | 1.2 |
| 2.3 | 主页 `home`（头像/昵称/剩余时长/开始游戏/画质档位） | 🔄 | ui-shell | 1.2 |
| 2.4 | 调度页 `dispatching`（排队/节点/步骤条） | 🔄 | ui-shell | 1.2 |
| 2.5 | 游戏页 `playing`（视频占位框 + 触摸映射层 + 自动隐藏控制条） | 🔄 | ui-shell | 1.2 |
| 2.6 | 错误页 `error`（米哈游错误码中文映射） | 🔄 | ui-shell | 1.2 |
| 2.7 | 真机安装 + 启动 + 截图能力验证 | 🔄 | ui-shell | 2.1-2.6 |

---

## 阶段 3 · HTTP 协议链（native + JS）

| # | 任务 | 状态 | 负责 | 依赖 |
|---|---|---|---|---|
| 3.1 | native `libjsapi_yshttp.so`：libcurl + 读响应头 + cookie jar | 🔄 | proto-http | 1.4 |
| 3.2 | `hmacSha256(keyHex, data)` native 导出（验证 QuickJS 是否有 WebCrypto） | 🔄 | proto-http | 3.1 |
| 3.3 | `rsaEncrypt`（RSA-1024 PKCS#1 v1.5，libcrypto） | 🔄 | proto-http | 3.1 |
| 3.4 | `wsRttTest(url, count, timeoutMs)` native 导出 | 🔄 | proto-http | 3.1 |
| 3.5 | 扫码登录：`createQRLogin` → 轮询 → `webVerifyForGame` → `webLogin` → `gamerLogin` | 🔄 | proto-http | 3.1 |
| 3.6 | 调度 6 步链（到 `getNodesInfo` 为止，不扣时长） | 🔄 | proto-http | 3.4, 3.5 |
| 3.7 | `paasDispatch` 签名（**签名对象 ≠ body**，见 pitfalls） | 🔄 | proto-http | 3.6 |
| 3.8 | protobuf：`sdk_param` 解码 + `PacketHead` 编码 | 🔄 | proto-http | 3.6 |
| 3.9 | 二维码生成 JS（复用 PenBili `qrcode.js`） | 🔄 | proto-http | — |
| 3.10 | wallet 接口（剩余时长） | 🔄 | proto-http | 3.5 |
| 3.11 | **`paasDispatch` 真机打一次验证**（触发计费，需克制） | ⬜ | proto-http | 3.6, 3.7 |

---

## 阶段 4 · 信令与安全层（native）

| # | 任务 | 状态 | 负责 | 依赖 |
|---|---|---|---|---|
| 4.1 | `wss.h/c`：TCP(AF_INET)+SNI+TLS+RFC6455，掩码/分片/ping-pong | 🔄 | ws-dtls | 1.4 |
| 4.2 | `dtls.h/c`：客户端握手 + 本地自签证书 + fingerprint | 🔄 | ws-dtls | 1.4 |
| 4.3 | `dtls_get_srtp_keys()`：`EXTRACTOR-dtls_srtp` 60B 切分 | 🔄 | ws-dtls | 4.2 |
| 4.4 | `sdp.h/c`：offer 解析 + answer 生成 | 🔄 | ws-dtls | 4.2 |
| 4.5 | `stun.h/c`：最小 Binding Request（UDP 可达性探测） | 🔄 | ws-dtls | — |
| 4.6 | 信令协议：8 字节大端头 + `client hello` + offer/answer/candidate | ⬜ | ws-dtls | 4.1 |
| 4.7 | **ICE candidate 地址改写**（→ `game_svr_addr:rtc_udp_port`） | ⬜ | ws-dtls | 4.6 |
| 4.8 | 心跳（10s 裸空帧） | ⬜ | ws-dtls | 4.6 |
| 4.9 | 与真实米哈游游戏服 WSS 握手验证 | ⬜ | ws-dtls | 3.11, 4.6 |

---

## 阶段 5 · 媒体层（native）

| # | 任务 | 状态 | 负责 | 依赖 |
|---|---|---|---|---|
| 5.1 | `srtp.h/c`：AES-CM + HMAC-SHA1-80 加解封（RFC 3711） | 🔄 | srtp-crypto | 1.4 |
| 5.2 | 重放窗口 + ROC 估计（乱序/丢包） | 🔄 | srtp-crypto | 5.1 |
| 5.3 | SRTCP 解密 | 🔄 | srtp-crypto | 5.1 |
| 5.4 | RFC 3711 附录 B 测试向量（外部权威值） | 🔄 | srtp-crypto | 5.1 |
| 5.5 | RTP 重组：FU-A / STAP-A / 单 NAL → 完整帧 | 🔄 | media-probe | — |
| 5.6 | `appsrc ! h264parse ! mppvideodec ! kmssink` 上屏 | 🔄 | media-probe | 5.5 |
| 5.7 | `render-rectangle` 视口映射（rotation=1 换算） | 🔄 | media-probe | 5.6 |
| 5.8 | 延迟/帧率调优（sync/queue/low-latency 参数） | 🔄 | media-probe | 5.6 |
| 5.9 | 音频：`rtpopusdepay ! opusdec ! alsasink` | 🔄 | media-probe | — |
| 5.10 | 视频平面是否遮挡 UI 的结论 | 🔄 | media-probe | 5.6 |
| 5.11 | A/V 同步（RTP 时间戳 + RTCP SR 对齐） | ⬜ | media-probe | 5.6, 5.9 |

---

## 阶段 6 · 输入层

| # | 任务 | 状态 | 负责 | 依赖 |
|---|---|---|---|---|
| 6.1 | `GameControlChannel`(WSS:5443) 连接 + `?sessionId=` | ⬜ | lead | 4.1 |
| 6.2 | `Packet` 帧封装（`0x4567` + cmd 20010 + head/msg len + `0x89AB`） | ⬜ | lead | 3.8 |
| 6.3 | 触摸坐标归一化（竖屏旋转 + rect 偏移） | ⬜ | lead | 6.2 |
| 6.4 | 单指/多指（10 槽位）+ primary 语义 | ⬜ | lead | 6.3 |
| 6.5 | 触摸映射层 UI ↔ native | ⬜ | lead | 2.5, 6.3 |

---

## 阶段 7 · 集成与真机验收

| # | 任务 | 状态 | 负责 | 依赖 |
|---|---|---|---|---|
| 7.1 | 主 .so 合并（http + wss + dtls + srtp + video + input） | ⬜ | lead | 3,4,5,6 |
| 7.2 | worker 线程 + 命令队列 + 状态机 | ⬜ | lead | 7.1 |
| 7.3 | JSAPI 层（start/stop/setRect/touch/status） | ⬜ | lead | 7.1 |
| 7.4 | 端到端：扫码 → 主页 → 调度 → 进游戏 → 画面+声音 | ⬜ | lead | 7.3 |
| 7.5 | 触摸能操作游戏 | ⬜ | lead | 6.5, 7.4 |
| 7.6 | 进出循环无泄漏（线程/子进程/FIFO/KMS 平面） | ⬜ | lead | 7.4 |
| 7.7 | 打包 `.amr` + 安装 + 冷启动验证 | ⬜ | lead | 7.4 |
| 7.8 | 交付前检查清单（skill 的 checklist 逐条过） | ⬜ | lead | 7.7 |

---

## 阶段 8 · 文档与交付

| # | 任务 | 状态 | 负责 |
|---|---|---|---|
| 8.1 | `README.md`（构建/安装/使用/已知限制） | ⬜ | lead |
| 8.2 | `docs/architecture.md`（分层、线程模型、状态机） | ⬜ | lead |
| 8.3 | `docs/protocol-notes.md`（米哈游协议要点 + 5 个踩坑） | ⬜ | lead |
| 8.4 | 版本号/manifest/README 三处一致 | ⬜ | lead |

---

## 阶段 9 · GitHub 交付（用户 m00701 要求）

仓库：https://github.com/56dz/PenYS （用户已建好，GPL-3.0，当前只有 Initial commit + LICENSE）

| # | 任务 | 状态 | 负责 |
|---|---|---|---|
| 9.1 | `git init` + remote 指向 `56dz/PenYS` + 分支 `main` | ⬜ | lead |
| 9.2 | `.gitignore`（排除 `libs/*.so`、`*.amr`、`profiles/evidence/`、`node_modules`） | ⬜ | lead |
| 9.3 | 以**开发者身份**完成 README（架构、构建、真机验证矩阵、已知限制、免责声明） | ⬜ | lead |
| 9.4 | 保留上游 `LICENSE`（GPL-3.0）不覆盖 | ⬜ | lead |
| 9.5 | commit + push 到 `main`，确认远端可见 | ⬜ | lead |

**README 撰写要求**（用户原话：「以开发者身份完成 readme」）：
- 第一人称开发者视角，不是营销文案，写清楚**为什么这么做**而不只是**做了什么**
- 必须包含真机验证矩阵，区分「真机已验证」/「host 已验证」/「未验证」
- 必须包含免责声明（非官方客户端、账号风险、计费）
- 必须写清楚 5 个硬技术坑（无 WebRTC、AF_INET、签名对象≠body、proto3 默认值、KMS render-rectangle 单向棘轮）

---

## 阶段 10 · 屏幕保活（用户 m00701 提醒的致命缺陷）

**背景**：用户提醒「词典笔自动 5 分钟熄屏」。PenDouyu 真机实测时间线
（`PenDouyu/src/services/screen.js:246-253`）：

```
570s  hal-screen down  → 亮度 113 → 5（几乎全黑）
600s  hal-screen off   → 背光断电 + DRM crtc disable → 播放进程被回收
```

息屏后 `pidof` 里播放进程为空——进程已死。用户看到「黑一下又亮」，第 1 次点击
只能亮屏、出不了画，第 2 次点击才恢复。**对云游戏这是致命缺陷**：
WSS 心跳会断、DTLS 会超时、服务端判定离线踢会话。

| # | 任务 | 状态 | 负责 |
|---|---|---|---|
| 10.1 | 复用 `PenDouyu/src/services/screen.js` → `PenYS/src/services/screen.js` | 🔄 | ui-shell |
| 10.2 | 播放态（含暂停态）每 20s 续屏（**必须 <30s**，`hal-screen keep` 只续 30s） | 🔄 | ui-shell |
| 10.3 | 熄屏恢复：`keepRetryNeeded(state)`——深熄态第 1 次 keep 不点亮，第 2 次才行 | 🔄 | ui-shell |
| 10.4 | 主判据用 `getScreenState()`，**不用** `getBrightness()`（后者返回配置档位不是实际 bl_br） | 🔄 | ui-shell |
| 10.5 | 扫码轮询 / 排队期间也要保活（可能持续几分钟） | 🔄 | ui-shell |
| 10.6 | native 侧暴露 `screenOff` 状态，WSS 心跳断线可诊断 | ⬜ | lead |
| 10.7 | 长测探针跑前续屏（`while true; do hal-screen keep; sleep 20; done &`），结论里记屏幕状态 | 🔄 | 全体 |

**为什么这是独立阶段而不是 UI 的一个小功能**：它同时影响 UI 定时器、native 心跳、
DTLS 超时判定、以及所有真机长测数据的可信度（息屏期间 KMS 平面停止提交，
帧率探针会量出「管线卡死」的假阳性；音频还在播而视频停了，会量出假的音画不同步）。


---

## 并行工作流

```
阶段 0 (✅)
   └─> 阶段 1 骨架
         ├─> 阶段 2 UI ........................ ui-shell
         ├─> 阶段 3 HTTP 协议链 ................ proto-http
         ├─> 阶段 4 信令 + 安全 ................ ws-dtls
         ├─> 阶段 5 媒体（SRTP 加解密） ........ srtp-crypto
         └─> 阶段 5 媒体（解码上屏） ........... media-probe
                     ↓
              阶段 6 输入 → 阶段 7 集成 → 阶段 8 交付
```

**关键路径**：4.6 信令协议 → 4.9 真实服握手 → 7.4 端到端。
信令段是唯一还没有真机验证的环节（因为需要真实 `sdk_param`），是最大不确定点。

---

## 需要用户确认/提供的事项

| 事项 | 说明 | 状态 |
|---|---|---|
| ⚠ **风控风险确认** | 非官方客户端接米哈游云游戏，有账号风险，需用户知情 | README 免责声明覆盖 |
| ⚠ **计费确认** | `paasDispatch` 成功即开始扣时长，调试会消耗云游戏时长 | 已验证 `GET /hk4e_cn/wallet/wallet/get` 可读剩余时长 |
| 🔑 登录方式 | 当前走**扫码登录**（设备无合适输入法）。若需账号密码，需额外做 RSA 加密 + 处理极验（**极验在设备上做不了**，见风险） | 已定 |
| 📱 扫码测试 | 端到端验证时需要用户用米游社 App 扫码 | **待用户配合** |
| 🐙 GitHub | 仓库已建：https://github.com/56dz/PenYS ，GPL-3.0 | 阶段 9 |
| 💡 熄屏 | 词典笔自动 5 分钟熄屏（用户 m00701 提醒） | 阶段 10 |

