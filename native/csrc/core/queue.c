#include "core/queue.h"

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

void ys_queue_init(ys_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cv, NULL);
    atomic_store(&q->count, 0);
    atomic_store(&q->closed, 0);
}

void ys_queue_destroy(ys_queue_t *q) {
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mtx);
}

int ys_queue_push(ys_queue_t *q, int type, const void *data, int len) {
    unsigned slot;
    if (atomic_load(&q->closed)) return -2;
    if (len < 0 || len > YS_CMD_MAX_PAYLOAD) return -1;
    if (atomic_load(&q->count) >= YS_CMD_RING) return -1;

    /*
     * The producer owns tail, so it can write the slot before taking the lock.
     * The lock only serialises the count/cv update with the consumer; payload
     * copying therefore never happens while the consumer holds the mutex.
     */
    slot = q->tail;
    q->slot[slot].type = type;
    q->slot[slot].len = len;
    if (len > 0 && data) memcpy(q->slot[slot].payload, data, (size_t)len);

    pthread_mutex_lock(&q->mtx);
    q->tail = (q->tail + 1) % YS_CMD_RING;
    atomic_fetch_add(&q->count, 1);
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

int ys_queue_pop(ys_queue_t *q, ys_cmd_t *out, int timeout_ms) {
    int rc = 0;

    pthread_mutex_lock(&q->mtx);
    while (atomic_load(&q->count) == 0 && !atomic_load(&q->closed)) {
        if (timeout_ms <= 0) {
            /* timeout_ms==0 means "block until something happens" */
            pthread_cond_wait(&q->cv, &q->mtx);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            rc = pthread_cond_timedwait(&q->cv, &q->mtx, &ts);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return -1;
            }
        }
    }
    if (atomic_load(&q->count) == 0) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }

    *out = q->slot[q->head];
    q->head = (q->head + 1) % YS_CMD_RING;
    atomic_fetch_sub(&q->count, 1);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

void ys_queue_close(ys_queue_t *q) {
    pthread_mutex_lock(&q->mtx);
    atomic_store(&q->closed, 1);
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mtx);
}

int ys_queue_is_closed(ys_queue_t *q) { return atomic_load(&q->closed); }

unsigned ys_queue_count(ys_queue_t *q) { return atomic_load(&q->count); }
