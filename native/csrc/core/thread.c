/* thread.c -- see thread.h for why this exists at all.
 *
 * Implementation notes worth keeping:
 *  - The per-thread cached monotonic clock is refreshed with a plain
 *    clock_gettime(CLOCK_MONOTONIC_COARSE) attempt first: on Linux that is a
 *    vDSO read with no syscall at all (~20 ns on A35). If the kernel does not
 *    offer COARSE we fall back to CLOCK_MONOTONIC. Either way the cache keeps
 *    us from re-entering the vDSO more than once per millisecond per thread.
 *  - The cache stores the raw timeval, not the computed ms, so the value a
 *    caller sees never goes backwards when the cached second rolls over.
 */
#define _GNU_SOURCE
#include "core/thread.h"

#include <errno.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

static __thread uint64_t tl_last_ns;
static __thread uint64_t tl_last_ms;

static uint64_t raw_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return tl_last_ns;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t ys_mono_ms64(void) {
    uint64_t ns = raw_ns();
    /* Refresh at most once per millisecond: 1e6 ns. When the vDSO read is
     * cheap this is almost free, but the guard also means a tight loop calling
     * this 100k times costs one clock read per ms, not 100k. */
    if (ns - tl_last_ns >= 1000000ull) {
        tl_last_ns = ns;
        tl_last_ms = ns / 1000000ull;
    }
    return tl_last_ms;
}

uint32_t ys_mono_ms(void) {
    return (uint32_t)ys_mono_ms64();
}

uint64_t ys_mono_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return ys_mono_ms64() * 1000ull;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

void ys_sleep_ms(int ms) {
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    /* clock_nanosleep with TIMER_ABSTIME would be more precise, but a plain
     * relative sleep on the monotonic clock is already immune to wall-clock
     * steps, and it keeps the call site trivial. EINTR is not retried: every
     * caller in this project treats an early return as "check the stop flag",
     * which is exactly what we want when a signal arrives. */
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

void ys_thread_set_name(const char *name) {
    if (!name || !*name) return;
    prctl(PR_SET_NAME, name, 0, 0, 0);
}

int ys_thread_spawn(pthread_t *out, const char *name, void *(*fn)(void *),
                    void *arg, size_t stack_bytes) {
    pthread_attr_t attr;
    int rc;

    if (!out || !fn) return -1;
    if (stack_bytes == 0) stack_bytes = 256u * 1024u;

    if (pthread_attr_init(&attr) != 0) return -1;
    /* The worker thread runs GStreamer pushes and a DTLS handshake; 256 KiB is
     * comfortable and the default 8 MiB is wasteful on a 288 MB budget. */
    if (stack_bytes < 64u * 1024u) stack_bytes = 64u * 1024u;
    pthread_attr_setstacksize(&attr, stack_bytes);

    rc = pthread_create(out, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    /* Naming has to happen inside the thread (PR_SET_NAME is per-thread) but
     * the creator cannot reach in, so name it here as a best-effort: on Linux
     * pthread_setname_np on another thread works. */
    if (name && *name) {
        char buf[16];
        strncpy(buf, name, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        pthread_setname_np(*out, buf);
    }
    return 0;
}
