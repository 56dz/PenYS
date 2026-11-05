# PenYS 真机端到端验证计划（E2E Plan）

> 目标：把 `docs/feasibility.md` 里尚未真机验证的**信令段**补完，跑通
> 「扫码登录 → 六步调度 → (计费点) → DTLS-SRTP → 视频上屏 → 触摸回环 → 干净退出」。
> 每一步都要留**可复查证据**（截图 / 日志 grep / KMS summary），最终写进 README 现状表。

## 0. 前置条件（每次开跑先做）

| 项 | 命令 / 位置 | 通过判据 |
|---|---|---|
| 设备在线 | `env/adb/adb.exe devices` | 一台 `device` 状态；掉线先重连 |
| 装机 | `powershell -File tools/deploy.ps1`（或手动 push+`miniapp_cli install`+`app_switch`+`start`） | `{"ret":0}`；`/tmp/penys.log` 出现 `ysplayer 1.0.0 module init` |
| 保屏 | `sh -c 'while true; do hal-screen keep >/dev/null 2>&1; sleep 20; done'` | 词典笔 270s 调暗 / 300s 熄屏 / 900s 睡眠，`hal-screen bright_time` 改不动它，**必须 20s 轮询** |
| JS 日志通道 | `adb shell "grep -a '\\[ys' /userdata/applog/DictPen_*.log \| tail -50"` | Falcon console 全在这里（`/tmp/penys.log` 只有 native 横幅） |
| native 日志 | `adb shell "tail -30 /tmp/penys.log"` | `[ys]`/`[ysplayer]` 行 |

## 1. P1 扫码登录链（不扣费）

1. UI 首页 →「重新获取」→ `ys.loginBeginQr()`。
   - 证据：applog 出现 `createQRLogin retcode=0`；UI 显示二维码。
   - 关键一致性：`x-rpc-device_id` 必须与 createQRLogin 完全一致，否则 `-3503 QRLoginDeviceInconsistent`。
2. **真人**用米游社/云原神网页版扫码确认。
3. 轮询 `queryQRLoginStatus`（2000ms）：`data.status` 取值 `Created → Scanned → Confirmed`（首字母大写）。
   - 未扫码原始形状见 `docs/feasibility.md`（`tokens:[]`、`user_info:null`、无 expires_at）。
4. Confirmed 后：`webVerifyForGame` 换 token → `loginByPassword`/`gamer login` 建会话。
   - 证据：cookie jar 落盘（`ys jarDump` 或 `/userdata/penys/` 下文件非空）；`ys.fetchWallet()` 不再是降级 mock。
   - **极验 -3101 不可用**（响应头不可见）→ 触发即记为已知限制，不算失败。
5. 卡点升级：若 `retcode=-3101/-3501/-3208`，按 `docs/feasibility.md` 错误码表处理。

## 2. P2 六步调度链（**不发 paasDispatch，不扣费**）

按 `ys/dispatch.js` 顺序逐个验证，每步留 retcode + 关键字段：

1. `POST /gamer/api/login`（建 session）
2. `GET /gamer/api/getSpeedLimitConfig?speed_client_type=8` → 5 档 `top=9000/4800/3200/1800/1200`，`bit_rate=1024*top`
3. `POST /dispatcher/api/statusCheck {biz_key}`
4. `POST /dispatcher/api/listPingServer {biz_key, ext_data:'{"platform":18}'}`（ext_data 是 **JSON 字符串**）
5. RTT 测速：连 `ping_svr.wss_url` 连发 20 个 `"X"`，取算术平均，1000ms 超时记 -1
6. `POST /dispatcher/api/getNodesInfo {biz_key, node:<JSON字符串>, speed_client_type}` → `region_ids` 顺序**照抄**服务端

通过判据：6 步全 retcode 0，`nodes[0].node_id/region_ids` 拿到。**此阶段绝不调 paasDispatch。**

## 3. P3 paasDispatch（**唯一计费点 — 必须 lead 明确放行**）

- 前置：`ys.fetchWallet()` 读到真实余额/免费时长（当前 `totalTime:0` 是 bug，归 http-fix）。
- 签名源对象 ≠ 最终 body（`docs/feasibility.md` / `docs/pitfalls.md` 有正确样例与错误对照）。
- 发完立刻 `leaveDispatchQueue`（`examples/dispatch.js` 的做法：paasDispatch 成功即开始计费）。
- 证据：`result_code=FINISHED`、`finish_result.sdk_param` base64 长度 1169~1253 字节、能解出字段 1/4/5/8/9/11/14。
- **未获 lead 明确指令前，任何测试代码/队友都不得调用 `dispatchPaas`。**

## 4. P4 信令 + 媒体（`session.c`，已迁移到 `sig_t`）

1. `sig_create`（fingerprint 先建）+ `sig_open` → `wss://<game_server_wss_url>/rtc-sdk`
2. 收 type=3 应答 → 服务端 offer → `on_sig_offer` 只设 flag → `sig_answer_auto`
3. DTLS 握手（复用同一 `dtls_client_t`，**不得重建第二张证书**）
4. SRTP 收方向用 **server_write_key**；UDP 打洞地址 = `game_svr_addr` + `rtc_udp_port`（无 STUN/TURN）
5. 视频：`ys_video_start` → KMS summary 出现 `Esmart1-win0: ACTIVE, zpos 2`，`buf[0]: addr` 非 0
6. 首帧：`status().state==='playing'`、`fps>0`、`gameW/gameH` 非 0

## 5. P5 触摸回环

- `nx=(clientX-rect.x)/rect.w` **不钳位**（归一化 [0,1]，相对**适配后的视频矩形**，不是 box）。
- 证据：点按后 applog 有发送计数增长；游戏内角色有响应（真人观察）。
- 竖屏旋转分支（`docs/feasibility.md` 触摸段）在 960x266 横向画布下**不启用**，保留注释即可。

## 6. P6 稳定性与退出清理

- 连续 30 分钟：每 20s 续屏；`status()` 轮询无 `lastError`；内存（`VmRSS`）平稳。
- 熄屏/亮屏切换：`screenOff` 浮层提示 + `screen.on()` 后能恢复。
- 退出（返回/`onHide`/`onUnload`）：`await player.stop()` → KMS summary `Video Port0: DISABLED`（**没有残留平面盖住 UI**），`sig_close`/`sig_destroy`，进程可再进。
- 二次进入：重复 P1~P4，验证 cookie jar 复用（无需重新扫码）。

## 7. 证据归档

- 截图：`miniapp_cli capture` → `profiles/evidence/`（注意：**只截 UI 层，不含 KMS 视频平面**，视频证据只能靠 KMS summary + 真人观察）。
- 日志：`/userdata/applog/DictPen_*.log`（Falcon console）、`/tmp/penys.log`（native）。
- KMS：`dumpsys`/`gst` summary 的 `Esmart1-win0` 行 + dst rect。
- 全部结果回填 `README.md` 现状表与 `docs/tasks.md` 的 5.x 行。

## 8. 失败升级顺序

1. 先看 applog 最后 50 行（JS 层错误全在这）
2. 再看 `/tmp/penys.log`（native 状态机 / 管线）
3. KMS summary（平面是否 ACTIVE、dst 是否等于适配矩形含 107 偏移）
4. 都正常但画面不对 → 查 `panel_x = 107 + logical_y` 这条换算是否被改坏
