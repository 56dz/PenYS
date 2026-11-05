#!/usr/bin/env python3
"""Verify the C touch encoder against the one authoritative vector we have.

The authoritative vector comes from the reverse-engineering notes
(docs/pitfalls.md item 6): finger=0, Down, (0.5, 0.5), primary=true

    message payload -> 08156a0e0a0c1d0000003f250000003f3801   (18 bytes)

Note this is the *message* payload, not the whole frame. The full frame we emit
is 30 bytes: 10-byte big-endian header + 18-byte payload + 2-byte magic tail.

The vector must match byte for byte, including the ABSENCE of the finger_id and
action_type bytes: proto3 does not serialise default values, so finger_id=0 and
action=Down contribute nothing. If the encoder ever starts emitting them the
packet still "works", but it diverges from the official SDK on the wire, which
is exactly the kind of drift that surfaces later as a mystery input desync.
"""
import os
import subprocess
import sys
import tempfile

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "input/touch.h"

static void dump(const unsigned char *b, int n) {
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

/* Print the message payload: skip the 10-byte header, stop before the 2-byte tail. */
static void dump_msg(const unsigned char *b, int n) {
    int msglen = (b[6] << 24) | (b[7] << 16) | (b[8] << 8) | b[9];
    int headlen = (b[4] << 8) | b[5];
    if (10 + headlen + msglen + 2 != n) { printf("BADLEN\n"); return; }
    dump(b + 10 + headlen, msglen);
}

int main(void) {
    unsigned char out[YS_TOUCH_MAX_PACKET];

    /* vector 1: single finger 0, DOWN, (0.5,0.5), primary */
    ys_touch_event e = { 0, YS_TOUCH_DOWN, 0.5f, 0.5f, 1 };
    int n = ys_touch_build(&e, 1, out, sizeof(out));
    printf("vec1_frame ");
    dump(out, n);
    printf("vec1_msg ");
    dump_msg(out, n);
    printf("vec1_len %d\n", n);

    /* vector 2: finger 3, UP, same coords, not primary */
    ys_touch_event e2 = { 3, YS_TOUCH_UP, 0.5f, 0.5f, 0 };
    n = ys_touch_build(&e2, 1, out, sizeof(out));
    printf("vec2_msg ");
    dump_msg(out, n);

    /* vector 3: state machine - down, move, up on identifier 7 */
    ys_touch_state st;
    ys_touch_reset(&st);
    n = ys_touch_feed(&st, 7, YS_TOUCH_DOWN, 0.25f, 0.75f, out, sizeof(out));
    printf("vec3_down_msg ");
    dump_msg(out, n);
    n = ys_touch_feed(&st, 7, YS_TOUCH_MOVE, 0.30f, 0.70f, out, sizeof(out));
    printf("vec3_move_msg ");
    dump_msg(out, n);
    n = ys_touch_feed(&st, 7, YS_TOUCH_UP, 1.50f, -0.5f, out, sizeof(out));
    printf("vec3_up_msg ");
    dump_msg(out, n);
    printf("vec3_primary_after_up %d\n", st.primary_finger_id);

    /* vector 4: clamping - out of range coordinates clamp into 0..1 */
    ys_touch_event e4 = { 1, YS_TOUCH_MOVE, 2.0f, -3.0f, 0 };
    n = ys_touch_build(&e4, 1, out, sizeof(out));
    printf("vec4_unclamped_feed ");
    ys_touch_state st4;
    ys_touch_reset(&st4);
    ys_touch_feed(&st4, 5, YS_TOUCH_DOWN, 0.5f, 0.5f, out, sizeof(out));
    n = ys_touch_feed(&st4, 5, YS_TOUCH_MOVE, 2.0f, -3.0f, out, sizeof(out));
    dump_msg(out, n);

    /* vector 5: two-finger down keeps primary on the first */
    ys_touch_reset(&st);
    ys_touch_feed(&st, 1, YS_TOUCH_DOWN, 0.1f, 0.1f, out, sizeof(out));
    printf("primary_first %d\n", st.primary_finger_id);
    ys_touch_feed(&st, 2, YS_TOUCH_DOWN, 0.2f, 0.2f, out, sizeof(out));
    printf("primary_second_still %d\n", st.primary_finger_id);
    printf("active_count %d\n", st.count);

    /* vector 6: unknown identifier Up must be ignored, not crash */
    ys_touch_reset(&st);
    n = ys_touch_feed(&st, 99, YS_TOUCH_UP, 0.5f, 0.5f, out, sizeof(out));
    printf("unknown_up_returns %d\n", n);
    return 0;
}
"""


def main():
    # This script lives in <workspace>/PenYS/tools/, so the workspace root is two
    # levels up and the project root is one level up.
    proj = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    root = os.path.abspath(os.path.join(proj, ".."))
    inc = os.path.join(proj, "native", "csrc")
    cf = os.path.join(proj, "native", "csrc", "input", "touch.c")

    # The dev box has no gcc/clang; zig compiles for the host when no -target
    # is given, so use it. This test is pure arithmetic + string.h anyway.
    zig = os.path.join(root, "env", "pyzig", "ziglang", "zig.exe")
    cands = []
    if os.path.exists(zig):
        cands.append([zig, "cc"])
    cands += [["gcc"], ["cc"], ["clang"]]

    with tempfile.TemporaryDirectory() as td:
        h = os.path.join(td, "h.c")
        with open(h, "w") as f:
            f.write(HARNESS)
        exe = os.path.join(td, "t.exe")
        built = False
        for cc in cands:
            try:
                r = subprocess.run(cc + ["-O1", "-I", inc, h, cf, "-o", exe],
                                   capture_output=True, text=True)
            except FileNotFoundError:
                continue
            if r.returncode == 0:
                built = True
                break
        if not built:
            print("no host C compiler found (tried zig cc, gcc, cc, clang)")
            return 2

        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode != 0:
            print("harness failed:", r.stderr)
            return 1
        print(r.stdout)
        out = {}
        for line in r.stdout.strip().splitlines():
            if " " in line:
                k, v = line.split(" ", 1)
                out[k] = v.strip()

        expected = "08156a0e0a0c1d0000003f250000003f3801"
        got = out.get("vec1_msg", "")
        print(f"vec1 expected {expected}")
        print(f"vec1 got      {got}")
        if got != expected:
            print("VECTOR MISMATCH")
            return 1
        print("VECTOR MATCH")

        # Frame envelope: 10-byte header + msg + 2-byte tail.
        frame = out.get("vec1_frame", "")
        if not (frame.startswith("4567") and frame.endswith("89ab")):
            print("FAIL: frame magic head/tail wrong:", frame)
            return 1
        if int(out.get("vec1_len", "0")) != 30:
            print("FAIL: frame length is not 30:", out.get("vec1_len"))
            return 1
        # cmd_id 20010 = 0x4e2a, big-endian at [2..3]; head_len 0.
        if frame[4:8] != "4e2a":
            print("FAIL: cmd id bytes wrong:", frame[4:8])
            return 1
        if frame[8:12] != "0000":
            print("FAIL: head len not zero:", frame[8:12])
            return 1
        if frame[12:20] != "00000012":
            print("FAIL: msg len (BE u32) not 0x12:", frame[12:20])
            return 1
        print("FRAME ENVELOPE OK")

        if out.get("primary_first") != "0":
            print("FAIL: primary not assigned to the first finger")
            return 1
        if out.get("primary_second_still") != "0":
            print("FAIL: primary moved when a second finger went down")
            return 1
        if out.get("active_count") != "2":
            print("FAIL: two fingers down but count is", out.get("active_count"))
            return 1
        if out.get("vec3_primary_after_up") != "-1":
            print("FAIL: primary not cleared after all fingers went up")
            return 1
        if out.get("unknown_up_returns") != "0":
            print("FAIL: unknown identifier Up was not ignored")
            return 1
        print("STATE MACHINE OK")

        # Clamping: 2.0 -> 1.0 and -3.0 -> 0.0. Fixed32 is little-endian on the
        # wire, so 1.0f appears as 00 00 80 3f, and 0.0f is elided by proto3
        # (which is itself the proof that the clamp produced an exact zero).
        v4 = out.get("vec4_unclamped_feed", "")
        if "0000803f" not in v4:
            print("FAIL: x=2.0 was not clamped to 1.0:", v4)
            return 1
        if "25" in v4[8:]:
            print("FAIL: y=-3.0 was not clamped to exactly 0.0 (still encoded):", v4)
            return 1
        print("CLAMP OK")

        print("ALL PASS")
        return 0


sys.exit(main())
