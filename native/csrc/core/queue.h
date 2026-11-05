#ifndef YS_QUEUE_H
#define YS_QUEUE_H
/*
 * Bounded MPSC command queue: JS thread is the only producer, the session
 * worker is the only consumer.
 *
 * Why bounded: an unbounded queue turns "the pipeline is wedged" into an OOM
 * kill minutes later with no useful diagnostics. Overflow is reported to the
 * caller so the JSAPI layer can surface "busy" instead of silently dropping.
 *
 * Why no malloc in push: push runs on the JS thread, which must stay under
 * ~5ms p99. Fixed-size ring, payload copied into a slot.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#define YS_CMD_MAX_PAYLOAD 2048
#define YS_CMD_RING 64

typedef struct {
    int type;
    int len;
    unsigned char payload[YS_CMD_MAX_PAYLOAD];
} ys_cmd_t;

typedef struct {
    ys_cmd_t slot[YS_CMD_RING];
    unsigned head; /* consumer owns */
    unsigned tail; /* producer owns */
    atomic_uint count;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    atomic_int closed;
} ys_queue_t;

void ys_queue_init(ys_queue_t *q);
void ys_queue_destroy(ys_queue_t *q);
/* returns 0 on success, -1 when full, -2 when closed */
int ys_queue_push(ys_queue_t *q, int type, const void *data, int len);
/* returns 0 on success, -1 on timeout/closed (closed is exposed via ys_queue_is_closed) */
int ys_queue_pop(ys_queue_t *q, ys_cmd_t *out, int timeout_ms);
void ys_queue_close(ys_queue_t *q);
int ys_queue_is_closed(ys_queue_t *q);
unsigned ys_queue_count(ys_queue_t *q);

#endif
