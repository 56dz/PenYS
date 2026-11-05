#ifndef YS_TOUCH_H
#define YS_TOUCH_H
/*
 * 云·原神 touch input encoder.
 *
 * The wire format was reverse-engineered from cg-sdk; it is NOT a documented
 * protocol. Three things about it are easy to get wrong and are load-bearing:
 *
 *  1. Coordinates are normalised [0,1] relative to the GAME RECT, not pixels
 *     and not absolute screen space. Sending pixels silently moves touches to
 *     a corner -- the game does not reject out-of-range values, it clamps.
 *  2. proto3 does not serialise default values, so finger_id==0, action==Down
 *     and is_primary==false produce NO bytes at all. A decoder must therefore
 *     restore defaults; an encoder must not "helpfully" emit them.
 *  3. `primary` is true only while the FIRST finger is down. It is not "this
 *     finger is the only one" -- moving a second finger must not flip it.
 *
 * Frame layout (big-endian, 12-byte minimum):
 *   [0..1]  0x45 0x67          magic head (17767)
 *   [2..3]  cmd_id = 20010
 *   [4..5]  head_len           PacketHead length; the real SDK sends 0
 *   [6..9]  msg_len
 *   [10..]  head (head_len bytes) then msg
 *   [-2..]  0x89 0xAB          magic tail (35243)
 */
#include <stdint.h>

#define YS_TOUCH_CMD_ID 20010u
#define YS_TOUCH_MAX_FINGERS 10
#define YS_TOUCH_MAX_PACKET 512

typedef enum {
    YS_TOUCH_DOWN = 0,
    YS_TOUCH_UP = 1,
    YS_TOUCH_MOVE = 2
} ys_touch_action;

typedef struct {
    int finger_id;   /* 0..9, from the slot pool */
    int action;      /* ys_touch_action */
    float x;         /* normalised [0,1] within the game rect */
    float y;
    int is_primary;
} ys_touch_event;

/*
 * Session state for the finger-slot pool. Touch identifiers from the browser /
 * runtime are arbitrary and may be reused after release, so we keep our own
 * mapping to a dense 0..9 pool exactly like the official SDK does.
 */
typedef struct {
    int slot_finger_id[YS_TOUCH_MAX_FINGERS]; /* identifier -> finger id, -1 = free */
    int slot_active[YS_TOUCH_MAX_FINGERS];
    int ident[YS_TOUCH_MAX_FINGERS];
    float last_x[YS_TOUCH_MAX_FINGERS];
    float last_y[YS_TOUCH_MAX_FINGERS];
    int count;
    int primary_finger_id; /* -1 when no finger is down */
} ys_touch_state;

void ys_touch_reset(ys_touch_state *st);

/*
 * Feed one input event. `ident` is the runtime's pointer identifier.
 * nx/ny are already normalised against the game rect by the caller, because
 * only the caller knows the current layout (and whether the panel is rotated).
 * Returns the number of bytes written to out, or 0 if the event was ignored
 * (e.g. an Up for an unknown identifier), or -1 on error.
 */
int ys_touch_feed(ys_touch_state *st, int ident, int action, float nx, float ny,
                  unsigned char *out, int cap);

/* Build a packet from an explicit event list (used by tests and by "release all"). */
int ys_touch_build(const ys_touch_event *evs, int n, unsigned char *out, int cap);

#endif
