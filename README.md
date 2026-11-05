# PenYS · 云·原神 词典笔客户端

有道词典笔 X6 Pro（RK3562 / Falcon mini-app）上的《云·原神》客户端。

> ⚠ **非官方客户端**。本项目通过逆向的米哈游云游戏协议接入，与米哈游官方无关。
> 使用存在账号风险与计费消耗，请自行评估。

---

## 项目现状

**阶段 0（可行性验证）已完成**，核心结论：

| 能力 | 结论 |
|---|---|
| 米哈游 HTTP 接口直连 | ✅ 真机 `createQRLogin` 返回 `retcode:0` |
| DTLS 1.2 + SRTP 密钥导出 | ✅ 真机握手成功，协商 `SRTP_AES128_CM_SHA1_80` |
| WSS 建连 + RTT 测速 | ✅ 真机 101 升级，256ms 建连 / 39ms RTT |
| 硬解上屏 `mppvideodec ! kmssink` | ✅ 可用 |
| WebRTC（现成方案） | ❌ 设备无 `webrtcbin`/libnice/libsrtp/libsctp，**必须自建** |

详见 [docs/feasibility.md](docs/feasibility.md) 与 [docs/tasks.md](docs/tasks.md)。

---

## 目录结构

```
PenYS/
├── docs/
│   ├── feasibility.md      可行性报告（真机证据）
│   ├── tasks.md            任务清单
│   └── protocol-notes.md   米哈游协议要点与踩坑
├── native/
│   ├── build.ps1           zig 交叉编译（aarch64）
│   ├── iot-miniapp-sdk/    Falcon JSAPI 头文件
│   ├── stubs-aarch64/      设备库 import stubs（仅链接，不打包）
│   ├── stubs-aarch64-gst/  GStreamer/GLib import stubs
│   └── csrc/
│       ├── ossl_min.h      手写 OpenSSL 1.1.1l 声明（设备无头文件）
│       ├── dtls_probe.c    【验证用】DTLS-SRTP 握手探针
│       ├── core/           日志 / 队列 / 线程
│       ├── session/        单 worker 状态机
│       ├── net/            HTTP(libcurl) / WSS
│       ├── webrtc/         DTLS / SDP
│       ├── rtp/            SRTP / RTP 重组
│       ├── video/          KMS 平面
│       ├── audio/          ALSA 输出
│       └── input/          触摸报文
├── src/                    Falcon Vue 2 前端
└── tools/
    ├── inspect_elf.py      ELF 校验（ELFCLASS64 / e_machine 183）
    └── deploy.ps1          打包 + 安装 + 真机验证
```

---

## 构建

```sh
# 1) native（交叉编译到 aarch64）
powershell -File native/build.ps1

# 2) 前端（QuickJS AMR）
node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p

# 3) 部署到设备
powershell -File tools/deploy.ps1
```

要求：
- Node.js（构建前端）、`env/pyzig/ziglang/zig.exe`（zig 0.16+）
- adb（`env/adb/adb.exe`），设备需已 root 并授权

---

## 设备约束（X6 Pro 实测）

- 屏幕：物理 **480×960 竖屏，DRM rotation=1**；Falcon 合成面 **960×266**（超宽横条）
- SoC：RK3562，4×A35，983MB RAM（约 288MB 可用）
- GStreamer 1.22.0，`mppvideodec` 硬解，**无任何可用编码器**
- OpenSSL **1.1.1l**（无头文件，需手写声明）
- 无 python / node / openssl CLI / nm / ldd；有 `strace` / `gdb` / `curl` / `ffmpeg`

---

## 已知限制

1. **未与真实游戏服完成端到端握手**：SDP answer 逻辑只对构造的 offer 做过 round-trip，尚未接真实 `sdk_param`。
2. **A/V 同步未验证**：需要 RTP 时间戳 + RTCP SR 对齐，尚无实流数据。
3. **极验（Aigis）无法处理**：设备上拿不到响应头，账号密码登录遇 `-3101` 即不可用。当前只支持扫码登录。
4. **视频平面会覆盖 UI**：云游戏画面走 KMS 独立平面，控制条需要靠 native 决定何时隐藏平面才可见（见 media 报告结论）。

---

## 许可

仅供学习与研究。不包含任何米哈游的素材、资源或代码。
