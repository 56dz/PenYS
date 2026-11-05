/* cmd.h -- the command vocabulary between the JS thread and the one worker.
 *
 * WHY A COMMAND QUEUE AT ALL
 * The JSAPI thread must never block. Every JS call (start/stop/touch/setRect)
 * drops a fixed-size command into core/queue.h and returns in microseconds.
 * The worker owns everything else: sockets, DTLS, GStreamer, the KMS plane.
 *
 * WHY THE PAYLOADS ARE SMALL AND FIXED
 * ys_cmd_t already carries 2048 bytes inline and push() never allocates. Every
 * command body below must therefore fit in that budget. The only variable-size
 * one is START, which carries a base64 sdk_param: measured on real dispatches
 * that is 1169-1253 bytes of protobuf, i.e. ~1700 bytes of base64, which fits
 * with room to spare. A longer one is rejected at the JSAPI boundary with a
 * clear error rather than truncated here.
 *
 * WHY TOUCH IS ITS OWN TYPE AND NOT MERGED WITH MOVE
 * Touch is the highest-rate command (a drag can hit 120 events/s) and it is the
 * only one that may legitimately be DROPPED when the ring is full: losing one
 * MOVE in a drag is invisible, whereas losing a DOWN breaks the finger state
 * machine on the server. So the worker treats DOWN/UP as must-deliver and MOVE
 * as droppable, which it can only do if it can tell them apart before parsing.
 */
#ifndef YS_SESSION_CMD_H
#define YS_SESSION_CMD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound on the base64 sdk_param accepted by START. The queue payload is
 * 2048 and the struct overhead is ~40 bytes, so 1980 is safe. */
#define YS_MAX_SDK_PARAM_B64 1980

typedef enum {
    YS_CMD_NONE = 0,

    /* Start a session. Reply is observed via status(), not via a return value:
     * the worker may be busy and the JS side polls. */
    YS_CMD_START = 1,

    /* Stop and return to idle. Must be accepted in EVERY state, including
     * mid-handshake and mid-connect. Idempotent. */
    YS_CMD_STOP = 2,

    YS_CMD_PAUSE = 3,
    YS_CMD_RESUME = 4,

    /* Change the video viewport. Separate from START because the UI toggles
     * between "fullscreen video" and "video + control bar" at runtime. */
    YS_CMD_SET_RECT = 5,

    /* One touch sample. See input/touch.h for the coordinate space. */
    YS_CMD_TOUCH = 6,

    /* Hard reset: stop, tear down, clear all error state. Used on logout. */
    YS_CMD_RELEASE = 7
} ys_cmd_type;

/* START body. `sdk` is NUL terminated, so the struct is variable-length in
 * practice; senders must send sizeof(header) + strlen(sdk) + 1 and receivers
 * must bounds-check against cmd->len. */
typedef struct {
    int32_t rect_x, rect_y, rect_w, rect_h;   /* logical canvas coords */
    int32_t sdk_len;                          /* excludes the NUL */
    char    sdk[1];                           /* sdk_len bytes + NUL */
} ys_cmd_start;

typedef struct {
    int32_t rect_x, rect_y, rect_w, rect_h;
} ys_cmd_rect;

/* Packed deliberately small: it travels at up to 120 Hz and the queue slot is
 * a full 2048 bytes regardless, but keeping it 16 bytes makes the copy cheap. */
typedef struct {
    float   nx, ny;      /* normalized [0,1] inside the video rect */
    int32_t action;      /* 0=down 1=up 2=move */
    int32_t finger_id;   /* caller's stable identifier, NOT the wire slot */
} ys_cmd_touch;

/* Convenience: the byte length of a START command for a given sdk_param. */
static inline int ys_cmd_start_len(int sdk_len) {
    return (int)(sizeof(ys_cmd_start) + (sdk_len > 0 ? sdk_len : 0));
}

#ifdef __cplusplus
}
#endif
#endif /* YS_SESSION_CMD_H */
