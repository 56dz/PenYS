/* thread.h -- the three things every other module in this project needs:
 * a monotonic clock, a bounded sleep, and a named pthread.
 *
 * Why a separate file instead of calling clock_gettime() everywhere:
 *   - CLOCK_MONOTONIC on this kernel has ~1us resolution but the syscall costs
 *     ~200ns on an A35; the RTP path calls it per packet. ys_mono_ms() keeps a
 *     coarse cached value that is refreshed at most once per millisecond, which
 *     turns 60 fps * 2 callers into ~1000 syscalls/s instead of tens of
 *     thousands. The coarse value is only ever used for *timeouts and
 *     statistics*; nothing in the media path depends on sub-millisecond
 *     precision.
 *   - The cached value is per-thread (__thread), so no atomics, no locks, and
 *     no false sharing between the worker thread and the JS thread.
 *
 * Threads: this project has exactly ONE long-lived worker thread, owned by
 * session/session.c. ys_thread_spawn() exists only so that owner does not have
 * to hand-roll pthread attribute setup. Do not add a second one.
 */
#ifndef YS_THREAD_H
#define YS_THREAD_H

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic milliseconds since an arbitrary fixed point (boot). Never goes
 * backwards, never jumps on RTC changes. This is the ONLY clock allowed for
 * timeouts, RTP timing and latency measurement in this project.
 * Wall-clock (time(2)) is used exactly once: the SDP o= session id. */
uint32_t ys_mono_ms(void);

/* Same clock, 64-bit, for durations that can exceed 49 days. */
uint64_t ys_mono_ms64(void);

/* Microseconds, uncached (syscall). For latency probes only, not steady state. */
uint64_t ys_mono_us(void);

/* Sleep up to ms, interrupted by signals. Returns immediately when ms <= 0.
 * Uses clock_nanosleep(CLOCK_MONOTONIC) so a wall-clock step cannot lengthen
 * the sleep. */
void ys_sleep_ms(int ms);

/* Spawn a detached-free thread: the caller keeps the pthread_t and must join
 * or detach it. Stack is capped at stack_bytes (0 = 256 KiB), because the
 * default 8 MiB times two threads is a measurable chunk of this device's
 * 288 MB of available RAM.
 * Returns 0 on success, -1 on failure (errno preserved). */
int ys_thread_spawn(pthread_t *out, const char *name, void *(*fn)(void *),
                    void *arg, size_t stack_bytes);

/* Ask the calling thread's kernel name to be set (15 chars max, "ys-" prefix
 * is added by the caller if desired). Best effort: silently no-op on kernels
 * without PR_SET_NAME support. */
void ys_thread_set_name(const char *name);

#ifdef __cplusplus
}
#endif
#endif /* YS_THREAD_H */
