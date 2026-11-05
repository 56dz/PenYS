param(
  [string]$Target = ''
)
$ErrorActionPreference = 'Stop'
# Cross-compile the PenYS native module with the zig toolchain.
#
#   libjsapi_ysplayer.so = 云原神 播放/信令/输入引擎 (X6 Pro only, aarch64)
#
# Why one .so instead of several: the whole cloud-game session is one state
# machine owning one worker thread, one UDP socket, one DTLS session, one
# GStreamer pipeline and one KMS plane. Splitting it across .so files would
# mean either a shared global registry (fragile) or IPC (pointless cost).
#
# Target arch (confirm before build!):
#   X6 Pro (melon_pro, fw4.3.5): aarch64 64-bit
#   X5  (youdao-x5)            : armv7 hard-float 32-bit -- NOT SUPPORTED here:
#                                X5 has no GStreamer, no /dev/dri, and no MPP,
#                                so the cloud-game pipeline cannot exist on it.
if (-not $Target) {
  $Target = if ($env:PENYS_TARGET) { $env:PENYS_TARGET } else { 'aarch64' }
}
if ($Target -ne 'aarch64') {
  Write-Error "PenYS only supports aarch64 (X6 Pro). The X5 lacks GStreamer/MPP/KMS."
}

$base = Split-Path -Parent $PSScriptRoot
$zig = $env:ZIG_EXE
if (-not $zig) {
  $cand = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'env/pyzig/ziglang/zig.exe'
  if (Test-Path $cand) { $zig = $cand }
}
if (-not $zig -or -not (Test-Path $zig)) {
  Write-Error 'zig not found: set ZIG_EXE to zig.exe (ziglang.org, or pip install ziglang)'
}
$cache = if ($env:ZIG_CACHE) { $env:ZIG_CACHE } else { Join-Path $env:TEMP 'penys-zig-cache' }
$env:ZIG_GLOBAL_CACHE_DIR = $cache
$env:ZIG_LOCAL_CACHE_DIR = $cache + '-local'

$stubDir = Join-Path $base 'native/stubs-aarch64'
$gstStubDir = Join-Path $base 'native/stubs-aarch64-gst'
foreach ($d in @($stubDir, $gstStubDir)) {
  if (-not (Test-Path $d)) { Write-Error "stub dir not found: $d" }
}
$libsDir = Join-Path $base 'libs'
if (-not (Test-Path $libsDir)) { New-Item -ItemType Directory -Path $libsDir | Out-Null }

$common = @(
  # -g0: no DWARF (absolute build paths would otherwise land in the .so)
  # -Wl,-s: strip symtab but keep .dynsym (custom_init_jsapis stays exported)
  # -fvisibility=hidden: only JQUICK_EXPORT symbols leave the .so
  'cc', '-target', 'aarch64-linux-gnu.2.17', '-O2', '-g0', '-Wl,-s',
  '-fPIC', '-shared', '-fvisibility=hidden',
  '-I', (Join-Path $base 'native/iot-miniapp-sdk/include'),
  '-I', (Join-Path $base 'native/csrc')
)

# ---- Source list is explicit (no wildcards) so a stray file can never ship ----
# Grouped by role, in dependency order. A file that exists but is missing from
# this list will not be compiled at all -- deliberate: a .so whose behaviour
# silently differs from the tree is worse than a failed build.
# Note: session/cmd.h is header-only (inline helpers), so there is no cmd.c;
# audio output lives in video/video_pipe.c because it shares the pipeline clock.
$srcNames = @(
  # the single custom_init_jsapis() entry point
  'jsapi_ysplayer.c'

  # infra
  'core/log.c'
  'core/queue.c'
  'core/thread.c'

  # session state machine (the only owner of the pipeline)
  'session/session.c'

  # HTTP / crypto / websocket JSAPI (yshttp + ysws) and the primitives under them
  'net/ys_jsapi.c'
  'net/ys_http.c'
  'net/ys_crypto.c'
  'net/ys_ws.c'

  # security: websocket client, signalling state machine, DTLS handshake,
  # SDP answer, SRTP, RTP depay
  'webrtc/wss.c'
  'webrtc/sig.c'
  'webrtc/dtls.c'
  'webrtc/sdp.c'
  'webrtc/stun.c'
  'rtp/srtp.c'
  'rtp/rtp_reorder.c'

  # output: decode + present + audio for one session
  'video/video_pipe.c'

  # input
  'input/touch.c'
)

$csrc = Join-Path $base 'native/csrc'
$present = @()
$missing = @()
foreach ($n in $srcNames) {
  $f = Join-Path $csrc $n
  if (Test-Path $f) { $present += $f } else { $missing += $n }
}
if ($missing.Count -gt 0) {
  Write-Host "skipping not-yet-written sources: $($missing -join ', ')" -ForegroundColor Yellow
}
if ($present.Count -eq 0) { Write-Error 'no sources found under native/csrc' }

$args1 = $common + $present + @(
  # Device import stubs: SONAME only, never shipped to the device.
  (Join-Path $gstStubDir 'libgstreamer-1.0.so.0'),
  (Join-Path $gstStubDir 'libgobject-2.0.so.0'),
  (Join-Path $gstStubDir 'libglib-2.0.so.0'),
  (Join-Path $gstStubDir 'libgmodule-2.0.so.0'),
  (Join-Path $stubDir 'libquickjs.so'),
  (Join-Path $stubDir 'libssl.so.1.1'),
  (Join-Path $stubDir 'libcrypto.so.1.1'),
  (Join-Path $stubDir 'libcurl.so.4'),
  (Join-Path $stubDir 'libcares.so.2'),
  (Join-Path $stubDir 'libz.so.1'),
  '-lpthread',
  '-Wl,-soname,libjsapi_ysplayer.so',
  '-o', (Join-Path $libsDir 'libjsapi_ysplayer.so')
)
& $zig @args1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Get-ChildItem (Join-Path $libsDir '*.so') | Select-Object Name, Length

# ELF structure / export check (mandatory before packaging into the AMR).
# inspect_elf.py validates EI_CLASS + e_machine: aarch64 must be ELFCLASS64/183.
# A 32-bit .so on X6 Pro fails at runtime with
#   "wrong ELF class: ELFCLASS32:dlopen in dynamic_load_jsapi"
python (Join-Path $base 'tools/inspect_elf.py') (Join-Path $libsDir 'libjsapi_ysplayer.so') --target aarch64 | Select-Object -Last 6
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
