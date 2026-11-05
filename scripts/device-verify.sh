#!/bin/sh
# device-verify.sh -- one-shot device verification of the media/signalling path.
#
# What it does: install the freshly packed .amr, clear the native log, start the
# app, and print the handful of lines that tell you whether the ICE/DTLS layer
# moved. Run it from the workspace root (the directory that contains penys/ and
# env/), while the pen is connected over adb.
#
#   sh PenYS/scripts/device-verify.sh
#
# Why a script: the sequence has four easy-to-get-wrong details that cost real
# time on 2026-11-05 --
#   * MSYS rewrites /tmp/... into C:/... on the way to adb (hence MSYS_NO_PATHCONV),
#   * the log switch file /tmp/penys_log is wiped by a reinstall,
#   * `miniapp_cli` has no `stop`; a reinstall is what restarts the app,
#   * the page name must NOT carry the "--" prefix (`--index` makes QuickJS look
#     for a module named "--index.js" and the app paints a black screen).
#
# Pressing "开始游戏" on the device still requires a human: touch injection does
# not work here because weston owns /dev/input/event1. For unattended runs add
# the DEV_AUTOSTART hook described in docs/signalling-and-media.md, rebuild, and
# delete it again before shipping.

set -e

ADB="${ADB:-./env/adb/adb.exe}"
AMR="${AMR:-PenYS/8002026110200003.1_0_0.amr}"
APPID="${APPID:-8002026110200003}"
PAGE="${PAGE:-index}"
WAIT="${WAIT:-25}"

if [ ! -x "$ADB" ] && [ ! -f "$ADB" ]; then
    echo "adb not found at $ADB (set ADB=/path/to/adb)" >&2
    exit 1
fi
if [ ! -f "$AMR" ]; then
    echo "package not found at $AMR (build it with: cd PenYS && node ../env/node_modules/aiot-vue-cli/src/cli.js -q -p)" >&2
    exit 1
fi

echo "== device"
MSYS_NO_PATHCONV=1 "$ADB" devices
if ! MSYS_NO_PATHCONV=1 "$ADB" shell true >/dev/null 2>&1; then
    echo "no device: power the pen on and re-run (the device must be unlocked: 'adb shell auth' is a human step)" >&2
    exit 1
fi

echo "== push $AMR"
MSYS_NO_PATHCONV=1 "$ADB" push "$AMR" /tmp/penys.amr >/dev/null

echo "== install (this also restarts a running instance)"
MSYS_NO_PATHCONV=1 "$ADB" shell "touch /tmp/penys_log; miniapp_cli install /tmp/penys.amr"

echo "== start page $PAGE"
MSYS_NO_PATHCONV=1 "$ADB" shell "rm -f /tmp/penys.log; miniapp_cli start $APPID $PAGE"

echo "== waiting ${WAIT}s for the session (start the game on the device now)"
sleep "$WAIT"

echo "== native log"
MSYS_NO_PATHCONV=1 "$ADB" shell "ls -l /tmp/penys.log 2>/dev/null || echo 'no log: native module never loaded'"

echo "== the lines that matter"
MSYS_NO_PATHCONV=1 "$ADB" shell "grep -a 'StartGameReq\|StartGameRsp\|type=offer\|answer sent\|local candidate\|udp: connected\|ice: \|dtls:\|session ERROR' /tmp/penys.log" || true

echo
echo "Read it like this:"
echo "  sig: StartGameReq sent ...        -> signalling works, sid/pod/aid all present"
echo "  sig: StartGameRsp retcode=1       -> the service booked a machine for us"
echo "  sig: json type=offer              -> SDP offer arrived (the original bug is gone)"
echo "  udp: connected to A:B (local=C:D)-> candidate now names C:D, not the server"
echo "  ice: reply #0 ...                 -> the edge answered our connectivity check"
echo "  dtls: handshake ok / first frame  -> media path is up"
echo "  still 'ice: no reply'             -> see docs/signalling-and-media.md section 2"
