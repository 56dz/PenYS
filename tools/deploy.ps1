# PenYS 真机部署与验证（有道 X6 Pro）
#
# 与 PenDouyu / PenBili 的 deploy.ps1 同构，差别只在 appid、包名、日志标签、残留指纹。
#
# 流程：
#   1) 设备与产物检查（AMR + native ELF 架构）
#   2) 安装（miniapp_cli 必须返回 ret: 0）+ 核对 slot 里 .so 是否真的落位
#   3) 切桌面 → 冷启动到 index（先切走再启动，保证重建页面实例）
#   4) 取证：capture 整页 + [ys] / [ysplayer] 日志 + /tmp/penys.log
#   5) 进出循环：退出后 KMS 视频平面 / 子进程 / FIFO 残留必须为 0
#   6) 汇总，证据落 profiles/evidence/
#
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools/deploy.ps1
param(
  [string]$StartPage = 'index',
  # 云游戏一局要扫码登录，默认不自动进出循环；需要时加 -Loop 手动验证泄漏
  [switch]$Loop,
  [int]$WaitSec = 15
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# adb 定位：$env:PENYS_ADB → PATH 中的 adb → 本机开发布局（../env/adb）
$adb = $env:PENYS_ADB
if (-not $adb) { $c = Get-Command adb -ErrorAction SilentlyContinue; if ($c) { $adb = $c.Source } }
if (-not $adb) { $cand = Join-Path $root '..\env\adb\adb.exe'; if (Test-Path $cand) { $adb = (Resolve-Path $cand).Path } }
if (-not $adb) { throw '找不到 adb：请设置 PENYS_ADB 指向 adb.exe，或将 adb 加入 PATH' }

$appid = '8002026110200003'
$desktop = '8080222437664451'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$ev = Join-Path $root 'profiles\evidence'
New-Item -ItemType Directory -Force -Path $ev | Out-Null

function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

function Pull-Capture([string]$remote, [string]$local) {
  & $adb pull $remote $local 2>&1 | Out-Null
  if ((Test-Path $local) -and ((Get-Item $local).Length -gt 0)) {
    Write-Host "evidence: $local ($([math]::Round((Get-Item $local).Length/1KB,1)) KB)"
  } else {
    Write-Host "MISSING/EMPTY: $local" -ForegroundColor Yellow
  }
}

# 云游戏的残留指纹和斗鱼不同：没有 ffmpeg/curl 子进程（信令和 RTP 都在本进程里），
# 所以要看的是
#   - KMS 视频平面是否还 ACTIVE（最硬的证据：画面还在屏上就是没收拾干净）
#   - 本进程是否还活着
#   - DTLS/UDP socket 是否还被占着（用 /proc/net/udp 里目标端口的字节数粗查）
function Residue() {
  $plane = & $adb shell "cat /sys/kernel/debug/dri/0/summary 2>/dev/null | grep -c 'Video Port0: ACTIVE'"
  $proc  = & $adb shell "pidof miniapp | wc -l"
  $child = & $adb shell "ps | grep -c '[j]sapi_ysplayer'"
  Write-Host "残留: kms_active=$plane miniapp=$proc child=$child (期望 0/1/0)"
  return @{ plane = "$plane".Trim(); child = "$child".Trim() }
}

Step '1/6 设备'
& $adb devices -l
$devs = & $adb devices | Select-String 'device$'
if (-not $devs) { throw '没有已连接的设备（adb devices 为空）' }

Step '2/6 产物与 native 检查'
$amr = Get-ChildItem -Path $root -Filter 'PenYS*.amr' -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $amr) {
  $amr = Get-ChildItem -Path $root -Filter "$appid.*.amr" -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if (-not $amr) { throw '找不到 AMR 包，请先 npm run build' }
Write-Host "AMR: $($amr.Name) ($([math]::Round($amr.Length/1KB,1)) KB)"

$soPath = Join-Path $root 'libs\libjsapi_ysplayer.so'
if (Test-Path $soPath) {
  python (Join-Path $root 'tools\inspect_elf.py') $soPath --target aarch64 | Select-Object -Last 4
} else {
  Write-Host 'WARN: libs/libjsapi_ysplayer.so 不存在（纯 UI 构建可忽略）' -ForegroundColor Yellow
}

Step '3/6 安装（ret:0 校验）'
& $adb push $amr.FullName /tmp/penys_app.amr 2>&1 | Out-Null
$ins = & $adb shell "miniapp_cli install /tmp/penys_app.amr"
$ins
if ("$ins" -notmatch '"ret":\s*0') { throw "install 未返回 ret:0，实际：$ins" }

# 安装会在 slot a / slot b 之间轮换，必须确认 .so 真的在生效 slot 里
$slots = & $adb shell "find /userdisk/miniapp/data/mini_app/pkg/$appid -maxdepth 3 -name '*.so' 2>/dev/null"
Write-Host "已落位的 .so："
$slots

Step '4/6 冷启动（index）'
& $adb shell "miniapp_cli start $desktop" | Out-Null
Start-Sleep -Seconds 2
$st = & $adb shell "miniapp_cli start $appid $StartPage"
$st
Start-Sleep -Seconds $WaitSec

$cap = "/tmp/penys_${stamp}.png"
& $adb shell "miniapp_cli capture $cap"
Pull-Capture $cap (Join-Path $ev "app_${stamp}.png")

# 三层日志：QuickJS console.warn / native [ysplayer] / 我方 /tmp/penys.log
$log1 = & $adb shell "grep -hE '\[ys\]|\[ysplayer\]' /userdata/applog/DictPen_*.log 2>/dev/null | tail -n 80"
$log1
$log1 | Out-File -FilePath (Join-Path $ev "app_${stamp}_log.txt") -Encoding utf8

$log2 = & $adb shell "tail -n 120 /tmp/penys.log 2>/dev/null"
$log2
$log2 | Out-File -FilePath (Join-Path $ev "app_${stamp}_native.txt") -Encoding utf8

Step '5/6 残留检查'
$r1 = Residue

if ($Loop) {
  Step '5b/6 进出循环：退出 → 残留 → 再进入'
  & $adb shell "miniapp_cli start $desktop" | Out-Null
  Start-Sleep -Seconds 3
  $r2 = Residue
  if ($r2.plane -ne '0') { Write-Host 'FAIL: 退出后 KMS 视频平面仍 ACTIVE（画面会留在屏上）' -ForegroundColor Red }
  if ($r2.child -ne '0') { Write-Host 'FAIL: 退出后仍有子进程残留' -ForegroundColor Red }
  & $adb shell "miniapp_cli start $appid $StartPage" | Out-Null
  Start-Sleep -Seconds 5
  $cap2 = "/tmp/penys_${stamp}_b.png"
  & $adb shell "miniapp_cli capture $cap2"
  Pull-Capture $cap2 (Join-Path $ev "app_${stamp}_reenter.png")
}

Step '6/6 汇总'
Write-Host "证据目录: $ev" -ForegroundColor Green
Write-Host "冷启动后平面/进程: kms_active=$($r1.plane) miniapp=$($r1.proc)"
Write-Host @"

手工验收清单（deploy.ps1 只能覆盖前 3 条）：
  [ ] 扫码登录 → 主页显示昵称与剩余时长
  [ ] 点开始游戏 → 调度进度条走完 → 出画面
  [ ] 画面能触摸操作（点击/拖动），坐标不偏移
  [ ] 声音出来，不是静音
  [ ] 暂停 / 继续 / 退出返回主页
  [ ] 退出后 /sys/kernel/debug/dri/0/summary 里 Video Port0 回到 DISABLED
"@ -ForegroundColor Cyan
