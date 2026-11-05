#include "input/touch.h"

#include <string.h>

/* ---------- protobuf writer (only the wire types we use) ---------- */

typedef struct {
    unsigned char *p;
    int cap;
    int len;
    int overflow;
} wbuf;

static void wb_byte(wbuf *w, unsigned v) {
    if (w->len >= w->cap) { w->overflow = 1; return; }
    w->p[w->len++] = (unsigned char)(v & 0xff);
}

static void wb_varint(wbuf *w, uint32_t v) {
    while (v > 0x7f) {
        wb_byte(w, (v & 0x7f) | 0x80);
        v >>= 7;
    }
    wb_byte(w, v);
}

static void wb_tag(wbuf *w, int field, int wire) {
    wb_varint(w, ((uint32_t)field << 3) | (uint32_t)wire);
}

/* proto3: a zero value is not serialised. Getting this wrong makes the server
 * see finger_id=0 / action=Down explicitly, which it treats the same way, but
 * it also bloats every packet and diverges from the official SDK on the wire. */
static void wb_uint32(wbuf *w, int field, uint32_t v) {
    if (v == 0) return;
    wb_tag(w, field, 0);
    wb_varint(w, v);
}

static void wb_bool(wbuf *w, int field, int v) {
    if (!v) return;
    wb_tag(w, field, 0);
    wb_varint(w, 1);
}

static void wb_float(wbuf *w, int field, float f) {
    uint32_t bits;
    if (f == 0.0f) return;
    memcpy(&bits, &f, 4);
    wb_tag(w, field, 5);
    /* protobuf fixed32 is little-endian on the wire regardless of host order. */
    wb_byte(w, bits & 0xff);
    wb_byte(w, (bits >> 8) & 0xff);
    wb_byte(w, (bits >> 16) & 0xff);
    wb_byte(w, (bits >> 24) & 0xff);
}

static void wb_bytes(wbuf *w, int field, const unsigned char *b, int n) {
    if (n <= 0) return;
    wb_tag(w, field, 2);
    wb_varint(w, (uint32_t)n);
    for (int i = 0; i < n; i++) wb_byte(w, b[i]);
}

/* ---------- packet framing ---------- */

#define PB_FINGER_ID 1
#define PB_ACTION 2
#define PB_X 3
#define PB_Y 4
#define PB_PRIMARY 7

#define PB_EVENT_LIST 1
#define PB_TOUCH_LIST 13
#define PB_CHAN_TYPE 1
#define PB_DATA_TYPE_TOUCH 21

int ys_touch_build(const ys_touch_event *evs, int n, unsigned char *out, int cap) {
    unsigned char listbuf[YS_TOUCH_MAX_PACKET];
    unsigned char chanbuf[YS_TOUCH_MAX_PACKET];
    wbuf lw = { listbuf, (int)sizeof(listbuf), 0, 0 };
    wbuf cw = { chanbuf, (int)sizeof(chanbuf), 0, 0 };
    int msg_len, total, head_len = 0;

    if (!evs || n <= 0 || n > YS_TOUCH_MAX_FINGERS) return -1;
    if (!out || cap < 12) return -1;

    for (int i = 0; i < n; i++) {
        unsigned char evbuf[64];
        wbuf ew = { evbuf, (int)sizeof(evbuf), 0, 0 };
        wb_uint32(&ew, PB_FINGER_ID, (uint32_t)evs[i].finger_id);
        wb_uint32(&ew, PB_ACTION, (uint32_t)evs[i].action);
        wb_float(&ew, PB_X, evs[i].x);
        wb_float(&ew, PB_Y, evs[i].y);
        wb_bool(&ew, PB_PRIMARY, evs[i].is_primary);
        if (ew.overflow) return -1;
        wb_bytes(&lw, PB_EVENT_LIST, evbuf, ew.len);
        if (lw.overflow) return -1;
    }

    wb_uint32(&cw, PB_CHAN_TYPE, PB_DATA_TYPE_TOUCH);
    wb_bytes(&cw, PB_TOUCH_LIST, listbuf, lw.len);
    if (cw.overflow) return -1;

    msg_len = cw.len;
    total = 10 + head_len + msg_len + 2;
    if (total > cap) return -1;

    out[0] = 0x45;
    out[1] = 0x67;
    out[2] = (unsigned char)((YS_TOUCH_CMD_ID >> 8) & 0xff);
    out[3] = (unsigned char)(YS_TOUCH_CMD_ID & 0xff);
    out[4] = (unsigned char)((head_len >> 8) & 0xff);
    out[5] = (unsigned char)(head_len & 0xff);
    out[6] = (unsigned char)((msg_len >> 24) & 0xff);
    out[7] = (unsigned char)((msg_len >> 16) & 0xff);
    out[8] = (unsigned char)((msg_len >> 8) & 0xff);
    out[9] = (unsigned char)(msg_len & 0xff);
    memcpy(out + 10, chanbuf, (size_t)msg_len);
    out[total - 2] = 0x89;
    out[total - 1] = 0xab;
    return total;
}

/* ---------- finger slot pool ---------- */

void ys_touch_reset(ys_touch_state *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < YS_TOUCH_MAX_FINGERS; i++) {
        st->slot_finger_id[i] = -1;
        st->slot_active[i] = 0;
        st->ident[i] = -1;
    }
    st->primary_finger_id = -1;
}

static int find_by_ident(ys_touch_state *st, int ident) {
    for (int i = 0; i < YS_TOUCH_MAX_FINGERS; i++) {
        if (st->slot_active[i] && st->ident[i] == ident) return i;
    }
    return -1;
}

static int alloc_slot(ys_touch_state *st) {
    for (int fid = 0; fid < YS_TOUCH_MAX_FINGERS; fid++) {
        int used = 0;
        for (int i = 0; i < YS_TOUCH_MAX_FINGERS; i++) {
            if (st->slot_active[i] && st->slot_finger_id[i] == fid) { used = 1; break; }
        }
        if (used) continue;
        for (int i = 0; i < YS_TOUCH_MAX_FINGERS; i++) {
            if (!st->slot_active[i]) {
                st->slot_active[i] = 1;
                st->slot_finger_id[i] = fid;
                st->count++;
                return i;
            }
        }
    }
    return -1;
}

static void free_slot(ys_touch_state *st, int idx) {
    if (idx < 0 || idx >= YS_TOUCH_MAX_FINGERS) return;
    st->slot_active[idx] = 0;
    st->ident[idx] = -1;
    st->slot_finger_id[idx] = -1;
    if (st->count > 0) st->count--;
}

/* Map a normalised coordinate into the float the encoder expects, clamping.
 * Clamping (rather than dropping) matches the game's own behaviour and keeps a
 * drag that leaves the rect from being lost. */
static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

int ys_touch_feed(ys_touch_state *st, int ident, int action, float nx, float ny,
                  unsigned char *out, int cap) {
    ys_touch_event ev;
    int idx;

    if (!st) return -1;
    nx = clamp01(nx);
    ny = clamp01(ny);

    if (action == YS_TOUCH_DOWN) {
        ys_touch_event all[YS_TOUCH_MAX_FINGERS];
        int n = 0, rc;
        if (st->count >= YS_TOUCH_MAX_FINGERS) {
            /* Official SDK releases the oldest finger rather than refusing the
             * new one; the server closes the session on unbalanced finger ids. */
            for (int i = 0; i < YS_TOUCH_MAX_FINGERS; i++) {
                if (!st->slot_active[i]) continue;
                all[n].finger_id = st->slot_finger_id[i];
                all[n].action = YS_TOUCH_UP;
                all[n].x = st->last_x[i];
                all[n].y = st->last_y[i];
                all[n].is_primary = (st->primary_finger_id == st->slot_finger_id[i]);
                n++;
                free_slot(st, i);
                break;
            }
        }
        idx = alloc_slot(st);
        if (idx < 0) return 0;
        st->ident[idx] = ident;
        st->last_x[idx] = nx;
        st->last_y[idx] = ny;
        /* primary belongs to the first finger that goes down, and stays there
         * until every finger is up. Do not recompute it per event. */
        if (st->primary_finger_id < 0) st->primary_finger_id = st->slot_finger_id[idx];
        ev.finger_id = st->slot_finger_id[idx];
        ev.action = YS_TOUCH_DOWN;
        ev.x = nx;
        ev.y = ny;
        ev.is_primary = (st->primary_finger_id == ev.finger_id);
        if (n > 0) {
            all[n++] = ev;
            return ys_touch_build(all, n, out, cap);
        }
        return ys_touch_build(&ev, 1, out, cap);
    }

    idx = find_by_ident(st, ident);
    if (idx < 0) return 0; /* Up/Move for an unknown pointer: ignore silently */

    if (action == YS_TOUCH_UP) {
        ev.finger_id = st->slot_finger_id[idx];
        ev.action = YS_TOUCH_UP;
        ev.x = nx;
        ev.y = ny;
        ev.is_primary = (st->primary_finger_id == ev.finger_id);
        free_slot(st, idx);
        if (st->count == 0) st->primary_finger_id = -1;
        return ys_touch_build(&ev, 1, out, cap);
    }

    /* MOVE */
    st->last_x[idx] = nx;
    st->last_y[idx] = ny;
    ev.finger_id = st->slot_finger_id[idx];
    ev.action = YS_TOUCH_MOVE;
    ev.x = nx;
    ev.y = ny;
    ev.is_primary = (st->primary_finger_id == ev.finger_id);
    return ys_touch_build(&ev, 1, out, cap);
}
